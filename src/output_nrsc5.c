/**
 * @file output_nrsc5.c
 * @brief Implements the NRSC5 (HD Radio) output module using CF32 floats.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>

#include "output_nrsc5.h"
#include "signal_handler.h"
#include "audio_output_functions.h"
#include "module.h"
#include "app_context.h"
#include "log.h"
#include "platform.h"
#include "ring_buffer.h"
#include "utilities.h"
#include "sample_format_table.h"
#include "nrsc5.h"
#include "queue.h"
#include "constants.h"

#define NRSC5_AUDIO_CHANNELS 2
#define NRSC5_AUDIO_SAMPLE_RATE 44100
#define SAFE_STR(s) ((s) ? (s) : "(null)")

#ifdef _WIN32
#include <windows.h>
#define PATH_SEPARATOR "\\"
#else
#include <unistd.h>
#define PATH_SEPARATOR "/"
#endif

#ifdef _WIN32
typedef struct {
    HINSTANCE dll_handle;
    void (*get_version)(const char **version);
    int  (*open_pipe)(nrsc5_t **nrsc5);
    void (*close)(nrsc5_t *nrsc5);
    int  (*set_mode)(nrsc5_t *nrsc5, int mode);
    void (*set_callback)(nrsc5_t *nrsc5, nrsc5_callback_t callback, void *opaque);
    int  (*start)(nrsc5_t *nrsc5);
    int  (*stop)(nrsc5_t *nrsc5);
    int  (*pipe_samples_cf32)(nrsc5_t *nrsc5, const float *samples, unsigned int length);
    void (*program_type_name)(unsigned int pty, const char **name);
    void (*service_data_type_name)(unsigned int type, const char **name);
    void (*alert_category_name)(unsigned int category, const char **name);
} nrsc5_winapi;

static nrsc5_winapi nrsc5_api = { NULL };

#define nrsc5_get_version             nrsc5_api.get_version
#define nrsc5_open_pipe               nrsc5_api.open_pipe
#define nrsc5_close                   nrsc5_api.close
#define nrsc5_set_mode                nrsc5_api.set_mode
#define nrsc5_set_callback            nrsc5_api.set_callback
#define nrsc5_start                   nrsc5_api.start
#define nrsc5_stop                    nrsc5_api.stop
#define nrsc5_pipe_samples_cf32       nrsc5_api.pipe_samples_cf32
#define nrsc5_program_type_name       nrsc5_api.program_type_name
#define nrsc5_service_data_type_name  nrsc5_api.service_data_type_name
#define nrsc5_alert_category_name     nrsc5_api.alert_category_name

static bool load_nrsc5_dll(void) {
    if (nrsc5_api.dll_handle) return true;

    log_debug("Attempting to dynamically load the NRSC5 library...");
    nrsc5_api.dll_handle = LoadLibraryA("libnrsc5.dll");

    if (!nrsc5_api.dll_handle) {
        log_error("NRSC5: 'libnrsc5.dll' is missing in the program folder.");
        log_error("NRSC5: To enable the NRSC5 output module, please see compilation instructions at:");
        log_error("NRSC5:   https://github.com/theori-io/nrsc5#building-for-windows");
        log_error("NRSC5: Once compiled, copy 'libnrsc5.dll' into the program folder.");
        return false;
    }

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
    BIND_FUNC(pipe_samples_cf32);
    BIND_FUNC(program_type_name);
    BIND_FUNC(service_data_type_name);
    BIND_FUNC(alert_category_name);

    #undef BIND_FUNC

    log_debug("All NRSC5 symbols bound successfully.");
    return true;
}
#endif

typedef struct {
    nrsc5_t* nrsc5_instance;
    AudioOutputContext* audio_out;
    unsigned int active_program;
    PipelineMode pipeline_mode;

    float input_samplerate;

    float ber_min;
    float ber_max;
    float ber_sum;
    float ber_count;

    unsigned int audio_packets;       
    unsigned int audio_packets_valid; 
    unsigned int audio_bytes;         
    unsigned int audio_errors;        
    unsigned int total_audio_errors;  

    size_t stat_counter;
    double accum_mag_sq_sum;
    size_t stat_rate_threshold;

    char filepath_buffer[APP_MAX_PATH_BUFFER];
} nrsc5_context;

static struct {
    char* band_str;          
    char* aas_dir_arg;
    int program_id;          
    int active_mode;         
} s_nrsc5_config = {
    .band_str = NULL,
    .aas_dir_arg = NULL,
    .program_id = -1,
    .active_mode = NRSC5_MODE_FM
};

static void nrsc5_event_callback(const nrsc5_event_t *event_payload, void *opaque);

static void update_ber_stats(nrsc5_context* nrsc5_decoder, float cber) {
    nrsc5_decoder->ber_sum += cber;
    nrsc5_decoder->ber_count += 1.0f;
    if (cber < nrsc5_decoder->ber_min) nrsc5_decoder->ber_min = cber;
    if (cber > nrsc5_decoder->ber_max) nrsc5_decoder->ber_max = cber;

    log_info("NRSC5: BER: %f, avg: %f, min: %f, max: %f",
             cber, nrsc5_decoder->ber_sum / nrsc5_decoder->ber_count, nrsc5_decoder->ber_min, nrsc5_decoder->ber_max);
}

static void sanitize_aas_filename(const char* raw, char* out, size_t max_length) {
    size_t j = 0;
    if (!raw || max_length == 0) {
        if (max_length > 0) out[0] = '\0';
        return;
    }
    for (size_t i = 0; i < 255 && raw[i] != '\0' && j < (max_length - 1); i++) {
        unsigned char c = (unsigned char)raw[i];
        if (isalnum(c) || c == '.' || c == '-' || c == '_') {
            out[j++] = (char)c;
        }
    }
    out[j] = '\0';
}

static void dump_aas_file(nrsc5_context* nrsc5_decoder, const nrsc5_event_t *event_payload) {
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
            if (event_payload->here_image.time_utc) {
                #if defined(_WIN32)
                    number = (unsigned int)_mkgmtime64(event_payload->here_image.time_utc);
                #else
                    number = (unsigned int)timegm(event_payload->here_image.time_utc);
                #endif
            } else {
                number = (unsigned int)time(NULL);
            }
            break;
        default: return;
    }

    if (!name_raw || !data || size == 0) return;

    char safe_name[256];
    sanitize_aas_filename(name_raw, safe_name, sizeof(safe_name));

    snprintf(nrsc5_decoder->filepath_buffer, APP_MAX_PATH_BUFFER, "%s" PATH_SEPARATOR "%u_%s",
             s_nrsc5_config.aas_dir_arg, number, safe_name);

#ifdef _WIN32
    wchar_t w_path[APP_MAX_PATH_BUFFER];
    MultiByteToWideChar(CP_UTF8, 0, nrsc5_decoder->filepath_buffer, -1, w_path, APP_MAX_PATH_BUFFER);
    FILE *fp = _wfopen(w_path, L"wb");
#else
    FILE *fp = fopen(nrsc5_decoder->filepath_buffer, "wb");
#endif
    if (fp) {
        size_t written = fwrite(data, 1, size, fp);
        fclose(fp);
        if (written == size) {
            log_info("NRSC5: AAS file saved: %s (%u bytes)", nrsc5_decoder->filepath_buffer, size);
        }
    } else {
        log_warn("NRSC5: Failed to save AAS file '%s': %s", safe_name, strerror(errno));
    }
}

static void nrsc5_event_callback(const nrsc5_event_t *event_payload, void *opaque) {
    nrsc5_context* nrsc5_decoder = (nrsc5_context*)opaque;
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
            nrsc5_decoder->ber_min = 1.0f;
            nrsc5_decoder->ber_max = 0.0f;
            nrsc5_decoder->ber_sum = 0.0f;
            nrsc5_decoder->ber_count = 0.0f;
            break;

        case NRSC5_EVENT_LOST_SYNC:
            log_info("NRSC5: Lost synchronization");
            break;

        case NRSC5_EVENT_MER:
            log_info("NRSC5: MER: %.1f dB (lower), %.1f dB (upper)", event_payload->mer.lower, event_payload->mer.upper);
            break;

        case NRSC5_EVENT_BER:
            update_ber_stats(nrsc5_decoder, event_payload->ber.cber);
            break;

        case NRSC5_EVENT_HDC:
            if (event_payload->hdc.program == nrsc5_decoder->active_program) {
                nrsc5_decoder->audio_bytes += event_payload->hdc.count;
                nrsc5_decoder->audio_packets++;

                if (event_payload->hdc.flags & NRSC5_PKT_FLAGS_CRC_ERROR) {
                    nrsc5_decoder->audio_errors++;
                    nrsc5_decoder->total_audio_errors++;
                } else {
                    nrsc5_decoder->audio_packets_valid++;
                }

                if (nrsc5_decoder->audio_packets_valid >= 32) {
                    float kbps = (float)nrsc5_decoder->audio_bytes * 8.0f * NRSC5_SAMPLE_RATE_AUDIO /
                                 NRSC5_AUDIO_FRAME_SAMPLES / nrsc5_decoder->audio_packets_valid / 1000.0f;
                    log_info("NRSC5: Audio bit rate: %.1f kbps", kbps);
                    nrsc5_decoder->audio_packets_valid = 0;
                    nrsc5_decoder->audio_bytes = 0;
                }

                if (nrsc5_decoder->audio_packets >= 32) {
                    if (nrsc5_decoder->audio_errors > 0) {
                        log_warn("NRSC5: Audio CRC errors (recent): %d", nrsc5_decoder->audio_errors);
                        log_warn("NRSC5: Audio CRC errors (total): %d", nrsc5_decoder->total_audio_errors);
                    }
                    nrsc5_decoder->audio_packets = 0;
                    nrsc5_decoder->audio_errors = 0;
                }
            }
            break;

        case NRSC5_EVENT_AUDIO:
            if (event_payload->audio.program == nrsc5_decoder->active_program) {
                if (!event_payload->audio.data || event_payload->audio.count == 0 || event_payload->audio.count > 100000) return;
                size_t bytes = event_payload->audio.count * sizeof(int16_t);
                audio_output_write(nrsc5_decoder->audio_out, event_payload->audio.data, bytes, nrsc5_decoder->pipeline_mode);
            }
            break;

        case NRSC5_EVENT_ID3:
            if (event_payload->id3.program == nrsc5_decoder->active_program) {
                if (event_payload->id3.title)  log_info("NRSC5: Title: %s", event_payload->id3.title);
                if (event_payload->id3.artist) log_info("NRSC5: Artist: %s", event_payload->id3.artist);
                if (event_payload->id3.album)  log_info("NRSC5: Album: %s", event_payload->id3.album);
                if (event_payload->id3.genre)  log_info("NRSC5: Genre: %s", event_payload->id3.genre);

                if (event_payload->id3.xhdr.param >= 0) {
                    log_info("NRSC5: XHDR: %d %08X %d",
                             event_payload->id3.xhdr.param, 
                             event_payload->id3.xhdr.mime, 
                             event_payload->id3.xhdr.lot);
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

                const char* format_label = NULL;
                switch (event_payload->emergency_alert.location_format) {
                    case NRSC5_LOCATION_FORMAT_SAME: format_label = "SAME="; break;
                    case NRSC5_LOCATION_FORMAT_FIPS: format_label = "FIPS="; break;
                    case NRSC5_LOCATION_FORMAT_ZIP:  format_label = "ZIP=";  break;
                    default:                                                 break;
                }

                if (format_label) {
                    offset += snprintf(alert_buf + offset, sizeof(alert_buf) - offset, "%s", format_label);
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
            if (event_payload->here_image.time_utc) {
                strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%SZ", event_payload->here_image.time_utc);
            } else {
                snprintf(time_str, sizeof(time_str), "UNKNOWN_TIME");
            }
            log_info("NRSC5: HERE Image: type=%s, seq=%d, n1=%d, n2=%d, time=%s, name=%s, size=%d",
                     event_payload->here_image.image_type == NRSC5_HERE_IMAGE_TRAFFIC ? "TRAFFIC" : "WEATHER",
                     event_payload->here_image.seq, event_payload->here_image.n1, event_payload->here_image.n2, time_str,
                     SAFE_STR(event_payload->here_image.name), event_payload->here_image.size);
            if (s_nrsc5_config.aas_dir_arg) dump_aas_file(nrsc5_decoder, event_payload);
            break;

        case NRSC5_EVENT_LOT:
            if (event_payload->lot.expiry_utc) {
                strftime(time_str, sizeof(time_str), "%Y-%m-%dT%H:%M:%SZ", event_payload->lot.expiry_utc);
            } else {
                snprintf(time_str, sizeof(time_str), "UNKNOWN_TIME");
            }
            log_info("NRSC5: LOT file: port=%04X lot=%d name=%s size=%d mime=%08X expiry=%s",
                     event_payload->lot.component->data.port, event_payload->lot.lot, SAFE_STR(event_payload->lot.name),
                     event_payload->lot.size, event_payload->lot.mime, time_str);
            if (s_nrsc5_config.aas_dir_arg) dump_aas_file(nrsc5_decoder, event_payload);
            break;

        case NRSC5_EVENT_EXCITER_INFO:
            log_info("NRSC5: Exciter: manufacturer=%s, core=%d.%d.%d.%d, status=%d, importer=%s",
                     SAFE_STR(event_payload->exciter_info.manufacturer_id),
                     event_payload->exciter_info.core_version[0],
                     event_payload->exciter_info.core_version[1],
                     event_payload->exciter_info.core_version[2],
                     event_payload->exciter_info.core_version[3],
                     event_payload->exciter_info.core_status,
                     event_payload->exciter_info.importer_connected ? "connected" : "disconnected");
            break;

        case NRSC5_EVENT_IMPORTER_INFO:
            log_info("NRSC5: Importer: manufacturer=%s, core=%d.%d.%d.%d, status=%d",
                     SAFE_STR(event_payload->importer_info.manufacturer_id),
                     event_payload->importer_info.core_version[0],
                     event_payload->importer_info.core_version[1],
                     event_payload->importer_info.core_version[2],
                     event_payload->importer_info.core_version[3],
                     event_payload->importer_info.manufacturer_status);
            break;

        case NRSC5_EVENT_LEAP_SECOND_OFFSET:
            log_info("NRSC5: GPS Leap Seconds: current=%d, pending=%d (ALFN=%u)",
                     event_payload->leap_second_offset.current_offset,
                     event_payload->leap_second_offset.pending_offset,
                     event_payload->leap_second_offset.pending_alfn);
            break;

        case NRSC5_EVENT_LOCAL_TIME:
            log_info("NRSC5: Local Time: offset=%d min, regional DST=%d, local DST=%d, DST schedule=%d",
                     event_payload->local_time.utc_offset,
                     event_payload->local_time.dst_regional,
                     event_payload->local_time.dst_local,
                     event_payload->local_time.dst_schedule);
            break;

        default:
            break;
    }
}

static double calculate_buffer_power(const float complex* samples, unsigned int frames) {
    double accum_mag_sq_sum = 0.0;
    for (unsigned int i = 0; i < frames; i++) {
        float r = crealf(samples[i]);
        float im = cimagf(samples[i]);
        accum_mag_sq_sum += (double)(r * r + im * im);
    }
    return accum_mag_sq_sum;
}

static bool output_nrsc5_validate_options(AppContext* app) {
    AppConfig* config = app ? (AppConfig*)app->config : NULL;
    
    config->baseband_sample_format.format = CF32;

    if (!s_nrsc5_config.band_str) {
        double active_freq = config->iq_file_metadata.rf_freq_provided ? 
                             config->iq_file_metadata.rf_freq_hz : config->sdr_general.rf_freq_hz;

        if (active_freq >= 530000.0 && active_freq <= 1710000.0) {
            s_nrsc5_config.active_mode = NRSC5_MODE_AM;
        } else {
            s_nrsc5_config.active_mode = NRSC5_MODE_FM;
        }
        log_info("NRSC5: No band specified, defaulting to %s.", 
                 (s_nrsc5_config.active_mode == NRSC5_MODE_FM) ? "FM" : "AM");
    } else {
        if (strcasecmp(s_nrsc5_config.band_str, "fm") == 0) {
            s_nrsc5_config.active_mode = NRSC5_MODE_FM;
        } else if (strcasecmp(s_nrsc5_config.band_str, "am") == 0) {
            s_nrsc5_config.active_mode = NRSC5_MODE_AM;
        } else {
            log_error("NRSC5: Invalid band '%s'. Valid bands: fm, am.", s_nrsc5_config.band_str);
            return false;
        }
    }

    double required_rate = (s_nrsc5_config.active_mode == NRSC5_MODE_FM) ? 744187.5 : 46511.71875;

    if (config->baseband_sample_rate.provided) {
        if (config->baseband_sample_rate.rate_hz != required_rate) {
            log_error("NRSC5: Invalid rate %.15g Hz. Selected band requires exactly %.15g Hz.",
                     config->baseband_sample_rate.rate_hz, required_rate);
            return false;
        }
    } else {
        config->baseband_sample_rate.rate_hz = required_rate;
    }

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

static bool output_nrsc5_initialize(ModuleContext* context) {
    AppContext* app = context->app;

#ifdef _WIN32
    if (!load_nrsc5_dll()) {
        return false;
    }
#endif

    nrsc5_context* nrsc5_decoder = (nrsc5_context*)mem_arena_alloc(&app->pipeline.setup_arena, sizeof(nrsc5_context), true);
    if (!nrsc5_decoder) return false;
    app->module.output_private_data = nrsc5_decoder;

    const char* ver_str = NULL;
    nrsc5_get_version(&ver_str);
    log_info("NRSC5: Library version %s", SAFE_STR(ver_str));

    nrsc5_decoder->audio_out = audio_output_create(app, NRSC5_AUDIO_SAMPLE_RATE, NRSC5_AUDIO_CHANNELS, app->module.source_info.demod_audio_buffer_size);
    if (!nrsc5_decoder->audio_out) return false;
    log_info("NRSC5: Audio device initialized (%d Hz, %d Channels).", NRSC5_AUDIO_SAMPLE_RATE, NRSC5_AUDIO_CHANNELS);

    nrsc5_decoder->pipeline_mode = app->pipeline_mode;
    nrsc5_decoder->input_samplerate = (float)context->config->baseband_sample_rate.rate_hz;

    nrsc5_decoder->stat_counter = 0;
    nrsc5_decoder->accum_mag_sq_sum = 0.0;
    nrsc5_decoder->stat_rate_threshold = (size_t)(nrsc5_decoder->input_samplerate * CONSOLE_UPDATE_INTERVAL_SEC);

    log_debug("NRSC5: Opening pipe...");
    if (nrsc5_open_pipe(&nrsc5_decoder->nrsc5_instance) != 0) {
        log_fatal("NRSC5: Failed to open decoder pipe.");
        return false;
    }

    if (nrsc5_set_mode(nrsc5_decoder->nrsc5_instance, s_nrsc5_config.active_mode) != 0) {
        log_fatal("NRSC5: Failed to set decoder mode.");
        return false;
    }

    log_info("NRSC5: Baseband %.15g Hz | Band: %s", 
             nrsc5_decoder->input_samplerate, 
             (s_nrsc5_config.active_mode == NRSC5_MODE_FM) ? "FM" : "AM");

    nrsc5_decoder->active_program = (unsigned int)s_nrsc5_config.program_id;
    nrsc5_set_callback(nrsc5_decoder->nrsc5_instance, nrsc5_event_callback, nrsc5_decoder);

    log_debug("NRSC5: Starting decoder thread...");
    nrsc5_start(nrsc5_decoder->nrsc5_instance);

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
    nrsc5_context* nrsc5_decoder = (nrsc5_context*)context->app->module.output_private_data;
    if (is_shutdown_requested()) {
        audio_output_clear(nrsc5_decoder->audio_out);
    } else {
        audio_output_drain(nrsc5_decoder->audio_out);
    }
}

static size_t output_nrsc5_write_chunk(ModuleContext* context, const void* buffer, size_t input_bytes) {
    AppContext* app = context->app;
    nrsc5_context* nrsc5_decoder = (nrsc5_context*)app->module.output_private_data;
    if (input_bytes == 0) return 0;

    unsigned int n = input_bytes / app->module.output_bytes_per_iq_sample;
    const float complex* iq = (const float complex*)buffer;

    nrsc5_decoder->accum_mag_sq_sum += calculate_buffer_power(iq, n);
    nrsc5_decoder->stat_counter += n;

    if (nrsc5_decoder->stat_rate_threshold > 0 && nrsc5_decoder->stat_counter >= nrsc5_decoder->stat_rate_threshold) {
        double avg_power = nrsc5_decoder->accum_mag_sq_sum / (double)nrsc5_decoder->stat_counter;
        float dbfs = utility_calculate_dbfs((float)avg_power);

        log_info("NRSC5: dBFS: %.1f", dbfs);

        nrsc5_decoder->stat_counter = 0;
        nrsc5_decoder->accum_mag_sq_sum = 0.0;
    }

    int res_code = nrsc5_pipe_samples_cf32(nrsc5_decoder->nrsc5_instance, (const float*)iq, n * 2);
    if (res_code != 0) {
        log_error("NRSC5: Failed to pipe samples to decoder.");
    }

    return input_bytes;
}

static void output_nrsc5_cleanup(ModuleContext* context) {
    AppContext* app = context->app;
    if (!app->module.output_private_data) return;
    nrsc5_context* nrsc5_decoder = (nrsc5_context*)app->module.output_private_data;

    audio_output_destroy(nrsc5_decoder->audio_out);

    if (nrsc5_decoder->nrsc5_instance) {
        nrsc5_stop(nrsc5_decoder->nrsc5_instance);
        nrsc5_close(nrsc5_decoder->nrsc5_instance);
        nrsc5_decoder->nrsc5_instance = NULL;
    }
}

static void output_nrsc5_get_summary_info(const ModuleContext* context, OutputSummaryInfo* info) {
    (void)context;
    utility_add_summary_item(info, "Output Type", "NRSC5 (HD Radio Player)");
    utility_add_summary_item(info, "Band", "%s", 
                             (s_nrsc5_config.active_mode == NRSC5_MODE_FM) ? "FM" : "AM");
    utility_add_summary_item(info, "Program", "%d (HD%d)", s_nrsc5_config.program_id, s_nrsc5_config.program_id + 1);
}

static const struct argparse_option output_nrsc5_cli_options[] = {
    OPT_GROUP("NRSC5 Output (nrsc5)"),
    OPT_STRING(0, "nrsc5-band", &s_nrsc5_config.band_str, "Set radio band {fm|am}. (Default: auto-detected)", NULL, 0, 0),
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
        nrsc5_context* nrsc5_decoder = (nrsc5_context*)context->app->module.output_private_data;
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
