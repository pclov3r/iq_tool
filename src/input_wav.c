#include "input_wav.h"
#include "constants.h"
#include "log.h"
#include "signal_handler.h"
#include "utils.h"
#include "app_context.h"
#include "platform.h"
#include "sample_convert.h"
#include "input_common.h"
#include "memory_arena.h"
#include "queue.h"
#include "file_write_buffer.h"
#include "argparse.h"
#include "iq_correct.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <sndfile.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>
#include <time.h>
#include <limits.h>
#include <expat.h>
#include <stddef.h>
#include <math.h>

#ifndef _WIN32
#include <strings.h>
#include <unistd.h> // For usleep
#else
#include <windows.h> // For Sleep
#endif

#ifdef _WIN32
#define strcasecmp _stricmp
#endif

#define SDRC_AUXI_CHUNK_ID_STR "auxi"
#define MAX_METADATA_CHUNK_SIZE (1024 * 1024)

#pragma pack(push, 1)
typedef struct {
    uint16_t wYear;
    uint16_t wMonth;
    uint16_t wDayOfWeek;
    uint16_t wDay;
    uint16_t wHour;
    uint16_t wMinute;
    uint16_t wSecond;
    uint16_t wMilliseconds;
} SdrUnoSystemTime;
#pragma pack(pop)

typedef enum {
    SDR_SOFTWARE_UNKNOWN,
    SDR_CONSOLE,
    SDR_SHARP,
    SDR_UNO,
    SDR_CONNECT,
} SdrSoftwareType;

typedef struct SdrMetadata {
    SdrSoftwareType source_software;
    char            software_name[64];
    char            software_version[64];
    char            radio_model[128];
    bool            software_name_present;
    bool            software_version_present;
    bool            radio_model_present;
    double          center_freq_hz;
    bool            center_freq_hz_present;
    time_t          timestamp_unix;
    char            timestamp_str[64];
    bool            timestamp_unix_present;
    bool            timestamp_str_present;
} SdrMetadata;

typedef enum {
    ATTR_TYPE_STRING,
    ATTR_TYPE_DOUBLE,
    ATTR_TYPE_TIME_T_SECONDS,
    ATTR_TYPE_TIME_T_STRING
} AttrType;

typedef struct {
    const char* name;
    AttrType type;
    size_t offset;
    size_t present_flag_offset;
    size_t buffer_size;
} AttributeParser;

// This is the private data structure for the WAV input module.
typedef struct {
    SNDFILE *infile;
    SdrMetadata sdr_info;
    bool sdr_info_present;
} WavPrivateData;

static void XMLCALL expat_start_element_handler(void *userData, const XML_Char *name, const XML_Char **atts);
static bool _parse_auxi_xml_expat(const unsigned char *chunk_data, sf_count_t chunk_size, SdrMetadata *metadata);
static bool _parse_binary_auxi_data(const unsigned char *chunk_data, sf_count_t chunk_size, SdrMetadata *metadata);
static time_t timegm_portable(struct tm *tm);
static void init_sdr_metadata(SdrMetadata *metadata);
static bool parse_sdr_metadata_chunks(SNDFILE *infile, const SF_INFO *sfinfo, SdrMetadata *metadata, MemoryArena* arena);
static bool parse_sdr_metadata_from_filename(const char* base_filename, SdrMetadata *metadata);

static const char* sdr_software_type_to_string(SdrSoftwareType type) {
    switch (type) {
        case SDR_SOFTWARE_UNKNOWN: return "Unknown";
        case SDR_CONSOLE:          return "SDR Console";
        case SDR_SHARP:            return "SDR#";
        case SDR_UNO:              return "SDRuno";
        case SDR_CONNECT:          return "SDRconnect";
        default:                   return "Invalid Type";
    }
}

#ifndef HAVE_STRCASESTR
static char *strcasestr(const char *haystack, const char *needle) {
    if (!needle || !*needle) return (char *)haystack;
    while (*haystack) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && (tolower((unsigned char)*h) == tolower((unsigned char)*n))) {
            h++;
            n++;
        }
        if (!*n) return (char *)haystack;
        haystack++;
    }
    return NULL;
}
#endif

static void init_sdr_metadata(SdrMetadata *metadata) {
    if (!metadata) return;
    memset(metadata, 0, sizeof(SdrMetadata));
    metadata->source_software = SDR_SOFTWARE_UNKNOWN;
}

static bool process_specific_chunk(SNDFILE *infile, SdrMetadata *metadata, const char* chunk_id_str, MemoryArena* arena) {
    SF_CHUNK_ITERATOR *iterator = NULL;
    SF_CHUNK_INFO chunk_info_filter, chunk_info_query;
    unsigned char* chunk_data_buffer = NULL;
    bool parsed_successfully = false;

    memset(&chunk_info_filter, 0, sizeof(SF_CHUNK_INFO));
    strncpy(chunk_info_filter.id, chunk_id_str, sizeof(chunk_info_filter.id) - 1);
    chunk_info_filter.id[sizeof(chunk_info_filter.id) - 1] = '\0';

    iterator = sf_get_chunk_iterator(infile, &chunk_info_filter);
    if (!iterator) return false;

    memset(&chunk_info_query, 0, sizeof(SF_CHUNK_INFO));
    if (sf_get_chunk_size(iterator, &chunk_info_query) != SF_ERR_NO_ERROR) return false;
    if (chunk_info_query.datalen == 0 || chunk_info_query.datalen > MAX_METADATA_CHUNK_SIZE) return false;

    chunk_data_buffer = (unsigned char*)mem_arena_alloc(arena, chunk_info_query.datalen, false);
    if (!chunk_data_buffer) {
        return false;
    }

    chunk_info_query.data = chunk_data_buffer;
    if (sf_get_chunk_data(iterator, &chunk_info_query) != SF_ERR_NO_ERROR) {
        return false;
    }

    if (strcmp(chunk_id_str, SDRC_AUXI_CHUNK_ID_STR) == 0) {
        if (!_parse_auxi_xml_expat(chunk_data_buffer, chunk_info_query.datalen, metadata)) {
            parsed_successfully = _parse_binary_auxi_data(chunk_data_buffer, chunk_info_query.datalen, metadata);
        } else {
            parsed_successfully = true;
        }
    }

    return parsed_successfully;
}

static bool parse_sdr_metadata_chunks(SNDFILE *infile, const SF_INFO *sfinfo, SdrMetadata *metadata, MemoryArena* arena) {
    if (!infile || !sfinfo || !metadata) return false;
    (void)sfinfo;
    return process_specific_chunk(infile, metadata, SDRC_AUXI_CHUNK_ID_STR, arena);
}

static bool parse_sdr_metadata_from_filename(const char* base_filename, SdrMetadata *metadata) {
    if (!base_filename || !metadata) return false;
    bool parsed_something_new = false;
    bool inferred_sdrsharp = false;

    if (!metadata->center_freq_hz_present) {
        const char *hz_ptr = strcasestr(base_filename, "Hz");
        if (hz_ptr) {
            const char *start_ptr = base_filename;
            const char *underscore_ptr = NULL;
            const char *temp_ptr = start_ptr;
            while ((temp_ptr = strchr(temp_ptr, '_')) != NULL && temp_ptr < hz_ptr) {
                underscore_ptr = temp_ptr;
                temp_ptr++;
            }
            if (underscore_ptr && underscore_ptr + 1 < hz_ptr) {
                char freq_str[32];
                size_t num_len = hz_ptr - (underscore_ptr + 1);
                if (num_len < sizeof(freq_str) && num_len > 0) {
                    strncpy(freq_str, underscore_ptr + 1, num_len);
                    freq_str[num_len] = '\0';
                    char *endptr;
                    double freq_hz = strtod(freq_str, &endptr);
                    if (*endptr == '\0' && isfinite(freq_hz) && freq_hz > 0) {
                        metadata->center_freq_hz = freq_hz;
                        metadata->center_freq_hz_present = true;
                        parsed_something_new = true;
                        inferred_sdrsharp = true;
                    }
                }
            }
        }
    }

    if (!metadata->timestamp_unix_present) {
        const char *match_start = strstr(base_filename, "_");
        while (match_start) {
            int year, month, day, hour, min, sec;
            if (strlen(match_start) >= 17 && match_start[9] == '_' && match_start[16] == 'Z' &&
                sscanf(match_start, "_%4d%2d%2d_%2d%2d%2dZ", &year, &month, &day, &hour, &min, &sec) == 6) {
                struct tm t = {0};
                t.tm_year = year - 1900;
                t.tm_mon = month - 1;
                t.tm_mday = day;
                t.tm_hour = hour;
                t.tm_min = min;
                t.tm_sec = sec;
                time_t timestamp = timegm_portable(&t);
                if (timestamp != (time_t)-1) {
                    metadata->timestamp_unix = timestamp;
                    metadata->timestamp_unix_present = true;
                    if (!metadata->timestamp_str_present) {
                        snprintf(metadata->timestamp_str, sizeof(metadata->timestamp_str),
                                 "%04d-%02d-%02d %02d:%02d:%02d UTC", year, month, day, hour, min, sec);
                        metadata->timestamp_str_present = true;
                    }
                    parsed_something_new = true;
                    inferred_sdrsharp = true;
                    break;
                }
            }
            match_start = strstr(match_start + 1, "_");
        }
    }

    if (metadata->source_software == SDR_SOFTWARE_UNKNOWN) {
        if (inferred_sdrsharp) {
            metadata->source_software = SDR_SHARP;
        } else if (strncmp(base_filename, "SDRuno_", 7) == 0) {
            metadata->source_software = SDR_UNO;
        } else if (strncmp(base_filename, "SDRconnect_", 11) == 0) {
            metadata->source_software = SDR_CONNECT;
        }
        if (metadata->source_software != SDR_SOFTWARE_UNKNOWN && !metadata->software_name_present) {
            snprintf(metadata->software_name, sizeof(metadata->software_name), "%s", sdr_software_type_to_string(metadata->source_software));
            metadata->software_name_present = true;
            parsed_something_new = true;
        }
    }

    return parsed_something_new;
}

static time_t timegm_portable(struct tm *tm) {
    if (!tm) return -1;
    tm->tm_isdst = 0;
#ifdef _WIN32
    return _mkgmtime(tm);
#else
    time_t result;
    char *tz_orig = getenv("TZ");
    setenv("TZ", "", 1);
    tzset();
    result = mktime(tm);
    if (tz_orig) {
        setenv("TZ", tz_orig, 1);
    } else {
        unsetenv("TZ");
    }
    tzset();
    return result;
#endif
}

static bool _parse_binary_auxi_data(const unsigned char *chunk_data, sf_count_t chunk_size, SdrMetadata *metadata) {
    const size_t min_req_size = sizeof(SdrUnoSystemTime) + 16 + 4;
    if (!chunk_data || !metadata || chunk_size < (sf_count_t)min_req_size) {
        return false;
    }
    bool time_parsed = false;
    bool freq_parsed = false;

    SdrUnoSystemTime st;
    memcpy(&st, chunk_data, sizeof(SdrUnoSystemTime));
    struct tm t = {0};
    t.tm_year = st.wYear - 1900;
    t.tm_mon = st.wMonth - 1;
    t.tm_mday = st.wDay;
    t.tm_hour = st.wHour;
    t.tm_min = st.wMinute;
    t.tm_sec = st.wSecond;
    time_t timestamp = timegm_portable(&t);
    if (timestamp != (time_t)-1 && !metadata->timestamp_unix_present) {
        metadata->timestamp_unix = timestamp;
        metadata->timestamp_unix_present = true;
        time_parsed = true;
        if (!metadata->timestamp_str_present) {
            snprintf(metadata->timestamp_str, sizeof(metadata->timestamp_str),
                     "%04u-%02u-%02u %02u:%02u:%02u UTC",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            metadata->timestamp_str_present = true;
        }
    }

    uint32_t freq_hz_int;
    memcpy(&freq_hz_int, chunk_data + 32, sizeof(uint32_t));
    if (freq_hz_int > 0 && !metadata->center_freq_hz_present) {
        metadata->center_freq_hz = (double)freq_hz_int;
        metadata->center_freq_hz_present = true;
        freq_parsed = true;
    }

    return time_parsed || freq_parsed;
}

static const AttributeParser attribute_parsers[] = {
    {"SoftwareName",    ATTR_TYPE_STRING,        offsetof(SdrMetadata, software_name),    offsetof(SdrMetadata, software_name_present),    sizeof(((SdrMetadata*)0)->software_name)},
    {"SoftwareVersion", ATTR_TYPE_STRING,        offsetof(SdrMetadata, software_version), offsetof(SdrMetadata, software_version_present), sizeof(((SdrMetadata*)0)->software_version)},
    {"RadioModel",      ATTR_TYPE_STRING,        offsetof(SdrMetadata, radio_model),      offsetof(SdrMetadata, radio_model_present),      sizeof(((SdrMetadata*)0)->radio_model)},
    {"RadioCenterFreq", ATTR_TYPE_DOUBLE,        offsetof(SdrMetadata, center_freq_hz),   offsetof(SdrMetadata, center_freq_hz_present),   0},
    {"UTCSeconds",      ATTR_TYPE_TIME_T_SECONDS,offsetof(SdrMetadata, timestamp_unix),   offsetof(SdrMetadata, timestamp_unix_present),   0},
    {"CurrentTimeUTC",  ATTR_TYPE_TIME_T_STRING, offsetof(SdrMetadata, timestamp_str),    offsetof(SdrMetadata, timestamp_str_present),    sizeof(((SdrMetadata*)0)->timestamp_str)}
};
static const size_t num_attribute_parsers = sizeof(attribute_parsers) / sizeof(attribute_parsers[0]);

static void XMLCALL expat_start_element_handler(void *userData, const XML_Char *name, const XML_Char **atts) {
    SdrMetadata *metadata = (SdrMetadata *)userData;
    if (strcmp(name, "Definition") != 0) return;

    for (int i = 0; atts[i] != NULL; i += 2) {
        const char *attr_name = atts[i];
        const char *attr_value = atts[i+1];
        for (size_t j = 0; j < num_attribute_parsers; ++j) {
            if (strcmp(attr_name, attribute_parsers[j].name) == 0) {
                const AttributeParser *parser = &attribute_parsers[j];
                char *data_ptr = (char*)metadata + parser->offset;
                bool *present_flag_ptr = (bool*)((char*)metadata + parser->present_flag_offset);
                errno = 0;
                switch (parser->type) {
                    case ATTR_TYPE_STRING:
                        if (strlen(attr_value) >= parser->buffer_size) {
                            log_warn("Metadata contains overly long string for attribute '%s'. Truncating.", attr_name);
                        }
                        snprintf(data_ptr, parser->buffer_size, "%s", attr_value);
                        *present_flag_ptr = true;
                        break;
                    case ATTR_TYPE_DOUBLE: {
                        char *endptr;
                        double d = strtod(attr_value, &endptr);
                        if (errno == 0 && *endptr == '\0' && isfinite(d)) {
                            *(double*)data_ptr = d;
                            *present_flag_ptr = true;
                        }
                    } break;
                    case ATTR_TYPE_TIME_T_SECONDS:
                        if (!metadata->timestamp_unix_present) {
                            char *endptr;
                            long long ts_ll = strtoll(attr_value, &endptr, 10);
                            if (errno == 0 && *endptr == '\0') {
                                *(time_t*)data_ptr = (time_t)ts_ll;
                                *present_flag_ptr = true;
                            }
                        }
                        break;
                    case ATTR_TYPE_TIME_T_STRING:
                        snprintf(data_ptr, parser->buffer_size, "%s", attr_value);
                        *present_flag_ptr = true;
                        struct tm t = {0};
                        int year, month, day, hour, min, sec;
                        if (sscanf(attr_value, "%d-%d-%d %d:%d:%d", &day, &month, &year, &hour, &min, &sec) == 6) {
                            t.tm_year = year - 1900;
                            t.tm_mon = month - 1;
                            t.tm_mday = day;
                            t.tm_hour = hour;
                            t.tm_min = min;
                            t.tm_sec = sec;
                            time_t timestamp = timegm_portable(&t);
                            if (timestamp != (time_t)-1) {
                                metadata->timestamp_unix = timestamp;
                                metadata->timestamp_unix_present = true;
                            }
                        }
                        break;
                }
                break;
            }
        }
    }
}

static bool _parse_auxi_xml_expat(const unsigned char *chunk_data, sf_count_t chunk_size, SdrMetadata *metadata) {
    if (!chunk_data || chunk_size <= 0 || !metadata) return false;
    XML_Parser parser = XML_ParserCreate(NULL);
    if (!parser) return false;

    XML_SetUserData(parser, metadata);
    XML_SetElementHandler(parser, expat_start_element_handler, NULL);
    XML_Parse(parser, (const char*)chunk_data, chunk_size, 1);

    bool any_data_parsed = metadata->software_name_present ||
                           metadata->radio_model_present ||
                           metadata->center_freq_hz_present ||
                           metadata->timestamp_unix_present;

    XML_ParserFree(parser);

    if (any_data_parsed && metadata->software_name_present) {
        if (strstr(metadata->software_name, "SDR Console") != NULL) {
            metadata->source_software = SDR_CONSOLE;
        }
    }
    return any_data_parsed;
}

static struct {
    float center_target_hz_arg;
} s_wav_config;

extern AppConfig g_config;

static const struct argparse_option wav_cli_options[] = {
    OPT_GROUP("WAV Input Specific Options"),
    OPT_FLOAT(0, "wav-center-target-freq", &s_wav_config.center_target_hz_arg, "Shift signal to a new target center frequency (e.g., 97.3e6)", NULL, 0, 0),
};

const struct argparse_option* wav_get_cli_options(int* count) {
    *count = sizeof(wav_cli_options) / sizeof(wav_cli_options[0]);
    return wav_cli_options;
}

static bool wav_initialize(InputSourceContext* ctx);
static void* wav_start_stream(InputSourceContext* ctx);
static void wav_stop_stream(InputSourceContext* ctx);
static void wav_cleanup(InputSourceContext* ctx);
static void wav_get_summary_info(const InputSourceContext* ctx, InputSummaryInfo* info);
static bool wav_pre_stream_iq_correction(InputSourceContext* ctx);

static InputSourceOps wav_ops = {
    .initialize = wav_initialize,
    .start_stream = wav_start_stream,
    .stop_stream = wav_stop_stream,
    .cleanup = wav_cleanup,
    .get_summary_info = wav_get_summary_info,
    .has_known_length = _input_source_has_known_length_true,
    .validate_options = NULL,
    .validate_generic_options = NULL,
    .pre_stream_iq_correction = wav_pre_stream_iq_correction,
};

InputSourceOps* get_wav_input_ops(void) {
    return &wav_ops;
}

static void wav_get_summary_info(const InputSourceContext* ctx, InputSummaryInfo* info) {
    const AppConfig *config = ctx->config;
    const AppResources *resources = ctx->resources;
    WavPrivateData* private_data = (WavPrivateData*)resources->input_module_private_data;

    const char* display_path = config->input_filename_arg;
#ifdef _WIN32
    if (config->effective_input_filename_utf8[0] != '\0') {
        display_path = config->effective_input_filename_utf8;
    }
#endif

    add_summary_item(info, "Input File", "%s", display_path);

    const char *format_str;
    switch (resources->input_format) {
        case CS16: format_str = "16-bit Signed Complex PCM (cs16)"; break;
        case CU8:  format_str = "8-bit Unsigned Complex PCM (cu8)"; break;
        default:   format_str = "Unknown PCM"; break;
    }
    add_summary_item(info, "Input Format", "%s", format_str);
    add_summary_item(info, "Input Rate", "%.0f Hz", (double)resources->source_info.samplerate);

    long long input_file_size = -1LL;
#ifdef _WIN32
    struct __stat64 stat_buf64;
    if (_wstat64(config->effective_input_filename_w, &stat_buf64) == 0)
        input_file_size = stat_buf64.st_size;
#else
    struct stat stat_buf;
    if (stat(display_path, &stat_buf) == 0)
        input_file_size = stat_buf.st_size;
#endif
    char size_buf[40];
    add_summary_item(info, "Input File Size", "%s", format_file_size(input_file_size, size_buf, sizeof(size_buf)));

    if (private_data->sdr_info_present) {
        if (private_data->sdr_info.timestamp_unix_present) {
            char time_buf[64];
            struct tm time_info;
#ifdef _WIN32
            if (gmtime_s(&time_info, &private_data->sdr_info.timestamp_unix) == 0) {
                strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S UTC", &time_info);
                add_summary_item(info, "Timestamp", "%s", time_buf);
            }
#else
            if (gmtime_r(&private_data->sdr_info.timestamp_unix, &time_info)) {
                strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S UTC", &time_info);
                add_summary_item(info, "Timestamp", "%s", time_buf);
            }
#endif
        } else if (private_data->sdr_info.timestamp_str_present) {
            add_summary_item(info, "Timestamp", "%s", private_data->sdr_info.timestamp_str);
        }
        if (private_data->sdr_info.center_freq_hz_present) {
            add_summary_item(info, "Center Frequency", "%.0f Hz", private_data->sdr_info.center_freq_hz);
        }
        if (private_data->sdr_info.software_name_present) {
            char sw_buf[128];
            snprintf(sw_buf, sizeof(sw_buf), "%s %s",
                     private_data->sdr_info.software_name,
                     private_data->sdr_info.software_version_present ? private_data->sdr_info.software_version : "");
            add_summary_item(info, "SDR Software", "%s", sw_buf);
        }
        if (private_data->sdr_info.radio_model_present) {
            add_summary_item(info, "Radio Model", "%s", private_data->sdr_info.radio_model);
        }
    }
}

static bool wav_initialize(InputSourceContext* ctx) {
    const AppConfig *config = ctx->config;
    AppResources *resources = ctx->resources;

    WavPrivateData* private_data = (WavPrivateData*)mem_arena_alloc(&resources->setup_arena, sizeof(WavPrivateData), true);
    if (!private_data) {
        return false;
    }
    resources->input_module_private_data = private_data;

#ifdef _WIN32
    log_info("Opening WAV input file: %s", config->effective_input_filename_utf8);
    SF_INFO sfinfo;
    memset(&sfinfo, 0, sizeof(SF_INFO));
    private_data->infile = sf_wchar_open(config->effective_input_filename_w, SFM_READ, &sfinfo);
#else
    log_info("Opening WAV input file: %s", config->effective_input_filename);
    SF_INFO sfinfo;
    memset(&sfinfo, 0, sizeof(SF_INFO));
    private_data->infile = sf_open(config->effective_input_filename, SFM_READ, &sfinfo);
#endif

    if (!private_data->infile) {
        log_fatal("Error opening input file: %s", sf_strerror(private_data->infile));
        return false;
    }

    if (sfinfo.channels != 2) {
        log_fatal("Error: Input file must have 2 channels (I/Q), but found %d.", sfinfo.channels);
        sf_close(private_data->infile);
        private_data->infile = NULL;
        return false;
    }

    int sf_subtype = (sfinfo.format & SF_FORMAT_SUBMASK);
    switch (sf_subtype) {
        case SF_FORMAT_PCM_16: resources->input_format = CS16; break;
        case SF_FORMAT_PCM_U8: resources->input_format = CU8; break;
        default:
            log_fatal("Error: Input WAV file uses an unsupported PCM subtype (0x%04X). "
                      "Supported WAV PCM subtypes are 16-bit Signed (cs16) and 8-bit Unsigned (cu8).", sf_subtype);
            sf_close(private_data->infile);
            private_data->infile = NULL;
            return false;
    }

    resources->input_bytes_per_sample_pair = get_bytes_per_sample(resources->input_format);

    if (sfinfo.samplerate <= 0) {
        log_fatal("Error: Invalid input sample rate (%d Hz).", sfinfo.samplerate);
        sf_close(private_data->infile);
        private_data->infile = NULL;
        return false;
    }

    if (sfinfo.frames == 0) {
        log_warn("Warning: Input file appears to be empty (0 frames).");
    }

    resources->source_info.samplerate = sfinfo.samplerate;
    resources->source_info.frames = sfinfo.frames;

    init_sdr_metadata(&private_data->sdr_info);
    private_data->sdr_info_present = parse_sdr_metadata_chunks(private_data->infile, &sfinfo, &private_data->sdr_info, &resources->setup_arena);

    char basename_buffer[MAX_PATH_BUFFER];
    const char* base_filename = get_basename_for_parsing(config, basename_buffer, sizeof(basename_buffer), &resources->setup_arena);
    if (base_filename) {
        bool filename_parsed = parse_sdr_metadata_from_filename(base_filename, &private_data->sdr_info);
        private_data->sdr_info_present = private_data->sdr_info_present || filename_parsed;
    }

    if (s_wav_config.center_target_hz_arg != 0.0f) {
        if (config->freq_shift_hz_arg != 0.0f) {
            log_fatal("Conflicting frequency shift options provided. Cannot use --freq-shift and --wav-center-target-freq at the same time.");
            sf_close(private_data->infile);
            return false;
        }

        if (!private_data->sdr_info.center_freq_hz_present) {
            log_fatal("Option --wav-center-target-freq was used, but the input WAV file does not contain the required center frequency metadata.");
            sf_close(private_data->infile);
            return false;
        }

        double target_freq = (double)s_wav_config.center_target_hz_arg;
        resources->nco_shift_hz = private_data->sdr_info.center_freq_hz - target_freq;
    }

    return true;
}

static void* wav_start_stream(InputSourceContext* ctx) {
    AppResources *resources = ctx->resources;
    WavPrivateData* private_data = (WavPrivateData*)resources->input_module_private_data;

    // Pre-calculate the back-pressure threshold in bytes for efficiency.
    // This check is only relevant for file-to-file operations.
    const size_t writer_buffer_capacity = resources->config->output_to_stdout ? 0 : file_write_buffer_get_capacity(resources->file_write_buffer);
    const size_t writer_buffer_threshold = (size_t)(writer_buffer_capacity * IO_WRITER_BUFFER_HIGH_WATER_MARK);

    while (!is_shutdown_requested() && !resources->error_occurred) {
        // --- START: Back-pressure Pacing Logic ---
        if (!resources->config->output_to_stdout && (file_write_buffer_get_size(resources->file_write_buffer) > writer_buffer_threshold)) {
            // The writer is falling behind. Pause briefly to let it catch up.
            #ifdef _WIN32
                Sleep(10); // 10 ms
            #else
                usleep(10000); // 10 ms
            #endif
            continue; // Re-evaluate the buffer state in the next loop iteration.
        }
        // --- END: Back-pressure Pacing Logic ---

        SampleChunk *current_item = (SampleChunk*)queue_dequeue(resources->free_sample_chunk_queue);
        if (!current_item) {
            break; // Shutdown or error signaled
        }

        current_item->stream_discontinuity_event = false;

        int64_t bytes_read = sf_read_raw(private_data->infile, current_item->raw_input_data, current_item->raw_input_capacity_bytes);

        if (bytes_read < 0) {
            log_fatal("libsndfile read error: %s", sf_strerror(private_data->infile));
            pthread_mutex_lock(&resources->progress_mutex);
            resources->error_occurred = true;
            pthread_mutex_unlock(&resources->progress_mutex);
            request_shutdown();
            queue_enqueue(resources->free_sample_chunk_queue, current_item);
            break;
        }

        current_item->frames_read = bytes_read / resources->input_bytes_per_sample_pair;
        current_item->packet_sample_format = resources->input_format;
        
        current_item->is_last_chunk = (current_item->frames_read == 0);

        if (!current_item->is_last_chunk) {
            pthread_mutex_lock(&resources->progress_mutex);
            resources->total_frames_read += current_item->frames_read;
            pthread_mutex_unlock(&resources->progress_mutex);
        }

        if (!queue_enqueue(resources->raw_to_pre_process_queue, current_item)) {
            queue_enqueue(resources->free_sample_chunk_queue, current_item);
            break;
        }

        if (current_item->is_last_chunk) {
            break;
        }
    }
    return NULL;
}

static void wav_stop_stream(InputSourceContext* ctx) {
    (void)ctx;
}

static void wav_cleanup(InputSourceContext* ctx) {
    AppResources *resources = ctx->resources;
    if (resources->input_module_private_data) {
        WavPrivateData* private_data = (WavPrivateData*)resources->input_module_private_data;
        if (private_data->infile) {
            log_info("Closing WAV input file.");
            sf_close(private_data->infile);
            private_data->infile = NULL;
        }
        resources->input_module_private_data = NULL;
    }
}

static bool wav_pre_stream_iq_correction(InputSourceContext* ctx) {
    AppConfig* config = (AppConfig*)ctx->config;
    WavPrivateData* private_data = (WavPrivateData*)ctx->resources->input_module_private_data;

    // This routine is only necessary if I/Q correction is enabled.
    if (!config->iq_correction.enable) {
        return true;
    }
    
    // The module's only job is to call the calibration service with its private file handle.
    return iq_correct_run_initial_calibration(ctx, private_data->infile);
}
