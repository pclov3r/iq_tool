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
#include "log.h"
#include "app_context.h"
#include "input_common.h"
#include "signal_handler.h"
#include "queue.h"
#include "argparse.h"
#include "sample_convert.h"
#include "memory_arena.h"
#include "ring_buffer.h"
#include "utils.h"
#include "networking.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>

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
    const char* format_str;
} s_spyserver_client_config;

// --- Private Module State ---
typedef struct {
    NetworkingContext* net_ctx;
    SpyServerDeviceInfo device_info;
    bool device_info_ok;
    format_t active_format;
    RingBuffer* stream_buffer;
} SpyServerClientPrivateData;


// --- CLI Options ---
static const struct argparse_option spyserver_client_cli_options[] = {
    OPT_GROUP("SpyServer Client Options"),
    OPT_STRING(0, "spyserver-client-host", &s_spyserver_client_config.hostname, "Hostname or IP of the spyserver instance (Required).", NULL, 0, 0),
    OPT_INTEGER(0, "spyserver-client-port", &s_spyserver_client_config.port, "Port number of the spyserver instance (Required).", NULL, 0, 0),
    OPT_INTEGER(0, "spyserver-client-gain", &s_spyserver_client_config.gain, "Set manual gain. Disables AGC. (Ignored on servers without gain control)", NULL, 0, 0),
    OPT_STRING(0, "spyserver-client-format", &s_spyserver_client_config.format_str, "Select sample format {cu8|cs16|cs24|cf32}. Default is cu8.", NULL, 0, 0),
};

const struct argparse_option* spyserver_client_get_cli_options(int* count) {
    *count = sizeof(spyserver_client_cli_options) / sizeof(spyserver_client_cli_options[0]);
    return spyserver_client_cli_options;
}

// --- Default Configuration ---
void spyserver_client_set_default_config(struct AppConfig* config) {
    config->sdr.sample_rate_hz = SPYSERVER_DEFAULT_SAMPLE_RATE_HZ;
    s_spyserver_client_config.hostname = NULL;
    s_spyserver_client_config.port = 0;
    s_spyserver_client_config.gain = -1;
    s_spyserver_client_config.gain_provided = false;
    s_spyserver_client_config.format_str = "cu8";
}

// --- Function Prototypes ---
static void* spyserver_client_producer_thread(void* arg);
static bool spyserver_client_initialize(ModuleContext* ctx);
static void* spyserver_client_start_stream(ModuleContext* ctx);
static void spyserver_client_stop_stream(ModuleContext* ctx);
static void spyserver_client_cleanup(ModuleContext* ctx);
static void spyserver_client_get_summary_info(const ModuleContext* ctx, InputSummaryInfo* info);
static bool spyserver_client_validate_options(AppConfig* config);

// --- The ModuleInterface V-Table ---
static ModuleInterface spyserver_client_module_api = {
    .initialize = spyserver_client_initialize,
    .start_stream = spyserver_client_start_stream,
    .stop_stream = spyserver_client_stop_stream,
    .cleanup = spyserver_client_cleanup,
    .get_summary_info = spyserver_client_get_summary_info,
    .validate_options = spyserver_client_validate_options,
    .validate_generic_options = NULL,
    .has_known_length = _input_source_has_known_length_false,
    .pre_stream_iq_correction = NULL
};

ModuleInterface* get_spyserver_client_input_module_api(void) {
    return &spyserver_client_module_api;
}

// --- Helper Functions for Protocol and Logic ---

static int get_spyserver_enum_from_internal_format(format_t fmt) {
    switch (fmt) {
        case CU8:  return SPYSERVER_STREAM_FORMAT_UINT8;
        case CS16: return SPYSERVER_STREAM_FORMAT_INT16;
        case CS24: return SPYSERVER_STREAM_FORMAT_INT24;
        case CF32: return SPYSERVER_STREAM_FORMAT_FLOAT;
        default:   return SPYSERVER_STREAM_FORMAT_INVALID;
    }
}

static format_t get_internal_format_from_spyserver_enum(int spyserver_format) {
    switch(spyserver_format) {
        case SPYSERVER_STREAM_FORMAT_UINT8: return CU8;
        case SPYSERVER_STREAM_FORMAT_INT16: return CS16;
        case SPYSERVER_STREAM_FORMAT_INT24: return CS24;
        case SPYSERVER_STREAM_FORMAT_FLOAT: return CF32;
        default: return FORMAT_UNKNOWN;
    }
}

static bool send_setting(SpyServerClientPrivateData* p, uint32_t setting, uint32_t value) {
    unsigned char command_buffer[sizeof(SpyServerCommandHeader) + sizeof(SpyServerSettingTarget)];

    SpyServerCommandHeader* header = (SpyServerCommandHeader*)command_buffer;
    header->CommandType = SPYSERVER_CMD_SET_SETTING;
    header->BodySize = sizeof(SpyServerSettingTarget);

    SpyServerSettingTarget* payload = (SpyServerSettingTarget*)(command_buffer + sizeof(SpyServerCommandHeader));
    payload->Setting = setting;
    payload->Value = value;

    return networking_send_all(p->net_ctx, command_buffer, sizeof(command_buffer));
}

// --- Validation Function ---
static bool spyserver_client_validate_options(AppConfig* config) {
    (void)config;
    if (s_spyserver_client_config.hostname == NULL) {
        log_fatal("Missing required argument: --spyserver-client-host <address>");
        return false;
    }
    if (s_spyserver_client_config.port == 0) {
        log_fatal("Missing required argument: --spyserver-client-port <number>");
        return false;
    }

    if (s_spyserver_client_config.gain != -1) {
        s_spyserver_client_config.gain_provided = true;
    }

    if (s_spyserver_client_config.format_str != NULL) {
        if (strcasecmp(s_spyserver_client_config.format_str, "cu8") != 0 &&
            strcasecmp(s_spyserver_client_config.format_str, "cs16") != 0 &&
            strcasecmp(s_spyserver_client_config.format_str, "cs24") != 0 &&
            strcasecmp(s_spyserver_client_config.format_str, "cf32") != 0) {
            log_fatal("Invalid value for --spyserver-client-format: '%s'. Must be one of {cu8|cs16|cs24|cf32}.", s_spyserver_client_config.format_str);
            return false;
        }
    }
    return true;
}

// --- Main Module Implementations ---
static bool spyserver_client_initialize(ModuleContext* ctx) {
    AppConfig* config = (AppConfig*)ctx->config;
    AppResources* resources = ctx->resources;

    SpyServerClientPrivateData* p = (SpyServerClientPrivateData*)mem_arena_alloc(&resources->setup_arena, sizeof(SpyServerClientPrivateData), true);
    if (!p) return false;
    resources->input_module_private_data = p;
    p->net_ctx = NULL; // Initialize handle

    // This module now takes responsibility for initializing its dependency.
    if (!networking_initialize_module()) {
        log_error("SpyServer client failed because the networking module could not be initialized.");
        return false;
    }

    log_info("Connecting to SpyServer at %s:%d...", s_spyserver_client_config.hostname, s_spyserver_client_config.port);

    p->net_ctx = networking_connect(s_spyserver_client_config.hostname, s_spyserver_client_config.port, &resources->setup_arena);
    if (!p->net_ctx) {
        networking_cleanup_module(); // Release our reference on failure.
        return false;
    }

    log_info("Connected. Performing handshake...");

    char user_agent[128];
    snprintf(user_agent, sizeof(user_agent), "%s version %s", APP_NAME, GIT_HASH);

    size_t user_agent_len = strlen(user_agent);
    size_t payload_size = sizeof(uint32_t) + user_agent_len;

    unsigned char* payload_buffer = (unsigned char*)mem_arena_alloc(&resources->setup_arena, payload_size, false);
    if (!payload_buffer) {
        networking_disconnect(p->net_ctx);
        networking_cleanup_module();
        return false;
    }

    uint32_t protocol_version = SPYSERVER_PROTOCOL_VERSION;
    memcpy(payload_buffer, &protocol_version, sizeof(uint32_t));
    memcpy(payload_buffer + sizeof(uint32_t), user_agent, user_agent_len);

    SpyServerCommandHeader hello_header;
    hello_header.CommandType = SPYSERVER_CMD_HELLO;
    hello_header.BodySize = payload_size;

    bool send_ok = networking_send_all(p->net_ctx, &hello_header, sizeof(hello_header)) &&
                   networking_send_all(p->net_ctx, payload_buffer, payload_size);

    if (!send_ok) {
        networking_disconnect(p->net_ctx);
        networking_cleanup_module();
        return false;
    }

    SpyServerMessageHeader response_header;
    if (!networking_recv_all(p->net_ctx, &response_header, sizeof(response_header))) {
        networking_disconnect(p->net_ctx);
        networking_cleanup_module();
        return false;
    }

    if (response_header.MessageType != SPYSERVER_MSG_TYPE_DEVICE_INFO) {
        log_fatal("Did not receive DeviceInfo after handshake. Server may have rejected the connection (MessageType=%u).", response_header.MessageType);
        networking_disconnect(p->net_ctx);
        networking_cleanup_module();
        return false;
    }
    if (response_header.BodySize != sizeof(SpyServerDeviceInfo)) {
        log_fatal("Received DeviceInfo with unexpected size (%u vs %zu).", response_header.BodySize, sizeof(SpyServerDeviceInfo));
        networking_disconnect(p->net_ctx);
        networking_cleanup_module();
        return false;
    }
    if (!networking_recv_all(p->net_ctx, &p->device_info, sizeof(SpyServerDeviceInfo))) {
        networking_disconnect(p->net_ctx);
        networking_cleanup_module();
        return false;
    }
    p->device_info_ok = true;

    log_info("Handshake complete. Waiting for client sync message...");
    if (!networking_recv_all(p->net_ctx, &response_header, sizeof(response_header))) {
        networking_disconnect(p->net_ctx);
        networking_cleanup_module();
        return false;
    }

    if (response_header.MessageType != SPYSERVER_MSG_TYPE_CLIENT_SYNC) {
        log_fatal("Did not receive ClientSync message after handshake. Protocol error.");
        networking_disconnect(p->net_ctx);
        networking_cleanup_module();
        return false;
    }

    SpyServerClientSync sync_info;
    if (response_header.BodySize < sizeof(SpyServerClientSync)) {
        log_fatal("Received ClientSync with unexpected size (%u vs %zu). Protocol mismatch.", response_header.BodySize, sizeof(SpyServerClientSync));
        networking_disconnect(p->net_ctx);
        networking_cleanup_module();
        return false;
    }

    if (!networking_recv_all(p->net_ctx, &sync_info, sizeof(SpyServerClientSync))) {
        networking_disconnect(p->net_ctx);
        networking_cleanup_module();
        return false;
    }

    size_t extra_bytes_to_discard = response_header.BodySize - sizeof(SpyServerClientSync);
    if (extra_bytes_to_discard > 0) {
        char discard_buffer[256];
        while (extra_bytes_to_discard > 0) {
            size_t to_read = (extra_bytes_to_discard > sizeof(discard_buffer)) ? sizeof(discard_buffer) : extra_bytes_to_discard;
            if (!networking_recv_all(p->net_ctx, discard_buffer, to_read)) {
                networking_disconnect(p->net_ctx);
                networking_cleanup_module();
                return false;
            }
            extra_bytes_to_discard -= to_read;
        }
    }

    if (sync_info.CanControl == 0) {
        log_error("Cannot control the remote device. Another client has control.");
        networking_disconnect(p->net_ctx);
        networking_cleanup_module();
        return false;
    }

    log_info("Client has control of the device. Negotiating stream parameters...");

    // Determine the format our client wants to request based on user args or defaults.
    format_t requested_format = utils_get_format_from_string(s_spyserver_client_config.format_str);
    log_info("Client requesting sample format: %s", utils_get_format_description_string(requested_format));

    // Assume our request will be honored unless the server says otherwise.
    format_t final_format = requested_format;

    // Check if the server is forcing a specific format.
    uint32_t forced_format_enum = p->device_info.ForcedIQFormat;
    if (forced_format_enum != 0) {
        format_t server_forced_format = get_internal_format_from_spyserver_enum(forced_format_enum);

        // Only warn and switch if the server's required format is valid and
        // DIFFERENT from what we were going to request.
        if (server_forced_format != FORMAT_UNKNOWN && server_forced_format != requested_format) {
            log_warn("Server requires the %s sample format. Switching...",
                     utils_get_format_description_string(server_forced_format));
            // Override our choice with the server's required format.
            final_format = server_forced_format;
        }
    }

    // Set the final, negotiated format for use by the rest of the application.
    p->active_format = final_format;
    resources->input_format = final_format;
    resources->input_bytes_per_sample_pair = get_bytes_per_sample(final_format);

    uint32_t max_sr = p->device_info.MaximumSampleRate;
    uint32_t min_dec = p->device_info.MinimumIQDecimation;
    uint32_t dec_count = p->device_info.DecimationStageCount;
    double supported_rates[32];
    int num_supported_rates = 0;
    for (uint32_t i = min_dec; i <= dec_count && num_supported_rates < 32; i++) {
        supported_rates[num_supported_rates++] = (double)max_sr / (double)(1 << i);
    }

    double user_rate = config->sdr.sample_rate_hz > 0 ? config->sdr.sample_rate_hz : supported_rates[0];
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
        log_info("Using requested sample rate: %.0f Hz.", actual_rate);
    } else {
        log_info("Requested sample rate %.0f Hz. Using closest available rate: %.0f Hz.", user_rate, actual_rate);
    }

    resources->source_info.samplerate = (int)actual_rate;

    int format_to_request_int = get_spyserver_enum_from_internal_format(final_format);

    log_info("Configuring remote device...");
    if (!send_setting(p, SPYSERVER_SETTING_IQ_FREQUENCY, (uint32_t)config->sdr.rf_freq_hz)) return false;
    if (!send_setting(p, SPYSERVER_SETTING_IQ_DECIMATION, dec_index_to_send)) return false;
    if (!send_setting(p, SPYSERVER_SETTING_IQ_FORMAT, format_to_request_int)) return false;

    if (s_spyserver_client_config.gain_provided) {
        if (!send_setting(p, SPYSERVER_SETTING_GAIN, s_spyserver_client_config.gain)) return false;
    }

    float digital_gain_float = 0.0f;
    uint32_t device_type = p->device_info.DeviceType;
    if (device_type == SPYSERVER_DEV_AIRSPY_ONE) {
        uint32_t gain_index = s_spyserver_client_config.gain_provided ? s_spyserver_client_config.gain : 0;
        digital_gain_float = (float)(p->device_info.MaximumGainIndex - gain_index) + ((float)dec_index_to_send * 3.01f);
    } else {
        digital_gain_float = (float)dec_index_to_send * 3.01f;
    }
    if (!send_setting(p, SPYSERVER_SETTING_IQ_DIGITAL_GAIN, (uint32_t)digital_gain_float)) return false;

    if (!send_setting(p, SPYSERVER_SETTING_STREAMING_MODE, SPYSERVER_STREAM_MODE_IQ_ONLY)) return false;

    p->stream_buffer = ring_buffer_create(SPYSERVER_STREAM_BUFFER_BYTES);
    if (!p->stream_buffer) {
        networking_disconnect(p->net_ctx);
        networking_cleanup_module();
        return false;
    }

    log_info("Initialization successful.");
    return true;
}

static void* spyserver_client_producer_thread(void* arg) {
    ModuleContext* ctx = (ModuleContext*)arg;
    AppResources* resources = ctx->resources;
    SpyServerClientPrivateData* p = (SpyServerClientPrivateData*)resources->input_module_private_data;

    unsigned char network_read_buffer[65536];

    while (!is_shutdown_requested()) {
        SpyServerMessageHeader header;
        if (!networking_recv_all(p->net_ctx, &header, sizeof(header))) {
            if (!is_shutdown_requested()) {
                 handle_fatal_thread_error("Connection to spyserver lost.", resources);
            }
            break;
        }

        uint32_t body_size = header.BodySize;
        if (body_size == 0) continue;

        if (ring_buffer_write(p->stream_buffer, &header, sizeof(header)) < sizeof(header)) {
             log_warn("SpyServer stream buffer overrun on header write. Dropping data.");
             break;
        }

        size_t bytes_remaining = body_size;
        while(bytes_remaining > 0) {
            size_t to_read = (bytes_remaining > sizeof(network_read_buffer)) ? sizeof(network_read_buffer) : bytes_remaining;
            if (!networking_recv_all(p->net_ctx, network_read_buffer, to_read)) {
                if (!is_shutdown_requested()) handle_fatal_thread_error("Connection to spyserver lost.", resources);
                goto end_loop;
            }

            if (ring_buffer_write(p->stream_buffer, network_read_buffer, to_read) < to_read) {
                log_warn("SpyServer stream buffer overrun on body write. Dropping data.");
                goto end_loop;
            }
            bytes_remaining -= to_read;
        }

        sdr_input_update_heartbeat(resources);
    }

end_loop:;
    ring_buffer_signal_end_of_stream(p->stream_buffer);
    log_debug("SpyServer producer thread is exiting.");
    return NULL;
}

static void* spyserver_client_start_stream(ModuleContext* ctx) {
    AppResources* resources = ctx->resources;
    SpyServerClientPrivateData* p = (SpyServerClientPrivateData*)resources->input_module_private_data;

    if (!send_setting(p, SPYSERVER_SETTING_STREAMING_ENABLED, 1)) {
        handle_fatal_thread_error("Failed to start spyserver stream.", resources);
        return NULL;
    }

    pthread_t producer_thread_id;
    if (pthread_create(&producer_thread_id, NULL, spyserver_client_producer_thread, ctx) != 0) {
        handle_fatal_thread_error("Failed to create spyserver producer thread.", resources);
        return NULL;
    }

    size_t buffer_capacity = ring_buffer_get_capacity(p->stream_buffer);
    size_t high_water_mark = (size_t)(buffer_capacity * SPYSERVER_PREBUFFER_HIGH_WATER_MARK);
    log_info("Pre-buffering SpyServer data...");

    while (!is_shutdown_requested() && ring_buffer_get_size(p->stream_buffer) < high_water_mark) {
        if (resources->error_occurred) break;
        #ifdef _WIN32
            Sleep(100);
        #else
            usleep(100000);
        #endif
    }

    if (is_shutdown_requested() || resources->error_occurred) {
        log_warn("Shutdown requested during pre-buffering phase.");
    } else {
        log_info("Pre-buffering complete.");
    }

    while (!is_shutdown_requested()) {
        SpyServerMessageHeader header;
        if (ring_buffer_read(p->stream_buffer, &header, sizeof(header)) < sizeof(header)) {
            break; // End of stream
        }

        uint32_t msg_type = header.MessageType & 0xFFFF;
        uint32_t body_size = header.BodySize;

        if (body_size == 0 || msg_type < SPYSERVER_MSG_TYPE_UINT8_IQ || msg_type > SPYSERVER_MSG_TYPE_FLOAT_IQ) {
            if (body_size > 0) {
                char discard_buf[1024];
                while(body_size > 0) {
                    size_t to_read = body_size > sizeof(discard_buf) ? sizeof(discard_buf) : body_size;
                    if (ring_buffer_read(p->stream_buffer, discard_buf, to_read) < to_read) goto end_loop;
                    body_size -= to_read;
                }
            }
            continue;
        }

        size_t bytes_remaining_in_packet = body_size;
        while (bytes_remaining_in_packet > 0) {
            SampleChunk* item = (SampleChunk*)queue_dequeue(resources->free_sample_chunk_queue);
            if (!item) goto end_loop; // Shutdown signaled

            size_t bytes_this_chunk = (bytes_remaining_in_packet > item->raw_input_capacity_bytes)
                                    ? item->raw_input_capacity_bytes
                                    : bytes_remaining_in_packet;

            if (ring_buffer_read(p->stream_buffer, item->raw_input_data, bytes_this_chunk) < bytes_this_chunk) {
                queue_enqueue(resources->free_sample_chunk_queue, item);
                goto end_loop; // End of stream or error
            }

            item->packet_sample_format = p->active_format;
            item->input_bytes_per_sample_pair = get_bytes_per_sample(p->active_format);
            item->frames_read = bytes_this_chunk / item->input_bytes_per_sample_pair;

            item->is_last_chunk = false;
            item->stream_discontinuity_event = false;

            if (item->frames_read > 0) {
                pthread_mutex_lock(&resources->progress_mutex);
                resources->total_frames_read += item->frames_read;
                pthread_mutex_unlock(&resources->progress_mutex);
            }

            if (!queue_enqueue(resources->reader_output_queue, item)) {
                queue_enqueue(resources->free_sample_chunk_queue, item);
                goto end_loop; // Shutdown signaled
            }

            bytes_remaining_in_packet -= bytes_this_chunk;
        }
    }
end_loop:;

    if (!is_shutdown_requested()) {
        request_shutdown();
    }
    pthread_join(producer_thread_id, NULL);

    log_debug("SpyServer Client stream thread is exiting.");
    return NULL;
}

static void spyserver_client_stop_stream(ModuleContext* ctx) {
    AppResources* resources = ctx->resources;
    if (resources->input_module_private_data) {
        SpyServerClientPrivateData* p = (SpyServerClientPrivateData*)resources->input_module_private_data;
        if (p->stream_buffer) {
            ring_buffer_signal_shutdown(p->stream_buffer);
        }
    }
}

static void spyserver_client_cleanup(ModuleContext* ctx) {
    AppResources* resources = ctx->resources;
    if (resources->input_module_private_data) {
        SpyServerClientPrivateData* p = (SpyServerClientPrivateData*)resources->input_module_private_data;
        if (p->stream_buffer) {
            ring_buffer_destroy(p->stream_buffer);
            p->stream_buffer = NULL;
        }
        if (p->net_ctx) {
            networking_disconnect(p->net_ctx);
            p->net_ctx = NULL;
        }
        networking_cleanup_module();
    }
    log_info("Exiting SpyServer client...");
}

static void spyserver_client_get_summary_info(const ModuleContext* ctx, InputSummaryInfo* info) {
    const SpyServerClientPrivateData* p = (const SpyServerClientPrivateData*)ctx->resources->input_module_private_data;
    const AppResources* resources = ctx->resources;
    const AppConfig* config = ctx->config;
    char server_addr[256];
    snprintf(server_addr, sizeof(server_addr), "%s:%d", s_spyserver_client_config.hostname, s_spyserver_client_config.port);
    add_summary_item(info, "Input Source", "SpyServer Client");
    add_summary_item(info, "Server Address", server_addr);

    if (p && p->device_info_ok) {
        const char* dev_type_str = "Unknown";
        switch (p->device_info.DeviceType) {
            case SPYSERVER_DEV_AIRSPY_ONE: dev_type_str = "Airspy One"; break;
            case SPYSERVER_DEV_AIRSPY_HF:  dev_type_str = "Airspy HF+"; break;
            case SPYSERVER_DEV_RTLSDR:     dev_type_str = "RTL-SDR"; break;
        }
        char dev_info_str[128];
        snprintf(dev_info_str, sizeof(dev_info_str), "%s (S/N: %08X)", dev_type_str, p->device_info.DeviceSerial);
        add_summary_item(info, "Remote Device", dev_info_str);
        add_summary_item(info, "Input Format", utils_get_format_description_string(resources->input_format));
        add_summary_item(info, "Input Rate", "%d Hz", resources->source_info.samplerate);
        add_summary_item(info, "RF Frequency", "%.0f Hz", config->sdr.rf_freq_hz);

        if (s_spyserver_client_config.gain_provided) {
            add_summary_item(info, "Gain", "%d (Manual)", s_spyserver_client_config.gain);
        } else {
            add_summary_item(info, "Gain", "Automatic (AGC)");
        }
    }
}
