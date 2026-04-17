/**
 * @file output_nrsc5.c
 * @brief Implements the NRSC5 (HD Radio) output module.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "output_nrsc5.h"
#include "miniaudio.h"
#include "module.h"
#include "app_context.h"
#include "log.h"
#include "platform.h"
#include "ring_buffer.h"
#include "utils.h"
#include "signal_handler.h"
#include "nrsc5.h"
#include "queue.h"

// --- Configuration Constants ---
#define NRSC5_AUDIO_BUFFER_SIZE (512 * 1024)
#define NRSC5_AUDIO_CHANNELS 2
#define NRSC5_AUDIO_SAMPLE_RATE 44100
#define SAFE_STR(s) ((s) ? (s) : "(null)")

// --- Private Types ---

typedef enum {
    NRSC5_MODE_CS16_FM,
    NRSC5_MODE_CS16_AM,
    NRSC5_MODE_CU8_FM,
    NRSC5_MODE_CU8_AM
} Nrsc5Mode;

typedef struct {
    // Instance State
    nrsc5_t* nrsc5_inst;
    ma_device audio_device;
    RingBuffer* audio_ring_buffer;
    bool audio_device_initialized;
    unsigned int active_program;

    // BER Stats
    float ber_min;
    float ber_max;
    float ber_sum;
    float ber_count;

    // Bitrate & Error Stats
    unsigned int audio_packets;       // Total packets in current batch
    unsigned int audio_packets_valid; // Packets passing CRC (for bitrate)
    unsigned int audio_bytes;         // Bytes from valid packets
    unsigned int audio_errors;        // Recent CRC errors
    unsigned int total_audio_errors;  // Total CRC errors since sync
} Nrsc5Context;

// --- CLI Configuration Storage ---
static struct {
    char* mode_str;
    int program_id;
    Nrsc5Mode active_mode;
} s_nrsc5_config = {
    .mode_str = NULL,
    .program_id = -1, // Sentinel: -1 indicates "not set by user"
    .active_mode = NRSC5_MODE_CS16_FM
};

// --- Forward Declarations ---
static void nrsc5_event_callback(const nrsc5_event_t *evt, void *opaque);
static void miniaudio_data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount);

// --- Miniaudio Callback ---
static void miniaudio_data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    (void)pInput;
    Nrsc5Context* ctx = (Nrsc5Context*)pDevice->pUserData;

    if (frameCount == 0) return;

    size_t bytes_needed = frameCount * NRSC5_AUDIO_CHANNELS * sizeof(int16_t);

    // Check availability to prevent deadlock at startup.
    size_t available = ring_buffer_get_size(ctx->audio_ring_buffer);

    if (available < bytes_needed) {
        if (available > 0) ring_buffer_read(ctx->audio_ring_buffer, pOutput, available);
        memset((uint8_t*)pOutput + available, 0, bytes_needed - available);
        return;
    }

    // We know we have enough data, so this call will not sleep.
    ring_buffer_read(ctx->audio_ring_buffer, pOutput, bytes_needed);
}

// --- Helper: BER Stats ---
static void update_ber_stats(Nrsc5Context* ctx, float cber) {
    ctx->ber_sum += cber;
    ctx->ber_count += 1.0f;
    if (cber < ctx->ber_min) ctx->ber_min = cber;
    if (cber > ctx->ber_max) ctx->ber_max = cber;

    log_info("NRSC5: BER: %f, avg: %f, min: %f, max: %f",
             cber, ctx->ber_sum / ctx->ber_count, ctx->ber_min, ctx->ber_max);
}

// --- NRSC5 Event Callback ---
static void nrsc5_event_callback(const nrsc5_event_t *evt, void *opaque) {
    Nrsc5Context* ctx = (Nrsc5Context*)opaque;
    const char* name_ptr = NULL;
    char time_str[64];

    switch (evt->event) {
        case NRSC5_EVENT_LOST_DEVICE:
            log_error("NRSC5: Lost device synchronization.");
            break;

        case NRSC5_EVENT_SYNC:
            log_info("NRSC5: Synchronized");
            log_info("NRSC5: Frequency offset: %.0f Hz", evt->sync.freq_offset);
            log_info("NRSC5: Primary service mode: %d", evt->sync.psmi);
            ctx->ber_min = 1.0f;
            ctx->ber_max = 0.0f;
            ctx->ber_sum = 0.0f;
            ctx->ber_count = 0.0f;
            ctx->total_audio_errors = 0;
            break;

        case NRSC5_EVENT_LOST_SYNC:
            log_info("NRSC5: Lost synchronization");
            break;

        case NRSC5_EVENT_MER:
            log_info("NRSC5: MER: %.1f dB (lower), %.1f dB (upper)", evt->mer.lower, evt->mer.upper);
            break;

        case NRSC5_EVENT_BER:
            update_ber_stats(ctx, evt->ber.cber);
            break;

        case NRSC5_EVENT_HDC:
            if (evt->hdc.program == ctx->active_program) {
                ctx->audio_bytes += evt->hdc.count;
                ctx->audio_packets++;

                if (evt->hdc.flags & NRSC5_PKT_FLAGS_CRC_ERROR) {
                    ctx->audio_errors++;
                    ctx->total_audio_errors++;
                } else {
                    ctx->audio_packets_valid++;
                }

                // Trigger 1: Bitrate (Based on 32 VALID packets)
                if (ctx->audio_packets_valid >= 32) {
                    float kbps = (float)ctx->audio_bytes * 8.0f * NRSC5_SAMPLE_RATE_AUDIO /
                                 NRSC5_AUDIO_FRAME_SAMPLES / ctx->audio_packets_valid / 1000.0f;
                    log_info("NRSC5: Audio bit rate: %.1f kbps", kbps);
                    ctx->audio_packets_valid = 0;
                    ctx->audio_bytes = 0;
                }

                // Trigger 2: Errors (Based on 32 TOTAL packets)
                if (ctx->audio_packets >= 32) {
                    if (ctx->audio_errors > 0) {
                        log_warn("NRSC5: Audio CRC errors (recent): %d", ctx->audio_errors);
                        log_warn("NRSC5: Audio CRC errors (total): %d", ctx->total_audio_errors);
                    }
                    ctx->audio_packets = 0;
                    ctx->audio_errors = 0;
                }
            }
            break;

        case NRSC5_EVENT_AUDIO:
            if (evt->audio.program == ctx->active_program) {
                // Sanity check incoming data
                if (!evt->audio.data || evt->audio.count == 0 || evt->audio.count > 100000) return;

                // evt->audio.count is TOTAL samples (interleaved), not frames.
                size_t bytes = evt->audio.count * sizeof(int16_t);

                // Write to ring buffer. If full, it drops data (safe).
                ring_buffer_write(ctx->audio_ring_buffer, evt->audio.data, bytes);
            }
            break;

        case NRSC5_EVENT_ID3:
            if (evt->id3.program == ctx->active_program) {
                if (evt->id3.title)  log_info("NRSC5: Title: %s", evt->id3.title);
                if (evt->id3.artist) log_info("NRSC5: Artist: %s", evt->id3.artist);
                if (evt->id3.album)  log_info("NRSC5: Album: %s", evt->id3.album);
                if (evt->id3.genre)  log_info("NRSC5: Genre: %s", evt->id3.genre);

                if (evt->id3.xhdr.param >= 0) {
                    log_info("NRSC5: XHDR: %d %08X %d",
                             evt->id3.xhdr.param, evt->id3.xhdr.mime, evt->id3.xhdr.lot);
                }
            }
            break;

        case NRSC5_EVENT_SIG:
            for (nrsc5_sig_service_t* svc = evt->sig.services; svc != NULL; svc = svc->next) {
                log_info("NRSC5: SIG Service: type=%s number=%d name=%s",
                         svc->type == NRSC5_SIG_SERVICE_AUDIO ? "audio" : "data",
                         svc->number, SAFE_STR(svc->name));

                for (nrsc5_sig_component_t* comp = svc->components; comp != NULL; comp = comp->next) {
                    if (comp->type == NRSC5_SIG_SERVICE_AUDIO) {
                        log_info("NRSC5:   Audio component: id=%d port=%04X type=%d mime=%08X",
                                 comp->id, comp->audio.port, comp->audio.type, comp->audio.mime);
                    } else {
                        log_info("NRSC5:   Data component: id=%d port=%04X service_data_type=%d type=%d mime=%08X",
                                 comp->id, comp->data.port, comp->data.service_data_type,
                                 comp->data.type, comp->data.mime);
                    }
                }
            }
            break;

        case NRSC5_EVENT_STATION_NAME:
            log_info("NRSC5: Station name: %s", SAFE_STR(evt->station_name.name));
            break;

        case NRSC5_EVENT_STATION_SLOGAN:
            log_info("NRSC5: Slogan: %s", SAFE_STR(evt->station_slogan.slogan));
            break;

        case NRSC5_EVENT_STATION_MESSAGE:
            log_info("NRSC5: Message: %s", SAFE_STR(evt->station_message.message));
            break;

        case NRSC5_EVENT_STATION_ID:
            log_info("NRSC5: Country: %s, FCC facility ID: %d",
                     SAFE_STR(evt->station_id.country_code), evt->station_id.fcc_facility_id);
            break;

        case NRSC5_EVENT_STATION_LOCATION:
            log_info("NRSC5: Station location: %.4f, %.4f, %dm",
                     evt->station_location.latitude,
                     evt->station_location.longitude,
                     evt->station_location.altitude);
            break;

        case NRSC5_EVENT_AUDIO_SERVICE_DESCRIPTOR:
            nrsc5_program_type_name(evt->asd.type, &name_ptr);
            log_info("NRSC5: Audio program %d: %s, type: %s, sound experience %d",
                     evt->asd.program,
                     evt->asd.access == NRSC5_ACCESS_PUBLIC ? "public" : "restricted",
                     SAFE_STR(name_ptr), evt->asd.sound_exp);
            break;

        case NRSC5_EVENT_DATA_SERVICE_DESCRIPTOR:
            nrsc5_service_data_type_name(evt->dsd.type, &name_ptr);
            log_info("NRSC5: Data service: %s, type: %s, MIME type %03x",
                     evt->dsd.access == NRSC5_ACCESS_PUBLIC ? "public" : "restricted",
                     SAFE_STR(name_ptr), evt->dsd.mime_type);
            break;

        case NRSC5_EVENT_AUDIO_SERVICE:
            nrsc5_program_type_name(evt->audio_service.type, &name_ptr);
            log_info("NRSC5: Audio service %d: %s, type: %s, codec: %d, blend: %d, gain: %d dB, delay: %d, latency: %d",
                     evt->audio_service.program,
                     evt->audio_service.access == NRSC5_ACCESS_PUBLIC ? "public" : "restricted",
                     SAFE_STR(name_ptr),
                     evt->audio_service.codec_mode,
                     evt->audio_service.blend_control,
                     evt->audio_service.digital_audio_gain,
                     evt->audio_service.common_delay,
                     evt->audio_service.latency);
            break;

        case NRSC5_EVENT_EMERGENCY_ALERT:
            if (evt->emergency_alert.message) {
                // Safe string construction to prevent buffer overflows
                char alert_buf[1024];
                int offset = 0;

                offset += snprintf(alert_buf + offset, sizeof(alert_buf) - offset, "Category=[");

                if (evt->emergency_alert.category1 >= 1) {
                    nrsc5_alert_category_name(evt->emergency_alert.category1, &name_ptr);
                    offset += snprintf(alert_buf + offset, sizeof(alert_buf) - offset, "%s", SAFE_STR(name_ptr));
                }
                if (evt->emergency_alert.category2 >= 1) {
                    nrsc5_alert_category_name(evt->emergency_alert.category2, &name_ptr);
                    offset += snprintf(alert_buf + offset, sizeof(alert_buf) - offset, ", %s", SAFE_STR(name_ptr));
                }
                offset += snprintf(alert_buf + offset, sizeof(alert_buf) - offset, "] ");

                switch (evt->emergency_alert.location_format) {
                    case NRSC5_LOCATION_FORMAT_SAME:
                        offset += snprintf(alert_buf + offset, sizeof(alert_buf) - offset, "SAME=");
                        break;
                    case NRSC5_LOCATION_FORMAT_FIPS:
                        offset += snprintf(alert_buf + offset, sizeof(alert_buf) - offset, "FIPS=");
                        break;
                    case NRSC5_LOCATION_FORMAT_ZIP:
                        offset += snprintf(alert_buf + offset, sizeof(alert_buf) - offset, "ZIP=");
                        break;
                }

                offset += snprintf(alert_buf + offset, sizeof(alert_buf) - offset, "[");
                for (int i = 0; i < evt->emergency_alert.num_locations; i++) {
                    if (i > 0) offset += snprintf(alert_buf + offset, sizeof(alert_buf) - offset, ", ");
                    offset += snprintf(alert_buf + offset, sizeof(alert_buf) - offset, "%d", evt->emergency_alert.locations[i]);
                }
                offset += snprintf(alert_buf + offset, sizeof(alert_buf) - offset, "]");

                log_info("NRSC5: Alert: %s %s", alert_buf, SAFE_STR(evt->emergency_alert.message));
            } else {
                log_info("NRSC5: Alert ended");
            }
            break;

        case NRSC5_EVENT_HERE_IMAGE:
            strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%SZ", evt->here_image.time_utc);
            log_info("NRSC5: HERE Image: type=%s, seq=%d, n1=%d, n2=%d, time=%s, name=%s, size=%d",
                     evt->here_image.image_type == NRSC5_HERE_IMAGE_TRAFFIC ? "TRAFFIC" : "WEATHER",
                     evt->here_image.seq, evt->here_image.n1, evt->here_image.n2, time_str,
                     SAFE_STR(evt->here_image.name), evt->here_image.size);
            break;

        case NRSC5_EVENT_LOT:
            strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%SZ", evt->lot.expiry_utc);
            log_info("NRSC5: LOT file: port=%04X lot=%d name=%s size=%d mime=%08X expiry=%s",
                     evt->lot.component->data.port, evt->lot.lot, SAFE_STR(evt->lot.name),
                     evt->lot.size, evt->lot.mime, time_str);
            break;

        default:
            break;
    }
}

// --- Module Interface Implementation ---

static bool nrsc5_output_initialize(ModuleContext* ctx) {
    AppContext* app = ctx->app;

    Nrsc5Context* p = (Nrsc5Context*)mem_arena_alloc(&app->pipeline.setup_arena, sizeof(Nrsc5Context), true);
    if (!p) return false;
    app->module.output_private_data = p;

    // Direct API call
    const char* ver_str = NULL;
    nrsc5_get_version(&ver_str);
    log_info("NRSC5: Library version %s", SAFE_STR(ver_str));

    // Initialize Ring Buffer
    p->audio_ring_buffer = ring_buffer_create(NRSC5_AUDIO_BUFFER_SIZE);
    if (p->audio_ring_buffer) {
        double bytes_per_sec = (double)NRSC5_AUDIO_SAMPLE_RATE * NRSC5_AUDIO_CHANNELS * sizeof(int16_t);
        double duration = (double)NRSC5_AUDIO_BUFFER_SIZE / bytes_per_sec;
        log_info("NRSC5: Audio Ring Buffer created: %zu bytes (%.2f seconds)", (size_t)NRSC5_AUDIO_BUFFER_SIZE, duration);
    }
    if (!p->audio_ring_buffer) return false;

    // Initialize Miniaudio
    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format   = ma_format_s16;
    deviceConfig.playback.channels = NRSC5_AUDIO_CHANNELS;
    deviceConfig.sampleRate        = NRSC5_AUDIO_SAMPLE_RATE;
    deviceConfig.dataCallback      = miniaudio_data_callback;
    deviceConfig.pUserData         = p;

    if (ma_device_init(NULL, &deviceConfig, &p->audio_device) != MA_SUCCESS) {
        log_fatal("NRSC5: Failed to initialize audio playback device.");
        return false;
    }
    p->audio_device_initialized = true;
    log_info("NRSC5: Audio device initialized (%d Hz, %d Channels).", NRSC5_AUDIO_SAMPLE_RATE, NRSC5_AUDIO_CHANNELS);

    // Initialize NRSC5 Instance
    log_debug("NRSC5: Opening pipe...");
    if (nrsc5_open_pipe(&p->nrsc5_inst) != 0) {
        log_fatal("NRSC5: Failed to open decoder pipe.");
        return false;
    }

    // Set and log active mode
    int decoder_mode = NRSC5_MODE_FM;
    if (s_nrsc5_config.active_mode == NRSC5_MODE_CS16_AM ||
        s_nrsc5_config.active_mode == NRSC5_MODE_CU8_AM) {
        decoder_mode = NRSC5_MODE_AM;
    }

    if (nrsc5_set_mode(p->nrsc5_inst, decoder_mode) != 0) {
        log_fatal("NRSC5: Failed to set decoder mode.");
        return false;
    }

    const char* mode_desc = "Unknown";
    switch (s_nrsc5_config.active_mode) {
        case NRSC5_MODE_CS16_FM: mode_desc = "cs16-fm (FM HD)"; break;
        case NRSC5_MODE_CS16_AM: mode_desc = "cs16-am (AM HD)"; break;
        case NRSC5_MODE_CU8_FM:  mode_desc = "cu8-fm (FM HD)"; break;
        case NRSC5_MODE_CU8_AM:  mode_desc = "cu8-am (AM HD)"; break;
    }
    log_info("NRSC5: Using mode: %s", mode_desc);

    // Set Callback
    p->active_program = (unsigned int)s_nrsc5_config.program_id;
    nrsc5_set_callback(p->nrsc5_inst, nrsc5_event_callback, p);

    // Start Decoder Thread
    log_debug("NRSC5: Starting decoder thread...");
    nrsc5_start(p->nrsc5_inst);

    // Start Audio Device
    log_debug("NRSC5: Starting audio device...");
    if (ma_device_start(&p->audio_device) != MA_SUCCESS) {
        log_error("NRSC5: Failed to start audio device.");
        return false;
    }

    return true;
}


static void nrsc5_output_reset(ModuleContext* ctx) { (void)ctx; }
static void nrsc5_output_flush(ModuleContext* ctx) {
    Nrsc5Context* p = (Nrsc5Context*)ctx->app->module.output_private_data;
    utils_wait_for_ring_buffer_drain(p->audio_ring_buffer, 10, 200, 200);
}
static size_t nrsc5_output_write_chunk(ModuleContext* ctx, const void* buffer, size_t input_bytes) {
    AppContext* app = ctx->app;
    Nrsc5Context* p = (Nrsc5Context*)app->module.output_private_data;
    if (input_bytes == 0) return 0;

    // --- RESTORE BACKPRESSURE ---
    const size_t THROTTLE_THRESHOLD = (size_t)(NRSC5_AUDIO_BUFFER_SIZE * 0.8);
    if (p->audio_ring_buffer) {
        ring_buffer_wait_for_threshold(p->audio_ring_buffer, THROTTLE_THRESHOLD);
        if (is_shutdown_requested()) return 0;
    }

    unsigned int frames = input_bytes / app->module.output_bytes_per_sample_pair;
    unsigned int num_scalars = frames * 2;
    int res = 0;
    switch (s_nrsc5_config.active_mode) {
        case NRSC5_MODE_CU8_FM:
        case NRSC5_MODE_CU8_AM:
            res = nrsc5_pipe_samples_cu8(p->nrsc5_inst, (uint8_t*)buffer, num_scalars);
            break;
        case NRSC5_MODE_CS16_FM:
        case NRSC5_MODE_CS16_AM:
            res = nrsc5_pipe_samples_cs16(p->nrsc5_inst, (int16_t*)buffer, num_scalars);
            break;
        default: break;
    }
    if (res != 0) log_error("NRSC5: Failed to pipe samples to decoder.");
    return input_bytes;
}


static void nrsc5_output_cleanup(ModuleContext* ctx) {
    AppContext* app = ctx->app;
    if (!app->module.output_private_data) return;
    Nrsc5Context* p = (Nrsc5Context*)app->module.output_private_data;

    if (p->audio_device_initialized) {
        ma_device_uninit(&p->audio_device);
    }

    if (p->nrsc5_inst) {
        nrsc5_stop(p->nrsc5_inst);
        nrsc5_close(p->nrsc5_inst);
        p->nrsc5_inst = NULL;
    }

    if (p->audio_ring_buffer) {
        ring_buffer_destroy(p->audio_ring_buffer);
    }
}

static bool nrsc5_output_validate_options(AppConfig* config) {
    // 1. Resolve Mode
    if (!s_nrsc5_config.mode_str) {
        s_nrsc5_config.active_mode = NRSC5_MODE_CS16_FM; // Default
    } else {
        if (strcasecmp(s_nrsc5_config.mode_str, "cs16-fm") == 0) s_nrsc5_config.active_mode = NRSC5_MODE_CS16_FM;
        else if (strcasecmp(s_nrsc5_config.mode_str, "cs16-am") == 0) s_nrsc5_config.active_mode = NRSC5_MODE_CS16_AM;
        else if (strcasecmp(s_nrsc5_config.mode_str, "cu8-fm") == 0) s_nrsc5_config.active_mode = NRSC5_MODE_CU8_FM;
        else if (strcasecmp(s_nrsc5_config.mode_str, "cu8-am") == 0) s_nrsc5_config.active_mode = NRSC5_MODE_CU8_AM;
        else {
            log_fatal("Invalid NRSC5 mode '%s'. Valid modes: cs16-fm, cs16-am, cu8-fm, cu8-am.", s_nrsc5_config.mode_str);
            return false;
        }
    }

    // 2. Enforce Format and Rate based on Mode
    double required_rate = 0.0;
    SampleFormat required_format = FORMAT_UNKNOWN;

    switch (s_nrsc5_config.active_mode) {
        case NRSC5_MODE_CS16_FM:
            required_rate = 744187.5;
            required_format = CS16;
            break;
        case NRSC5_MODE_CS16_AM:
            required_rate = 46511.71875;
            required_format = CS16;
            break;
        case NRSC5_MODE_CU8_FM:
        case NRSC5_MODE_CU8_AM:
            required_rate = 1488375.0;
            required_format = CU8;
            break;
    }

    // Override User Settings
    if (config->output_rate.target_rate != required_rate) {
        if (config->output_rate.provided) {
            log_warn("NRSC5: Ignoring user output rate %.0f Hz. Forcing required rate %.5f Hz.",
                     config->output_rate.target_rate, required_rate);
        }
        config->output_rate.target_rate = required_rate;
    }

    if (config->output.format != required_format) {
        if (config->output.type_provided) {
             log_warn("NRSC5: Ignoring user format. Forcing required format for selected mode.");
        }
        config->output.format = required_format;
    }

    // 3. Validate Program ID
    if (s_nrsc5_config.program_id == -1) {
        log_fatal("Missing required option: --nrsc5-program <0-7>.");
        return false;
    }

    if (s_nrsc5_config.program_id < 0 || s_nrsc5_config.program_id > 7) {
        log_fatal("Invalid NRSC5 program ID %d. Must be 0-7.", s_nrsc5_config.program_id);
        return false;
    }

    return true;
}

static void nrsc5_output_get_summary_info(const ModuleContext* ctx, OutputSummaryInfo* info) {
    (void)ctx;
    add_summary_item(info, "Output Type", "NRSC5 (HD Radio Player)");

    const char* mode_desc = "Unknown";
    switch (s_nrsc5_config.active_mode) {
        case NRSC5_MODE_CS16_FM: mode_desc = "cs16-fm (FM HD)"; break;
        case NRSC5_MODE_CS16_AM: mode_desc = "cs16-am (AM HD)"; break;
        case NRSC5_MODE_CU8_FM:  mode_desc = "cu8-fm (FM HD)"; break;
        case NRSC5_MODE_CU8_AM:  mode_desc = "cu8-am (AM HD)"; break;
    }
    add_summary_item(info, "Mode", "%s", mode_desc);
    add_summary_item(info, "Program", "%d (HD%d)", s_nrsc5_config.program_id, s_nrsc5_config.program_id + 1);
}

// --- CLI Options ---
static const struct argparse_option nrsc5_output_cli_options[] = {
    OPT_GROUP("NRSC5 Output (nrsc5)"),
    OPT_STRING(0, "nrsc5-mode", &s_nrsc5_config.mode_str, "Set decoder mode {cs16-fm|cs16-am|cu8-fm|cu8-am}. (Default: cs16-fm)", NULL, 0, 0),
    OPT_INTEGER(0, "nrsc5-program", &s_nrsc5_config.program_id, "Select HD program/subchannel (0-7). (Required)", NULL, 0, 0),
};

const struct argparse_option* nrsc5_output_get_cli_options(int* count) {
    *count = sizeof(nrsc5_output_cli_options) / sizeof(nrsc5_output_cli_options[0]);
    return nrsc5_output_cli_options;
}

// --- The V-Table ---
static OutputModuleInterface s_nrsc5_output_api = {
    .validate_options = nrsc5_output_validate_options,
    .get_cli_options = nrsc5_output_get_cli_options,
    .initialize = nrsc5_output_initialize,
    .write_chunk = nrsc5_output_write_chunk,
    .reset = nrsc5_output_reset,
    .flush = nrsc5_output_flush,
    .cleanup = nrsc5_output_cleanup,
    .get_summary_info = nrsc5_output_get_summary_info,
};

OutputModuleInterface* output_nrsc5_get_module_api(void) {
    return &s_nrsc5_output_api;
}
