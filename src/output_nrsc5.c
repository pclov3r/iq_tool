/**
 * @file output_nrsc5.c
 * @brief Implements the NRSC5 (HD Radio) output module.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#define PATH_SEPARATOR "\\"
#else
#include <unistd.h>
#define PATH_SEPARATOR "/"
#endif

#include "output_nrsc5.h"
#include "signal_handler.h"
#include "audio_output_functions.h"
#include "module.h"
#include "app_context.h"
#include "log.h"
#include "platform.h"
#include "ring_buffer.h"
#include "utilities.h"
#include "signal_handler.h"
#include "sample_format_table.h"
#include "nrsc5.h"
#include "queue.h"
#include "constants.h"

#ifdef _WIN32
#include <windows.h>

// --- Windows Dynamic Function Pointer Definitions ---
typedef struct {
    HINSTANCE dll_handle;
    void (*get_version)(const char **version);
    int  (*open_pipe)(nrsc5_t **nrsc5);
    void (*close)(nrsc5_t *nrsc5);
    int  (*set_mode)(nrsc5_t *nrsc5, int mode);
    void (*set_callback)(nrsc5_t *nrsc5, nrsc5_callback_t callback, void *opaque);
    int  (*start)(nrsc5_t *nrsc5);
    int  (*stop)(nrsc5_t *nrsc5);
    int  (*pipe_samples_cu8)(nrsc5_t *nrsc5, uint8_t *samples, unsigned int count);
    int  (*pipe_samples_cs16)(nrsc5_t *nrsc5, int16_t *samples, unsigned int count);
    void (*program_type_name)(unsigned int pty, const char **name);
    void (*service_data_type_name)(unsigned int type, const char **name);
    void (*alert_category_name)(unsigned int category, const char **name);
} Nrsc5WinApi;

static Nrsc5WinApi nrsc5_api = { NULL };

// --- Windows Preprocessor Overrides ---
#define nrsc5_get_version             nrsc5_api.get_version
#define nrsc5_open_pipe               nrsc5_api.open_pipe
#define nrsc5_close                   nrsc5_api.close
#define nrsc5_set_mode                nrsc5_api.set_mode
#define nrsc5_set_callback            nrsc5_api.set_callback
#define nrsc5_start                   nrsc5_api.start
#define nrsc5_stop                    nrsc5_api.stop
#define nrsc5_pipe_samples_cu8        nrsc5_api.pipe_samples_cu8
#define nrsc5_pipe_samples_cs16       nrsc5_api.pipe_samples_cs16
#define nrsc5_program_type_name       nrsc5_api.program_type_name
#define nrsc5_service_data_type_name  nrsc5_api.service_data_type_name
#define nrsc5_alert_category_name     nrsc5_api.alert_category_name

static bool load_nrsc5_dll(void) {
    if (nrsc5_api.dll_handle) return true;

    log_debug("Attempting to dynamically load the NRSC5 library...");

    // Target libnrsc5.dll exclusively
    nrsc5_api.dll_handle = LoadLibraryA("libnrsc5.dll");

    if (!nrsc5_api.dll_handle) {
        log_error("NRSC5: 'libnrsc5.dll' is missing in the program folder.");
        log_error("NRSC5: To enable the NRSC5 output module, please see compilation instructions at:");
        log_error("NRSC5:   https://github.com/theori-io/nrsc5#building-for-windows");
        log_error("NRSC5: Once compiled, copy 'libnrsc5.dll' into the program folder.");
        return false;
    }

    // Bind function pointers using memcpy to prevent ISO C pedantic warnings
    #define BIND_FUNC(name) \
        do { \
            FARPROC proc = GetProcAddress(nrsc5_api.dll_handle, "nrsc5_" #name); \
            if (!proc) { \
                log_error("Failed to bind nrsc5_" #name " from DLL."); \
                FreeLibrary(nrsc5_api.dll_handle); \
                nrsc5_api.dll_handle = NULL; \
                return false; \
            } \
            memcpy(&nrsc5_api.name, &proc, sizeof(nrsc5_api.name)); \
        } while (0)

    BIND_FUNC(get_version);
    BIND_FUNC(open_pipe);
    BIND_FUNC(close);
    BIND_FUNC(set_mode);
    BIND_FUNC(set_callback);
    BIND_FUNC(start);
    BIND_FUNC(stop);
    BIND_FUNC(pipe_samples_cu8);
    BIND_FUNC(pipe_samples_cs16);
    BIND_FUNC(program_type_name);
    BIND_FUNC(service_data_type_name);
    BIND_FUNC(alert_category_name);

    #undef BIND_FUNC

    log_debug("All NRSC5 symbols bound successfully.");
    return true;
}
#endif

// --- Configuration Constants ---
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
    AudioOutputContext* audio_out;
    unsigned int active_program;
    PipelineMode pipeline_mode;

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

    char filepath_buffer[APP_MAX_PATH_BUFFER];
} Nrsc5Context;

// --- CLI Configuration Storage ---
static struct {
    char* mode_str;
    char* aas_dir_arg;
    int program_id;
    Nrsc5Mode active_mode;
} s_nrsc5_config = {
    .mode_str = NULL,
    .aas_dir_arg = NULL,
    .program_id = -1, // Sentinel: -1 indicates "not set by user"
    .active_mode = NRSC5_MODE_CS16_FM
};

// --- Forward Declarations ---
static void nrsc5_event_callback(const nrsc5_event_t *event_payload, void *opaque);

// --- Helper: BER Stats ---
static void update_ber_stats(Nrsc5Context* context, float cber) {
    context->ber_sum += cber;
    context->ber_count += 1.0f;
    if (cber < context->ber_min) context->ber_min = cber;
    if (cber > context->ber_max) context->ber_max = cber;

    log_info("NRSC5: BER: %f, avg: %f, min: %f, max: %f",
             cber, context->ber_sum / context->ber_count, context->ber_min, context->ber_max);
}

/**
 * @brief Surgically sanitizes filenames from the air using a strict whitelist pattern.
 */
static void sanitize_aas_filename(const char* raw, char* out, size_t max_length) {
    size_t j = 0;
    if (!raw || max_length == 0) {
        if (max_length > 0) out[0] = '\0';
        return;
    }
    for (size_t i = 0; i < 255 && raw[i] != '\0' && j < (max_length - 1); i++) {
        unsigned char c = (unsigned char)raw[i];
        // Whitelist: Alphanumeric, dots, dashes, underscores only.
        if (isalnum(c) || c == '.' || c == '-' || c == '_') {
            out[j++] = (char)c;
        }
    }
    out[j] = '\0';
}

/**
 * @brief Safely dumps AAS/AAA files to disk using pre-allocated arena memory.
 */
static void dump_aas_file(Nrsc5Context* context, const nrsc5_event_t *event_payload) {
    if (is_shutdown_requested()) return;

    const char *name_raw = NULL;
    const uint8_t *data = NULL;
    unsigned int size = 0;
    unsigned int number = 0;

    switch (event_payload->event) {
        case NRSC5_EVENT_LOT:
            name_raw = event_payload->lot.name;
            data = event_payload->lot.data;
            size = event_payload->lot.size;
            number = event_payload->lot.lot;
            break;
        case NRSC5_EVENT_HERE_IMAGE:
            name_raw = event_payload->here_image.name;
            data = event_payload->here_image.data;
            size = event_payload->here_image.size;
            #if defined(_WIN32)
                number = (unsigned int)_mkgmtime64(event_payload->here_image.time_utc);
            #else
                number = (unsigned int)timegm(event_payload->here_image.time_utc);
            #endif
            break;
        default: return;
    }

    if (!name_raw || !data || size == 0) return;

    char safe_name[256];
    sanitize_aas_filename(name_raw, safe_name, sizeof(safe_name));

    snprintf(context->filepath_buffer, APP_MAX_PATH_BUFFER, "%s" PATH_SEPARATOR "%u_%s",
             s_nrsc5_config.aas_dir_arg, number, safe_name);

#ifdef _WIN32
    wchar_t w_path[APP_MAX_PATH_BUFFER];
    MultiByteToWideChar(CP_UTF8, 0, context->filepath_buffer, -1, w_path, APP_MAX_PATH_BUFFER);
    FILE *fp = _wfopen(w_path, L"wb");
#else
    FILE *fp = fopen(context->filepath_buffer, "wb");
#endif
    if (fp) {
        size_t written = fwrite(data, 1, size, fp);
        fclose(fp);
        if (written == size) {
            log_info("NRSC5: AAS file saved: %s (%u bytes)", context->filepath_buffer, size);
        }
    } else {
        log_warn("NRSC5: Failed to save AAS file '%s': %s", safe_name, strerror(errno));
    }
}

// --- NRSC5 Event Callback ---
static void nrsc5_event_callback(const nrsc5_event_t *event_payload, void *opaque) {
    Nrsc5Context* context = (Nrsc5Context*)opaque;
    const char* name_ptr = NULL;
    char time_str[64];

    switch (event_payload->event) {
        case NRSC5_EVENT_LOST_DEVICE:
            log_error("NRSC5: Lost device synchronization.");
            break;

        case NRSC5_EVENT_SYNC:
            log_info("NRSC5: Synchronized");
            log_info("NRSC5: Frequency offset: %.15g Hz", event_payload->sync.freq_offset);
            log_info("NRSC5: Primary service mode: %d", event_payload->sync.psmi);
            context->ber_min = 1.0f;
            context->ber_max = 0.0f;
            context->ber_sum = 0.0f;
            context->ber_count = 0.0f;
            break;

        case NRSC5_EVENT_LOST_SYNC:
            log_info("NRSC5: Lost synchronization");
            break;

        case NRSC5_EVENT_MER:
            log_info("NRSC5: MER: %.1f dB (lower), %.1f dB (upper)", event_payload->mer.lower, event_payload->mer.upper);
            break;

        case NRSC5_EVENT_BER:
            update_ber_stats(context, event_payload->ber.cber);
            break;

        case NRSC5_EVENT_HDC:
            if (event_payload->hdc.program == context->active_program) {
                context->audio_bytes += event_payload->hdc.count;
                context->audio_packets++;

                if (event_payload->hdc.flags & NRSC5_PKT_FLAGS_CRC_ERROR) {
                    context->audio_errors++;
                    context->total_audio_errors++;
                } else {
                    context->audio_packets_valid++;
                }

                if (context->audio_packets_valid >= 32) {
                    float kbps = (float)context->audio_bytes * 8.0f * NRSC5_SAMPLE_RATE_AUDIO /
                                 NRSC5_AUDIO_FRAME_SAMPLES / context->audio_packets_valid / 1000.0f;
                    log_info("NRSC5: Audio bit rate: %.1f kbps", kbps);
                    context->audio_packets_valid = 0;
                    context->audio_bytes = 0;
                }

                if (context->audio_packets >= 32) {
                    if (context->audio_errors > 0) {
                        log_warn("NRSC5: Audio CRC errors (recent): %d", context->audio_errors);
                        log_warn("NRSC5: Audio CRC errors (total): %d", context->total_audio_errors);
                    }
                    context->audio_packets = 0;
                    context->audio_errors = 0;
                }
            }
            break;

        case NRSC5_EVENT_AUDIO:
            if (event_payload->audio.program == context->active_program) {
                if (!event_payload->audio.data || event_payload->audio.count == 0 || event_payload->audio.count > 100000) return;
                size_t bytes = event_payload->audio.count * sizeof(int16_t);
                audio_output_write(context->audio_out, event_payload->audio.data, bytes, context->pipeline_mode);
            }
            break;

        case NRSC5_EVENT_ID3:
            if (event_payload->id3.program == context->active_program) {
                if (event_payload->id3.title)  log_info("NRSC5: Title: %s", event_payload->id3.title);
                if (event_payload->id3.artist) log_info("NRSC5: Artist: %s", event_payload->id3.artist);
                if (event_payload->id3.album)  log_info("NRSC5: Album: %s", event_payload->id3.album);
                if (event_payload->id3.genre)  log_info("NRSC5: Genre: %s", event_payload->id3.genre);

                if (event_payload->id3.xhdr.param >= 0) {
                    log_info("NRSC5: XHDR: %d %08X %d",
                             event_payload->id3.xhdr.param, event_payload->id3.xhdr.mime, event_payload->id3.xhdr.lot);
                }
            }
            break;

        case NRSC5_EVENT_SIG:
            for (nrsc5_sig_service_t* svc = event_payload->sig.services; svc != NULL; svc = svc->next) {
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
            log_info("NRSC5: Station name: %s", SAFE_STR(event_payload->station_name.name));
            break;

        case NRSC5_EVENT_STATION_SLOGAN:
            log_info("NRSC5: Slogan: %s", SAFE_STR(event_payload->station_slogan.slogan));
            break;

        case NRSC5_EVENT_STATION_MESSAGE:
            log_info("NRSC5: Message: %s", SAFE_STR(event_payload->station_message.message));
            break;

        case NRSC5_EVENT_STATION_ID:
            log_info("NRSC5: Country: %s, FCC facility ID: %d",
                     SAFE_STR(event_payload->station_id.country_code), event_payload->station_id.fcc_facility_id);
            break;

        case NRSC5_EVENT_STATION_LOCATION:
            log_info("NRSC5: Station location: %.4f, %.4f, %dm",
                     event_payload->station_location.latitude,
                     event_payload->station_location.longitude,
                     event_payload->station_location.altitude);
            break;

        case NRSC5_EVENT_AUDIO_SERVICE_DESCRIPTOR:
            nrsc5_program_type_name(event_payload->asd.type, &name_ptr);
            log_info("NRSC5: Audio program %d: %s, type: %s, sound experience %d",
                     event_payload->asd.program,
                     event_payload->asd.access == NRSC5_ACCESS_PUBLIC ? "public" : "restricted",
                     SAFE_STR(name_ptr), event_payload->asd.sound_exp);
            break;

        case NRSC5_EVENT_DATA_SERVICE_DESCRIPTOR:
            nrsc5_service_data_type_name(event_payload->dsd.type, &name_ptr);
            log_info("NRSC5: Data service: %s, type: %s, MIME type %03x",
                     event_payload->dsd.access == NRSC5_ACCESS_PUBLIC ? "public" : "restricted",
                     SAFE_STR(name_ptr), event_payload->dsd.mime_type);
            break;

        case NRSC5_EVENT_AUDIO_SERVICE:
            nrsc5_program_type_name(event_payload->audio_service.type, &name_ptr);
            log_info("NRSC5: Audio service %d: %s, type: %s, codec: %d, blend: %d, gain: %d dB, delay: %d, latency: %d",
                     event_payload->audio_service.program,
                     event_payload->audio_service.access == NRSC5_ACCESS_PUBLIC ? "public" : "restricted",
                     SAFE_STR(name_ptr),
                     event_payload->audio_service.codec_mode,
                     event_payload->audio_service.blend_control,
                     event_payload->audio_service.digital_audio_gain,
                     event_payload->audio_service.common_delay,
                     event_payload->audio_service.latency);
            break;

        case NRSC5_EVENT_EMERGENCY_ALERT:
            if (event_payload->emergency_alert.message) {
                char alert_buf[1024];
                int offset = 0;
                offset += snprintf(alert_buf + offset, sizeof(alert_buf) - offset, "Category=[");
                if (event_payload->emergency_alert.category1 >= 1) {
                    nrsc5_alert_category_name(event_payload->emergency_alert.category1, &name_ptr);
                    offset += snprintf(alert_buf + offset, sizeof(alert_buf) - offset, "%s", SAFE_STR(name_ptr));
                }
                if (event_payload->emergency_alert.category2 >= 1) {
                    nrsc5_alert_category_name(event_payload->emergency_alert.category2, &name_ptr);
                    offset += snprintf(alert_buf + offset, sizeof(alert_buf) - offset, ", %s", SAFE_STR(name_ptr));
                }
                offset += snprintf(alert_buf + offset, sizeof(alert_buf) - offset, "] ");

                switch (event_payload->emergency_alert.location_format) {
                    case NRSC5_LOCATION_FORMAT_SAME: offset += snprintf(alert_buf + offset, sizeof(alert_buf) - offset, "SAME="); break;
                    case NRSC5_LOCATION_FORMAT_FIPS: offset += snprintf(alert_buf + offset, sizeof(alert_buf) - offset, "FIPS="); break;
                    case NRSC5_LOCATION_FORMAT_ZIP:  offset += snprintf(alert_buf + offset, sizeof(alert_buf) - offset, "ZIP="); break;
                }
                offset += snprintf(alert_buf + offset, sizeof(alert_buf) - offset, "[");
                for (int i = 0; i < event_payload->emergency_alert.num_locations; i++) {
                    if (i > 0) offset += snprintf(alert_buf + offset, sizeof(alert_buf) - offset, ", ");
                    offset += snprintf(alert_buf + offset, sizeof(alert_buf) - offset, "%d", event_payload->emergency_alert.locations[i]);
                }
                offset += snprintf(alert_buf + offset, sizeof(alert_buf) - offset, "]");
                log_info("NRSC5: Alert: %s %s", alert_buf, SAFE_STR(event_payload->emergency_alert.message));
            } else {
                log_info("NRSC5: Alert ended");
            }
            break;

        case NRSC5_EVENT_HERE_IMAGE:
            strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%SZ", event_payload->here_image.time_utc);
            log_info("NRSC5: HERE Image: type=%s, seq=%d, n1=%d, n2=%d, time=%s, name=%s, size=%d",
                     event_payload->here_image.image_type == NRSC5_HERE_IMAGE_TRAFFIC ? "TRAFFIC" : "WEATHER",
                     event_payload->here_image.seq, event_payload->here_image.n1, event_payload->here_image.n2, time_str,
                     SAFE_STR(event_payload->here_image.name), event_payload->here_image.size);
            if (s_nrsc5_config.aas_dir_arg) dump_aas_file(context, event_payload);
            break;

        case NRSC5_EVENT_LOT:
            strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%SZ", event_payload->lot.expiry_utc);
            log_info("NRSC5: LOT file: port=%04X lot=%d name=%s size=%d mime=%08X expiry=%s",
                     event_payload->lot.component->data.port, event_payload->lot.lot, SAFE_STR(event_payload->lot.name),
                     event_payload->lot.size, event_payload->lot.mime, time_str);
            if (s_nrsc5_config.aas_dir_arg) dump_aas_file(context, event_payload);
            break;

        default:
            break;
    }
}

// --- Helper: dBFS Calculation ---
static double calculate_buffer_power(const void* buffer, unsigned int frames, Nrsc5Mode mode) {
    double accum_mag_sq_sum = 0.0;

    if (mode == NRSC5_MODE_CS16_FM || mode == NRSC5_MODE_CS16_AM) {
        const int16_t* samples = (const int16_t*)buffer;
        for (unsigned int i = 0; i < frames; i++) {
            float i_val = (float)samples[2*i] * (1.0f / 32768.0f);
            float q_val = (float)samples[2*i+1] * (1.0f / 32768.0f);
            accum_mag_sq_sum += (double)(i_val*i_val + q_val*q_val);
        }
    } else {
        const uint8_t* samples = (const uint8_t*)buffer;
        for (unsigned int i = 0; i < frames; i++) {
            float i_val = ((float)samples[2*i] - 127.5f) * (1.0f / 128.0f);
            float q_val = ((float)samples[2*i+1] - 127.5f) * (1.0f / 128.0f);
            accum_mag_sq_sum += (double)(i_val*i_val + q_val*q_val);
        }
    }

    return accum_mag_sq_sum;
}

// --- Module Interface Implementation ---

static bool output_nrsc5_initialize(ModuleContext* context) {
    AppContext* app = context->app;

#ifdef _WIN32
    if (!load_nrsc5_dll()) {
        return false;
    }
#endif

    Nrsc5Context* nrsc5_decoder = (Nrsc5Context*)mem_arena_alloc(&app->pipeline.setup_arena, sizeof(Nrsc5Context), true);
    if (!nrsc5_decoder) return false;
    app->module.output_private_data = nrsc5_decoder;

    // Direct API call
    const char* ver_str = NULL;
    nrsc5_get_version(&ver_str);
    log_info("NRSC5: Library version %s", SAFE_STR(ver_str));

    nrsc5_decoder->audio_out = audio_output_create(app, NRSC5_AUDIO_SAMPLE_RATE, NRSC5_AUDIO_CHANNELS, app->module.source_info.demod_audio_buffer_size);
    if (!nrsc5_decoder->audio_out) return false;
    log_info("NRSC5: Audio device initialized (%d Hz, %d Channels).", NRSC5_AUDIO_SAMPLE_RATE, NRSC5_AUDIO_CHANNELS);

    nrsc5_decoder->pipeline_mode = app->pipeline_mode;

    // Initialize NRSC5 Instance
    log_debug("NRSC5: Opening pipe...");
    if (nrsc5_open_pipe(&nrsc5_decoder->nrsc5_inst) != 0) {
        log_fatal("NRSC5: Failed to open decoder pipe.");
        return false;
    }

    // Set and log active mode
    int decoder_mode = NRSC5_MODE_FM;
    if (s_nrsc5_config.active_mode == NRSC5_MODE_CS16_AM ||
        s_nrsc5_config.active_mode == NRSC5_MODE_CU8_AM) {
        decoder_mode = NRSC5_MODE_AM;
    }

    if (nrsc5_set_mode(nrsc5_decoder->nrsc5_inst, decoder_mode) != 0) {
        log_fatal("NRSC5: Failed to set decoder mode.");
        return false;
    }

    log_info("NRSC5: Using mode: %s", s_nrsc5_config.mode_str);

    // Set Callback
    nrsc5_decoder->active_program = (unsigned int)s_nrsc5_config.program_id;
    nrsc5_set_callback(nrsc5_decoder->nrsc5_inst, nrsc5_event_callback, nrsc5_decoder);

    // Start Decoder Thread
    log_debug("NRSC5: Starting decoder thread...");
    nrsc5_start(nrsc5_decoder->nrsc5_inst);

        if (s_nrsc5_config.aas_dir_arg) {
        struct stat st;
        if (stat(s_nrsc5_config.aas_dir_arg, &st) != 0 || !S_ISDIR(st.st_mode)) {
            log_error("NRSC5: AAS directory '%s' is invalid or not a directory.", s_nrsc5_config.aas_dir_arg);
            return false;
        }
    }

    return true;
}

static void output_nrsc5_reset(ModuleContext* context) { (void)context; }

static void output_nrsc5_flush(ModuleContext* context) {
    Nrsc5Context* nrsc5_decoder = (Nrsc5Context*)context->app->module.output_private_data;
    if (is_shutdown_requested()) {
        audio_output_clear(nrsc5_decoder->audio_out);
    } else {
        audio_output_drain(nrsc5_decoder->audio_out);
    }
}

static size_t output_nrsc5_write_chunk(ModuleContext* context, const void* buffer, size_t input_bytes) {
    AppContext* app = context->app;
    Nrsc5Context* nrsc5_decoder = (Nrsc5Context*)app->module.output_private_data;
    if (input_bytes == 0) return 0;

    unsigned int frames = input_bytes / app->module.output_bytes_per_iq_sample;
    unsigned int num_scalars = frames * 2;

    // --- dBFS Calculation ---
    static size_t stat_counter = 0;
    static double accum_mag_sq_sum = 0.0;
    static size_t stat_rate_threshold = 0;

    if (stat_rate_threshold == 0) {
        stat_rate_threshold = (size_t)(context->config->baseband_sample_rate.rate_hz * CONSOLE_UPDATE_INTERVAL_SEC);
        if (stat_rate_threshold == 0) stat_rate_threshold = (size_t)(744187 * CONSOLE_UPDATE_INTERVAL_SEC);
    }

    accum_mag_sq_sum += calculate_buffer_power(buffer, frames, s_nrsc5_config.active_mode);
    stat_counter += frames;

    // Print dBFS according to global console interval
    if (stat_counter >= stat_rate_threshold) {
        double avg_power = accum_mag_sq_sum / (double)stat_counter;
        float dbfs = utility_calculate_dbfs((float)avg_power);

        log_info("NRSC5: dBFS: %.1f", dbfs);

        stat_counter = 0;
        accum_mag_sq_sum = 0.0;
    }

    int res = 0;
    switch (s_nrsc5_config.active_mode) {
        case NRSC5_MODE_CU8_FM:
        case NRSC5_MODE_CU8_AM:
            res = nrsc5_pipe_samples_cu8(nrsc5_decoder->nrsc5_inst, (uint8_t*)buffer, num_scalars);
            break;
        case NRSC5_MODE_CS16_FM:
        case NRSC5_MODE_CS16_AM:
            res = nrsc5_pipe_samples_cs16(nrsc5_decoder->nrsc5_inst, (int16_t*)buffer, num_scalars);
            break;
        default: break;
    }
    if (res != 0) log_error("NRSC5: Failed to pipe samples to decoder.");
    return input_bytes;
}

static void output_nrsc5_cleanup(ModuleContext* context) {
    AppContext* app = context->app;
    if (!app->module.output_private_data) return;
    Nrsc5Context* nrsc5_decoder = (Nrsc5Context*)app->module.output_private_data;

    audio_output_destroy(nrsc5_decoder->audio_out);

    if (nrsc5_decoder->nrsc5_inst) {
        nrsc5_stop(nrsc5_decoder->nrsc5_inst);
        nrsc5_close(nrsc5_decoder->nrsc5_inst);
        nrsc5_decoder->nrsc5_inst = NULL;
    }
}

static bool output_nrsc5_validate_options(AppContext* app) {
    AppConfig* config = app ? (AppConfig*)app->config : NULL;
    // 1. Resolve Mode
    if (!s_nrsc5_config.mode_str) {
        double active_freq = 0.0;

        // The Fallback Shield
        if (config->iq_file_metadata.rf_freq_provided) {
            active_freq = config->iq_file_metadata.rf_freq_hz;
        } else {
            active_freq = config->sdr_general.rf_freq_hz;
        }

        // The Smart Context Check
        if (active_freq >= 87500000.0 && active_freq <= 108000000.0) {
            // FM Broadcast Band (87.5 MHz to 108 MHz)
            s_nrsc5_config.mode_str = "cs16-fm";
        } else if (active_freq >= 530000.0 && active_freq <= 1710000.0) {
            // AM Broadcast Band (530 kHz to 1710 kHz)
            s_nrsc5_config.mode_str = "cs16-am";
        } else {
            // Unrecognized frequency (or 0.0 Hz). Default to the most common mode.
            s_nrsc5_config.mode_str = "cs16-fm";
        }

        log_info("NRSC5: No mode specified, defaulting to '%s'.", s_nrsc5_config.mode_str);
    }

    if (strcasecmp(s_nrsc5_config.mode_str, "cs16-fm") == 0) {
        s_nrsc5_config.active_mode = NRSC5_MODE_CS16_FM;
    } else if (strcasecmp(s_nrsc5_config.mode_str, "cs16-am") == 0) {
        s_nrsc5_config.active_mode = NRSC5_MODE_CS16_AM;
    } else if (strcasecmp(s_nrsc5_config.mode_str, "cu8-fm") == 0) {
        s_nrsc5_config.active_mode = NRSC5_MODE_CU8_FM;
    } else if (strcasecmp(s_nrsc5_config.mode_str, "cu8-am") == 0) {
        s_nrsc5_config.active_mode = NRSC5_MODE_CU8_AM;
    } else {
        log_error("NRSC5: Invalid mode '%s'. Valid modes: cs16-fm, cs16-am, cu8-fm, cu8-am.", s_nrsc5_config.mode_str);
        return false;
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

    // Check User Settings
    if (config->baseband_sample_rate.rate_hz != required_rate) {
        if (config->baseband_sample_rate.provided) {
            log_error("NRSC5: Invalid baseband rate %.15g Hz. The selected mode requires exactly %.15g Hz.",
                     config->baseband_sample_rate.rate_hz, required_rate);
            return false;
        }
        config->baseband_sample_rate.rate_hz = required_rate;
    }

    if (config->baseband_sample_format.format != required_format) {
        if (config->baseband_sample_format.provided) {
            log_error("NRSC5: Invalid baseband format '%s'. The selected mode requires the '%s' sample format.",
                     get_format_info_by_enum(config->baseband_sample_format.format)->name_str,
                     get_format_info_by_enum(required_format)->name_str);
            return false;
        }
        log_info("NRSC5: Mode '%s' requires '%s' baseband sample format. Automatically configuring.",
                 s_nrsc5_config.mode_str,
                 get_format_info_by_enum(required_format)->name_str);
        config->baseband_sample_format.format = required_format;
        config->baseband_sample_format.format_str = (char*)get_format_info_by_enum(required_format)->name_str;
    }

    // 3. Validate Program ID
    if (s_nrsc5_config.program_id == -1) {
        log_error("NRSC5: Missing required option: --nrsc5-program <0-7>.");
        return false;
    }

    if (s_nrsc5_config.program_id < 0 || s_nrsc5_config.program_id > 7) {
        log_error("NRSC5: Invalid program ID %d. Must be 0-7.", s_nrsc5_config.program_id);
        return false;
    }

    return true;
}

static void output_nrsc5_get_summary_info(const ModuleContext* context, OutputSummaryInfo* info) {
    (void)context;
    utility_add_summary_item(info, "Output Type", "NRSC5 (HD Radio Player)");

    utility_add_summary_item(info, "Mode", "%s", s_nrsc5_config.mode_str);
    utility_add_summary_item(info, "Program", "%d (HD%d)", s_nrsc5_config.program_id, s_nrsc5_config.program_id + 1);
}

// --- CLI Options ---
static const struct argparse_option output_nrsc5_cli_options[] = {
    OPT_GROUP("NRSC5 Output (nrsc5)"),
    OPT_STRING(0, "nrsc5-mode", &s_nrsc5_config.mode_str, "Set decoder mode {cs16-fm|cs16-am|cu8-fm|cu8-am}. (Default: cs16-fm)", NULL, 0, 0),
    OPT_INTEGER(0, "nrsc5-program", &s_nrsc5_config.program_id, "Select initial HD program/subchannel (0-7). (Required) Press keys 0-7 during playback to switch.", NULL, 0, 0),
    OPT_STRING(0, "nrsc5-aas-dir", &s_nrsc5_config.aas_dir_arg, "Directory to dump AAS files (logos, maps, etc).", NULL, 0, 0),
};

const struct argparse_option* output_nrsc5_get_cli_options(int* count) {
    *count = sizeof(output_nrsc5_cli_options) / sizeof(output_nrsc5_cli_options[0]);
    return output_nrsc5_cli_options;
}

static void output_nrsc5_on_keypress(ModuleContext* context, int key) {
    if (key >= '0' && key <= '7') {
        unsigned int new_program = key - '0';
        Nrsc5Context* nrsc5_decoder = (Nrsc5Context*)context->app->module.output_private_data;
        if (nrsc5_decoder->active_program != new_program) {
            nrsc5_decoder->active_program = new_program;
            audio_output_clear(nrsc5_decoder->audio_out);

            // Inject 138ms of silence (Exactly 3 HDC frames) to build a pre-buffer and prevent stuttering
            size_t silence_frames = (size_t)(NRSC5_AUDIO_SAMPLE_RATE * 0.138);
            size_t silence_bytes = silence_frames * NRSC5_AUDIO_CHANNELS * sizeof(int16_t);
            int16_t* silence = calloc(1, silence_bytes);
            if (silence) {
                audio_output_write(nrsc5_decoder->audio_out, silence, silence_bytes, nrsc5_decoder->pipeline_mode);
                free(silence);
            }

            log_info("NRSC5: Switched to program %u (HD%u)", new_program, new_program + 1);
        }
    }
}

// --- The V-Table ---
static OutputModuleInterface s_output_nrsc5_api = {
    .validate_options = output_nrsc5_validate_options,
    .get_cli_options = output_nrsc5_get_cli_options,
    .initialize = output_nrsc5_initialize,
    .write_chunk = output_nrsc5_write_chunk,
    .reset = output_nrsc5_reset,
    .flush = output_nrsc5_flush,
    .cleanup = output_nrsc5_cleanup,
    .get_summary_info = output_nrsc5_get_summary_info,
    .on_keypress = output_nrsc5_on_keypress,
};

OutputModuleInterface* output_nrsc5_get_module_api(void) {
    return &s_output_nrsc5_api;
}
