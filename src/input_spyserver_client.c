/**
 * @file input_spyserver_client.c
 * @brief Implements the input source for connecting to a SpyServer over the network.
 *
 * This module acts as a network client for the spyserver protocol. It handles
 * TCP connection, handshaking, device configuration, and parsing of the I/Q
 * data stream. The logic is heavily based on the reference implementation
 * found in the SDR++ project.
 */

/*  input_spyserver_client - SpyServer network client for iq_tool
 *
 *  This file is part of iq_tool.
 *
 *  The network protocol and data handling logic in this file is heavily based
 *  on the SpyServer source module from the SDR++ project.
 *  SDR++ is Copyright (C) 2020-2023 Alexandre Rouma <alexandre.rouma@gmail.com>
 *  and is licensed under the GPLv2.0-or-later.
 *
 *  Copyright (C) 2025 iq_tool
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "input_spyserver_client.h"
#include "module.h"
#include "constants.h"
#include "module_defaults.h"
#include "log.h"
#include "app_context.h"
#include "input_common.h"
#include "signal_handler.h"
#include "queue.h"
#include "argparse.h"
#include "mem_arena.h"
#include "ring_buffer.h"
#include "utilities.h"
#include "sample_format_table.h"
#include "networking.h"
#include "platform.h"
#include "packet_serializer.h"
#include "sample_format_table.h" // Required for standardized packet format
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>

// --- Platform-Specific Includes ---
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

// =============================================================================
// == START: Encapsulated SpyServer Protocol Definitions
// =============================================================================

#define SPYSERVER_PROTOCOL_VERSION (((uint32_t)2 << 24) | ((uint32_t)0 << 16) | (1700))

#pragma pack(push, 1)

typedef enum {
    SPYSERVER_CMD_HELLO = 0,
    SPYSERVER_CMD_SET_SETTING = 2,
} SpyServerCommandType;

typedef enum {
    SPYSERVER_SETTING_STREAMING_MODE = 0,
    SPYSERVER_SETTING_STREAMING_ENABLED = 1,
    SPYSERVER_SETTING_GAIN = 2,
    SPYSERVER_SETTING_IQ_FORMAT = 100,
    SPYSERVER_SETTING_IQ_FREQUENCY = 101,
    SPYSERVER_SETTING_IQ_DECIMATION = 102,
    SPYSERVER_SETTING_IQ_DIGITAL_GAIN = 103,
} SpyServerSettingType;

typedef enum {
    SPYSERVER_STREAM_MODE_IQ_ONLY = 1,
} SpyServerStreamingMode;

typedef enum {
    SPYSERVER_STREAM_FORMAT_INVALID = 0,
    SPYSERVER_STREAM_FORMAT_UINT8 = 1,
    SPYSERVER_STREAM_FORMAT_INT16 = 2,
    SPYSERVER_STREAM_FORMAT_INT24 = 3,
    SPYSERVER_STREAM_FORMAT_FLOAT = 4,
} SpyServerStreamFormat;

typedef enum {
    SPYSERVER_DEV_INVALID = 0,
    SPYSERVER_DEV_AIRSPY_ONE = 1,
    SPYSERVER_DEV_AIRSPY_HF = 2,
    SPYSERVER_DEV_RTLSDR = 3,
} SpyServerDeviceType;

typedef enum {
    SPYSERVER_MSG_TYPE_DEVICE_INFO = 0,
    SPYSERVER_MSG_TYPE_CLIENT_SYNC = 1,
    SPYSERVER_MSG_TYPE_UINT8_IQ = 100,
    SPYSERVER_MSG_TYPE_INT16_IQ = 101,
    SPYSERVER_MSG_TYPE_INT24_IQ = 102,
    SPYSERVER_MSG_TYPE_FLOAT_IQ = 103,
} SpyServerMessageType;

typedef struct {
    uint32_t CommandType;
    uint32_t BodySize;
} SpyServerCommandHeader;

typedef struct {
    uint32_t ProtocolVersion;
    char ClientName[16];
} SpyServerClientHandshake;

typedef struct {
    uint32_t Setting;
    uint32_t Value;
} SpyServerSettingTarget;

typedef struct {
    uint32_t ProtocolID;
    uint32_t MessageType;
    uint32_t StreamType;
    uint32_t SequenceNumber;
    uint32_t BodySize;
} SpyServerMessageHeader;

typedef struct {
    uint32_t DeviceType;
    uint32_t DeviceSerial;
    uint32_t MaximumSampleRate;
    uint32_t MaximumBandwidth;
    uint32_t DecimationStageCount;
    uint32_t GainStageCount;
    uint32_t MaximumGainIndex;
    uint32_t MinimumFrequency;
    uint32_t MaximumFrequency;
    uint32_t Resolution;
    uint32_t MinimumIQDecimation;
    uint32_t ForcedIQFormat;
} SpyServerDeviceInfo;

typedef struct {
    uint32_t CanControl;
    uint32_t Gain;
    uint32_t DeviceCenterFrequency;
    uint32_t IQCenterFrequency;
    uint32_t FFTCenterFrequency;
    uint32_t MinimumIQCenterFrequency;
    uint32_t MaximumIQCenterFrequency;
    uint32_t MinimumFFTCenterFrequency;
    uint32_t MaximumFFTCenterFrequency;
} SpyServerClientSync;

#pragma pack(pop)

// =============================================================================
// == END: Encapsulated SpyServer Protocol Definitions
// =============================================================================

// --- Private Module Configuration ---
static struct {
    const char* hostname;
    int port;
    int gain;
    bool gain_provided;
    const char* sample_format_str;
} s_spyserver_client_config;

// --- Private Module State ---
typedef struct {
    NetworkingContext* net_context;
    SpyServerDeviceInfo device_info;
    bool device_info_ok;
    SampleFormat active_format;
    RingBuffer* stream_buffer;

    // Scratch buffer for reading network payloads before stripping headers
    unsigned char* rx_buffer;
    size_t rx_buffer_size;
} SpyServerClientContext;

// --- CLI Options ---
static const struct argparse_option input_spyserver_client_cli_options[] = {
    OPT_GROUP("SpyServer Client Input (spyserver-client)"),
    OPT_STRING(0, "spyserver-client-host", &s_spyserver_client_config.hostname, "Hostname or IP of the spyserver instance (Required).", NULL, 0, 0),
    OPT_INTEGER(0, "spyserver-client-port", &s_spyserver_client_config.port, "Port number of the spyserver instance (Required).", NULL, 0, 0),
    OPT_INTEGER(0, "spyserver-client-gain", &s_spyserver_client_config.gain, "Set manual gain. Disables AGC. (Ignored on servers without gain control)", NULL, 0, 0),
    OPT_STRING(0, "spyserver-client-sample-format", &s_spyserver_client_config.sample_format_str, "Select sample format {cu8|cs16|cs24|cf32}. Default is cu8.", NULL, 0, 0),
};

const struct argparse_option* input_spyserver_client_get_cli_options(int* count) {
    *count = sizeof(input_spyserver_client_cli_options) / sizeof(input_spyserver_client_cli_options[0]);
    return input_spyserver_client_cli_options;
}

// --- Default Configuration ---
void input_spyserver_client_set_default_config(struct AppConfig* config) {
    config->sdr_general.sample_rate_hz = SPYSERVER_DEFAULT_SAMPLE_RATE_HZ;
    s_spyserver_client_config.hostname = NULL;
    s_spyserver_client_config.port = 0;
    s_spyserver_client_config.gain = -1;
    s_spyserver_client_config.gain_provided = false;
    s_spyserver_client_config.sample_format_str = "cu8";
}

// --- Function Prototypes ---
static void* input_spyserver_client_producer_thread(void* arg);
static void input_spyserver_client_get_summary_info(const ModuleContext* context, InputSummaryInfo* info);
static bool input_spyserver_client_validate_options(AppContext* app);

// --- The InputModuleInterface V-Table ---

// --- Helper Functions for Protocol and Logic ---

typedef struct {
    SampleFormat internal_fmt;
    int spyserver_fmt;
} FormatMap;

static const FormatMap format_map[] = {
    { CU8,  SPYSERVER_STREAM_FORMAT_UINT8 },
    { CS16, SPYSERVER_STREAM_FORMAT_INT16 },
    { CS24, SPYSERVER_STREAM_FORMAT_INT24 },
    { CF32, SPYSERVER_STREAM_FORMAT_FLOAT },
};

static int get_spyserver_enum_from_internal_format(SampleFormat fmt) {
    for (size_t i = 0; i < sizeof(format_map) / sizeof(format_map[0]); i++) {
        if (format_map[i].internal_fmt == fmt) {
            return format_map[i].spyserver_fmt;
        }
    }
    return SPYSERVER_STREAM_FORMAT_INVALID;
}

static SampleFormat get_internal_format_from_spyserver_enum(int spyserver_format) {
    for (size_t i = 0; i < sizeof(format_map) / sizeof(format_map[0]); i++) {
        if (format_map[i].spyserver_fmt == spyserver_format) {
            return format_map[i].internal_fmt;
        }
    }
    return FORMAT_UNKNOWN;
}

static bool send_setting(SpyServerClientContext* client, uint32_t setting, uint32_t value) {
    unsigned char command_buffer[sizeof(SpyServerCommandHeader) + sizeof(SpyServerSettingTarget)];

    SpyServerCommandHeader* header = (SpyServerCommandHeader*)command_buffer;
    header->CommandType = SPYSERVER_CMD_SET_SETTING;
    header->BodySize = sizeof(SpyServerSettingTarget);

    SpyServerSettingTarget* payload = (SpyServerSettingTarget*)(command_buffer + sizeof(SpyServerCommandHeader));
    payload->Setting = setting;
    payload->Value = value;

    return networking_send_all(client->net_context, command_buffer, sizeof(command_buffer));
}

// --- Validation Function ---
static bool input_spyserver_client_validate_options(AppContext* app) {
    AppConfig* config = app ? (AppConfig*)app->config : NULL;
    (void)config;
    if (s_spyserver_client_config.hostname == NULL) {
        log_error("Missing required argument: --spyserver-client-host <address>");
        return false;
    }
    if (s_spyserver_client_config.port == 0) {
        log_error("Missing required argument: --spyserver-client-port <number>");
        return false;
    }

    if (s_spyserver_client_config.gain != -1) {
        s_spyserver_client_config.gain_provided = true;
    }

    if (s_spyserver_client_config.sample_format_str != NULL) {
        if (strcasecmp(s_spyserver_client_config.sample_format_str, "cu8") != 0 &&
            strcasecmp(s_spyserver_client_config.sample_format_str, "cs16") != 0 &&
            strcasecmp(s_spyserver_client_config.sample_format_str, "cs24") != 0 &&
            strcasecmp(s_spyserver_client_config.sample_format_str, "cf32") != 0) {
            log_error("Invalid value for --spyserver-client-sample-format: '%s'. Must be one of {cu8|cs16|cs24|cf32}.", s_spyserver_client_config.sample_format_str);
            return false;
        }
    }
    return true;
}

static bool discard_network_bytes(NetworkingContext* net_context, size_t bytes_to_discard) {
    char discard_buffer[1024];
    while (bytes_to_discard > 0) {
        size_t to_read = (bytes_to_discard > sizeof(discard_buffer)) ? sizeof(discard_buffer) : bytes_to_discard;
        if (!networking_recv_all(net_context, discard_buffer, to_read)) {
            return false;
        }
        bytes_to_discard -= to_read;
    }
    return true;
}

static bool input_spyserver_client_initialize(ModuleContext* context) {
    AppConfig* config = (AppConfig*)context->config;
    AppContext* app = context->app;

    SpyServerClientContext* client = (SpyServerClientContext*)mem_arena_alloc(&app->pipeline.setup_arena, sizeof(SpyServerClientContext), true);
    if (!client) return false;
    app->module.input_private_data = client;
    client->net_context = NULL;

    // Allocate receive scratch buffer
    client->rx_buffer_size = 16384; // 16KB sweet-spot for low-latency flow
    client->rx_buffer = (unsigned char*)mem_arena_alloc(&app->pipeline.setup_arena, client->rx_buffer_size, false);
    if (!client->rx_buffer) return false;

    // This module now takes responsibility for initializing its dependency.
    if (!networking_init()) {
        log_error("SpyServer client failed because the networking module could not be initialized.");
        return false;
    }

    client->net_context = networking_connect(s_spyserver_client_config.hostname, s_spyserver_client_config.port, &app->pipeline.setup_arena);
    if (!client->net_context) {
        networking_cleanup(); // Release our reference on failure.
        return false;
    }

    log_info("Performing SpyServer protocol handshake...");

    char user_agent[128];
    snprintf(user_agent, sizeof(user_agent), "%s version %s", APP_NAME, GIT_HASH);

    size_t user_agent_length = strlen(user_agent);
    size_t payload_size = sizeof(uint32_t) + user_agent_length;

    unsigned char* payload_buffer = (unsigned char*)mem_arena_alloc(&app->pipeline.setup_arena, payload_size, false);
    if (!payload_buffer) {
        goto error_cleanup;
    }

    uint32_t protocol_version = SPYSERVER_PROTOCOL_VERSION;
    memcpy(payload_buffer, &protocol_version, sizeof(uint32_t));
    memcpy(payload_buffer + sizeof(uint32_t), user_agent, user_agent_length);

    SpyServerCommandHeader hello_header;
    hello_header.CommandType = SPYSERVER_CMD_HELLO;
    hello_header.BodySize = payload_size;

    bool send_ok = networking_send_all(client->net_context, &hello_header, sizeof(hello_header)) &&
                   networking_send_all(client->net_context, payload_buffer, payload_size);

    if (!send_ok) {
        goto error_cleanup;
    }

    SpyServerMessageHeader response_header;
    if (!networking_recv_all(client->net_context, &response_header, sizeof(response_header))) {
        goto error_cleanup;
    }

    if (response_header.MessageType != SPYSERVER_MSG_TYPE_DEVICE_INFO) {
        log_error("Did not receive DeviceInfo after handshake. Server may have rejected the connection (MessageType=%u).", response_header.MessageType);
        goto error_cleanup;
    }
    if (response_header.BodySize < sizeof(SpyServerDeviceInfo)) {
        log_error("Received DeviceInfo with unexpected size (%u vs %zu).", response_header.BodySize, sizeof(SpyServerDeviceInfo));
        goto error_cleanup;
    }
    if (!networking_recv_all(client->net_context, &client->device_info, sizeof(SpyServerDeviceInfo))) {
        goto error_cleanup;
    }
    size_t dev_extra_bytes = response_header.BodySize - sizeof(SpyServerDeviceInfo);
    if (dev_extra_bytes > 0) {
        if (!discard_network_bytes(client->net_context, dev_extra_bytes)) {
            goto error_cleanup;
        }
    }
    client->device_info_ok = true;

    log_info("Handshake complete. Waiting for client sync message...");
    bool got_sync = false;
    int max_ignored = 5;
    int ignored = 0;
    SpyServerClientSync sync_info;
    while (!got_sync && ignored < max_ignored) {
        if (!networking_recv_all(client->net_context, &response_header, sizeof(response_header))) {
            goto error_cleanup;
        }
        if (response_header.MessageType == SPYSERVER_MSG_TYPE_CLIENT_SYNC) {
            if (response_header.BodySize < sizeof(SpyServerClientSync)) {
                log_error("Received ClientSync with unexpected size (%u vs %zu). Protocol mismatch.", response_header.BodySize, sizeof(SpyServerClientSync));
                goto error_cleanup;
            }
            if (!networking_recv_all(client->net_context, &sync_info, sizeof(SpyServerClientSync))) {
                goto error_cleanup;
            }
            size_t sync_extra = response_header.BodySize - sizeof(SpyServerClientSync);
            if (sync_extra > 0) {
                if (!discard_network_bytes(client->net_context, sync_extra)) {
                    goto error_cleanup;
                }
            }
            got_sync = true;
        } else {
            size_t extra = response_header.BodySize;
            if (extra > 0) {
                if (!discard_network_bytes(client->net_context, extra)) {
                    goto error_cleanup;
                }
            }
            ignored++;
        }
    }
    if (!got_sync) {
        log_error("Did not receive ClientSync message.");
        goto error_cleanup;
    }

    if (sync_info.CanControl == 0) {
        log_error("Cannot control the remote device. Another client has control.");
        goto error_cleanup;
    }

    log_info("Client has control of the remote device. Negotiating stream parameters...");

    // Determine the format our client wants to request based on user args or defaults.
    const SampleFormatInfo* req_fmt_info = get_format_info_by_name(s_spyserver_client_config.sample_format_str);
    SampleFormat requested_format = req_fmt_info ? req_fmt_info->format_enum : FORMAT_UNKNOWN;
    log_info("Client requesting sample format: %s", req_fmt_info ? req_fmt_info->description_str : "Unknown");

    // Assume our request will be honored unless the server says otherwise.
    SampleFormat final_format = requested_format;

    // Check if the server is forcing a specific format.
    uint32_t forced_format_enum = client->device_info.ForcedIQFormat;
    if (forced_format_enum != 0) {
        SampleFormat server_forced_format = get_internal_format_from_spyserver_enum(forced_format_enum);

        // Only warn and switch if the server's required format is valid and
        // DIFFERENT from what we were going to request.
        if (server_forced_format != FORMAT_UNKNOWN && server_forced_format != requested_format) {
            log_warn("Server requires the %s sample format. Switching...",
                     get_format_info_by_enum(server_forced_format) ? get_format_info_by_enum(server_forced_format)->description_str : "Unknown");
            // Override our choice with the server's required format.
            final_format = server_forced_format;
        }
    }

    // Set the final, negotiated format for use by the rest of the application.
    client->active_format = final_format;
    app->module.input_format = final_format;
    app->module.input_bytes_per_iq_sample = get_bytes_per_iq_sample(final_format);

    uint32_t max_sr = client->device_info.MaximumSampleRate;
    uint32_t min_dec = client->device_info.MinimumIQDecimation;
    uint32_t dec_count = client->device_info.DecimationStageCount;
    double supported_rates[32];
    int num_supported_rates = 0;
    for (uint32_t i = min_dec; i <= dec_count && num_supported_rates < 32; i++) {
        supported_rates[num_supported_rates++] = (double)max_sr / (double)(1 << i);
    }

    double user_rate = config->sdr_general.sample_rate_hz > 0 ? config->sdr_general.sample_rate_hz : supported_rates[0];
    int best_rate_idx = 0;
    double min_diff = fabs(supported_rates[0] - user_rate);
    for (int i = 1; i < num_supported_rates; i++) {
        double diff = fabs(supported_rates[i] - user_rate);
        if (diff < min_diff) {
            min_diff = diff;
            best_rate_idx = i;
        }
    }

    double actual_rate = supported_rates[best_rate_idx];
    uint32_t dec_index_to_send = min_dec + best_rate_idx;
    if (min_diff < 1.0) {
        log_info("Using requested sample rate: %.15g Hz.", actual_rate);
    } else {
        log_info("Requested sample rate %.15g Hz. Using closest available rate: %.15g Hz.", user_rate, actual_rate);
    }

    app->module.source_info.sample_rate = (int)actual_rate;

    int format_to_request_int = get_spyserver_enum_from_internal_format(final_format);

    log_info("Configuring remote device...");
    if (!send_setting(client, SPYSERVER_SETTING_IQ_FREQUENCY, (uint32_t)config->sdr_general.rf_freq_hz)) goto error_cleanup;
    if (!send_setting(client, SPYSERVER_SETTING_IQ_DECIMATION, dec_index_to_send)) goto error_cleanup;
    if (!send_setting(client, SPYSERVER_SETTING_IQ_FORMAT, format_to_request_int)) goto error_cleanup;

    uint32_t effective_gain_index = 0;

    if (s_spyserver_client_config.gain_provided) {
        // Strictly check the protocol: Does the device support a gain index range?
        if (client->device_info.MaximumGainIndex > 0) {
            uint32_t requested_gain = (uint32_t)s_spyserver_client_config.gain;

            // Fail outright if the user requests a gain value that does not exist
            if (requested_gain > client->device_info.MaximumGainIndex) {
                log_error("Requested gain value %u is out of range.", requested_gain);
                log_error("Valid gain range for this server is 0 to %u.", client->device_info.MaximumGainIndex);

                goto error_cleanup;
            }

            if (!send_setting(client, SPYSERVER_SETTING_GAIN, requested_gain)) {
                goto error_cleanup;
            }

            // Store the successfully applied gain for the digital gain calculation below
            effective_gain_index = requested_gain;

        } else {
            // If the server simply doesn't support gain (e.g. Airspy HF+ or fixed config), warn but proceed.
            log_warn("Manual gain requested, but not supported by this server. Ignoring.");
            // effective_gain_index remains 0
        }
    }

    float digital_gain_float = 0.0f;
    uint32_t device_type = client->device_info.DeviceType;

    if (device_type == SPYSERVER_DEV_AIRSPY_ONE) {
        // Airspy One specific formula requires the Gain Index we just determined/validated
        digital_gain_float = (float)(client->device_info.MaximumGainIndex - effective_gain_index) + ((float)dec_index_to_send * 3.01f);
    } else {
        // Airspy HF+, RTL-SDR, and others do NOT use the Gain Index in this calculation
        digital_gain_float = (float)dec_index_to_send * 3.01f;
    }

    if (!send_setting(client, SPYSERVER_SETTING_IQ_DIGITAL_GAIN, (uint32_t)digital_gain_float)) goto error_cleanup;

    if (!send_setting(client, SPYSERVER_SETTING_STREAMING_MODE, SPYSERVER_STREAM_MODE_IQ_ONLY)) goto error_cleanup;

    // Dynamic Ring Buffer Sizing
    double bytes_per_sec = (double)actual_rate * (double)app->module.input_bytes_per_iq_sample;

    // Calculate total capacity based on the pre-buffer target and the headroom factor.
    // e.g. 2.5s * 4.0 = 10.0s of total capacity.
    size_t desired_buffer_size = (size_t)(bytes_per_sec * SPYSERVER_PREBUFFER_TARGET_SECONDS * SPYSERVER_BUFFER_HEADROOM_FACTOR);

    // Sanity Clamp: Minimum 1MB
    if (desired_buffer_size < SPYSERVER_RING_BUFFER_MIN_BYTES) desired_buffer_size = SPYSERVER_RING_BUFFER_MIN_BYTES;

    // Hard Ceiling: Never exceed the absolute max defined in constants.h
    if (desired_buffer_size > SPYSERVER_MAX_BUFFER_BYTES) desired_buffer_size = SPYSERVER_MAX_BUFFER_BYTES;

    log_info("SpyServer Ring Buffer: Allocating %zu bytes (%.2f sec capacity) for %.2f sec pre-buffer target.",
             desired_buffer_size,
             (double)desired_buffer_size / bytes_per_sec,
             SPYSERVER_PREBUFFER_TARGET_SECONDS);

    client->stream_buffer = ring_buffer_create(desired_buffer_size, &app->pipeline.setup_arena);
    if (!client->stream_buffer) {
        goto error_cleanup;
    }

    return true;

error_cleanup:
    if (client && client->net_context) {
        networking_disconnect(client->net_context);
    }
    networking_cleanup();
    return false;
}

static void* input_spyserver_client_producer_thread(void* arg) {
    platform_set_thread_priority(PRIORITY_REALTIME, "SpyServer Producer");

    ModuleContext* context = (ModuleContext*)arg;
    AppContext* app = context->app;
    SpyServerClientContext* client = (SpyServerClientContext*)app->module.input_private_data;

    while (!is_shutdown_requested()) {
        SpyServerMessageHeader header;

        // Block waiting for a protocol header
        if (!networking_recv_all(client->net_context, &header, sizeof(header))) {
            if (!is_shutdown_requested()) {
                 handle_fatal_thread_error("Connection to spyserver lost (header recv failed).", app);
            }
            break;
        }

        uint32_t msg_type = header.MessageType & 0xFFFF;
        uint32_t body_size = header.BodySize;

        // Is this an I/Q data packet?
        bool is_iq_data = (msg_type >= SPYSERVER_MSG_TYPE_UINT8_IQ && msg_type <= SPYSERVER_MSG_TYPE_FLOAT_IQ);

        if (is_iq_data && body_size > 0) {
            size_t bytes_remaining = body_size;
            size_t bpp = get_bytes_per_iq_sample(client->active_format);
            if (bpp == 0) bpp = 1;

            while (bytes_remaining > 0 && !is_shutdown_requested()) {
                // Determine how much to read in this chunk (limited by scratch buffer size)
                size_t chunk_size = (bytes_remaining > client->rx_buffer_size) ? client->rx_buffer_size : bytes_remaining;

                // Align read size to sample boundary
                size_t aligned_read = (chunk_size / bpp) * bpp;

                if (aligned_read == 0) {
                    // We have fewer bytes left than a single sample requires (e.g. 1 byte left for 2-byte format).
                    // We MUST consume these from the socket or the next header read will be desynchronized.
                    // We use bytes_remaining here because aligned_read is 0.
                    if (!networking_recv_all(client->net_context, client->rx_buffer, bytes_remaining)) {
                        if (!is_shutdown_requested()) handle_fatal_thread_error("Connection lost draining leftovers.", app);
                        goto end_loop;
                    }
                    // Discard them (do nothing with rx_buffer) and exit inner loop
                    bytes_remaining = 0;
                    break;
                }

                // 1. Read Payload Chunk from Network
                if (!networking_recv_all(client->net_context, client->rx_buffer, aligned_read)) {
                    if (!is_shutdown_requested()) handle_fatal_thread_error("Connection lost reading payload.", app);
                    goto end_loop;
                }

                // 2. Wrap and Write to Ring Buffer
                uint32_t samples_in_chunk = (uint32_t)(aligned_read / bpp);
                if (!packet_serializer_write_packet(client->stream_buffer, samples_in_chunk, client->rx_buffer, client->active_format)) {
                    static double last_drop_log_time = 0.0;
                    static size_t accumulated_drops = 0;

                    accumulated_drops += samples_in_chunk;
                    double current_time = utility_get_time();

                    if (current_time - last_drop_log_time >= CONSOLE_UPDATE_INTERVAL_SEC) {
                        log_warn("SpyServer: Ring buffer full! Dropped %zu samples.", accumulated_drops);
                        accumulated_drops = 0;
                        last_drop_log_time = current_time;
                    }
                }

                bytes_remaining -= aligned_read;
            }
        }
        else {
            // --- METADATA / INFO / SYNC ---
            // We discard these packets to prevent them from entering the processing pipeline.
            if (body_size > 0) {
                if (!discard_network_bytes(client->net_context, body_size)) {
                    if (!is_shutdown_requested()) handle_fatal_thread_error("Connection lost discarding packet.", app);
                    goto end_loop;
                }
            }
        }
    }

end_loop:;
    ring_buffer_signal_end_of_stream(client->stream_buffer);
    log_debug("SpyServer producer thread is exiting.");
    return NULL;
}

static void* input_spyserver_client_push_samples_to_queue(ModuleContext* context, QueueSamples queue_samples, void* pipeline_context) {
    context->app->module.queue_samples = queue_samples;
    context->app->module.pipeline_context = pipeline_context;
    AppContext* app = context->app;
    SpyServerClientContext* client = (SpyServerClientContext*)app->module.input_private_data;

    if (!send_setting(client, SPYSERVER_SETTING_STREAMING_ENABLED, 1)) {
        handle_fatal_thread_error("Failed to start spyserver stream.", app);
        return NULL;
    }

    pthread_t producer_thread_id;
    if (pthread_create(&producer_thread_id, NULL, input_spyserver_client_producer_thread, context) != 0) {
        handle_fatal_thread_error("Failed to create spyserver producer thread.", app);
        return NULL;
    }

    size_t buffer_capacity = ring_buffer_get_capacity(client->stream_buffer);
    double bytes_per_second = (double)app->module.source_info.sample_rate * (double)app->module.input_bytes_per_iq_sample;
    size_t high_water_mark = (size_t)(bytes_per_second * SPYSERVER_PREBUFFER_TARGET_SECONDS);

    // Sanity Cap
    size_t max_safe_mark = (size_t)(buffer_capacity * SPYSERVER_PREBUFFER_MAX_FILL_RATIO);
    if (high_water_mark > max_safe_mark) high_water_mark = max_safe_mark;
    if (high_water_mark < SPYSERVER_PREBUFFER_MIN_BYTES) high_water_mark = SPYSERVER_PREBUFFER_MIN_BYTES;

    log_info("Pre-buffering SpyServer data...");

    while (!is_shutdown_requested() && ring_buffer_get_size(client->stream_buffer) < high_water_mark) {
        if (app->stats.error_occurred) break;
        #ifdef _WIN32
            Sleep(100);
        #else
            usleep(100000);
        #endif
    }

    if (is_shutdown_requested() || app->stats.error_occurred) {
        log_warn("Shutdown requested during pre-buffering phase.");
    } else {
        log_info("Pre-buffering complete.");
    }

    SerializerState state;
    memset(&state, 0, sizeof(state));

    while (!is_shutdown_requested()) {
        SampleChunk* item = (SampleChunk*)queue_dequeue(app->pipeline.free_sample_chunk_queue);
        if (!item) break;

        bool is_reset = false;

        // Read clean data from the ring buffer into the sample chunk
        int64_t frames_read = packet_serializer_read_packet(
            client->stream_buffer,
            item,
            &state,
            &is_reset,
            app->pipeline.read_chunk_size
        );

        if (frames_read < 0) {
            handle_fatal_thread_error("SpyServer Client: Fatal error parsing internal buffer stream.", app);
            queue_enqueue(app->pipeline.free_sample_chunk_queue, item);
            break;
        }

        if (frames_read == 0 && !is_reset) {
            // End of Stream (Producer closed buffer)
            item->is_last_chunk = true;
            item->frames_read = 0;
            queue_enqueue(app->pipeline.reader_output_queue, item);
            break;
        }

        item->frames_read = frames_read;
        item->frames_to_write = (unsigned int)frames_read;
        item->stream_discontinuity_event = is_reset;
        item->is_last_chunk = false;

        // Packet format is set by the serializer based on the header it read
        // But for extra safety, we ensure byte size matches
        const SampleFormatInfo* pkt_fmt_info = get_format_info_by_enum(item->packet_sample_format);
        item->input_bytes_per_iq_sample = pkt_fmt_info ? pkt_fmt_info->bytes_per_iq_sample : 0;

        if (item->frames_read > 0) {
            atomic_fetch_add_explicit(&app->stats.total_frames_read, item->frames_read, memory_order_relaxed);
        }

        if (!queue_enqueue(app->pipeline.reader_output_queue, item)) {
            queue_enqueue(app->pipeline.free_sample_chunk_queue, item);
            break;
        }
    }

    if (!is_shutdown_requested()) {
        request_shutdown();
    }
    pthread_join(producer_thread_id, NULL);

    log_debug("SpyServer Client stream thread is exiting.");
    return NULL;
}

static void input_spyserver_client_stop_sample_queue_push(ModuleContext* context) {
    AppContext* app = context->app;
    if (app->module.input_private_data) {
        SpyServerClientContext* client = (SpyServerClientContext*)app->module.input_private_data;
        if (client->stream_buffer) {
            ring_buffer_signal_shutdown(client->stream_buffer);
        }
    }
}

static void input_spyserver_client_cleanup(ModuleContext* context) {
    AppContext* app = context->app;
    if (app->module.input_private_data) {
        SpyServerClientContext* client = (SpyServerClientContext*)app->module.input_private_data;
        if (client->stream_buffer) {
            ring_buffer_destroy(client->stream_buffer);
            client->stream_buffer = NULL;
        }
        if (client->net_context) {
            networking_disconnect(client->net_context);
            client->net_context = NULL;
        }
        // rx_buffer is in arena, no free needed
        networking_cleanup();
    }
}

static void input_spyserver_client_get_summary_info(const ModuleContext* context, InputSummaryInfo* info) {
    const SpyServerClientContext* client = (const SpyServerClientContext*)context->app->module.input_private_data;
    const AppContext* app = context->app;
    char server_addr[256];
    snprintf(server_addr, sizeof(server_addr), "%s:%d", s_spyserver_client_config.hostname, s_spyserver_client_config.port);
    utility_add_summary_item(info, "Input Source", "SpyServer Client");
    utility_add_summary_item(info, "Server Address", server_addr);

    if (client && client->device_info_ok) {
        const char* dev_type_str = "Unknown";
        switch (client->device_info.DeviceType) {
            case SPYSERVER_DEV_AIRSPY_ONE: dev_type_str = "Airspy One"; break;
            case SPYSERVER_DEV_AIRSPY_HF:  dev_type_str = "Airspy HF+"; break;
            case SPYSERVER_DEV_RTLSDR:     dev_type_str = "RTL-SDR"; break;
        }
        char dev_info_str[128];
        snprintf(dev_info_str, sizeof(dev_info_str), "%s (S/N: %08X)", dev_type_str, client->device_info.DeviceSerial);
        utility_add_summary_item(info, "Remote Device", dev_info_str);
        utility_add_summary_item(info, "Input Format", get_format_info_by_enum(app->module.input_format) ? get_format_info_by_enum(app->module.input_format)->description_str : "Unknown");
        utility_add_summary_item(info, "Input Sample Rate", "%.15g Hz", (double)app->module.source_info.sample_rate);

        if (s_spyserver_client_config.gain_provided) {
            utility_add_summary_item(info, "Gain", "%d (Manual)", s_spyserver_client_config.gain);
        } else {
            utility_add_summary_item(info, "Gain", "Automatic (AGC)");
        }
    }
}

// --- The InputModuleInterface V-Table ---
static InputModuleInterface s_input_spyserver_client_api = {
    .initialize = input_spyserver_client_initialize,
    .push_samples_to_queue = input_spyserver_client_push_samples_to_queue,
    .stop_sample_queue_push = input_spyserver_client_stop_sample_queue_push,
    .cleanup = input_spyserver_client_cleanup,
    .get_summary_info = input_spyserver_client_get_summary_info,
    .validate_options = input_spyserver_client_validate_options,
    .validate_generic_options = NULL,
    .pre_stream_iq_correction = NULL
};

InputModuleInterface* input_spyserver_client_get_module_api(void) {
    return &s_input_spyserver_client_api;
}
