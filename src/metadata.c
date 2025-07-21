#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700 // For timegm if available and needed

#include "metadata.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <limits.h>
#include <sndfile.h>
#include <expat.h>
#include <stddef.h> // Required for offsetof macro

// --- Constants ---
#define SDRC_AUXI_CHUNK_ID_STR "auxi"
#define MAX_METADATA_CHUNK_SIZE (1024 * 1024)

// Structure mimicking Windows SYSTEMTIME for SDRuno/SDRconnect auxi chunk
#pragma pack(push, 1)
typedef struct {
    uint16_t wYear;
    uint16_t wMonth;
    uint16_t wDayOfWeek; // Ignored
    uint16_t wDay;
    uint16_t wHour;
    uint16_t wMinute;
    uint16_t wSecond;
    uint16_t wMilliseconds; // Ignored
} SdrUnoSystemTime;
#pragma pack(pop)


// --- Data-Driven XML Parsing ---

// Enum to identify the data type of an XML attribute
typedef enum {
    ATTR_TYPE_STRING,
    ATTR_TYPE_DOUBLE,
    ATTR_TYPE_TIME_T_SECONDS,
    ATTR_TYPE_TIME_T_STRING
} AttrType;

// Struct to map an XML attribute name to its parser and location in SdrMetadata
typedef struct {
    const char* name;
    AttrType type;
    size_t offset; // Offset of the data field in SdrMetadata
    size_t present_flag_offset; // Offset of the boolean 'present' flag
    size_t buffer_size; // For string types
} AttributeParser;

// --- Forward Declarations for Static Functions ---
static void XMLCALL expat_start_element_handler(void *userData, const XML_Char *name, const XML_Char **atts);
static bool _parse_auxi_xml_expat(const unsigned char *chunk_data, sf_count_t chunk_size, SdrMetadata *metadata);
static bool _parse_binary_auxi_data(const unsigned char *chunk_data, sf_count_t chunk_size, SdrMetadata *metadata);
static time_t timegm_portable(struct tm *tm);

// --- Polyfill for strcasestr (if not available) ---
#ifndef HAVE_STRCASESTR
static char *strcasestr(const char *haystack, const char *needle) {
    if (!needle || !*needle) return (char *)haystack;
    while (*haystack) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && (tolower((unsigned char)*h) == tolower((unsigned char)*n))) { h++; n++; }
        if (!*n) return (char *)haystack;
        haystack++;
    }
    return NULL;
}
#endif

// --- Public Functions ---

/**
 * @brief Initializes the SdrMetadata struct to default/unknown values.
 */
void init_sdr_metadata(SdrMetadata *metadata) {
    if (!metadata) return;
    memset(metadata, 0, sizeof(SdrMetadata));
    metadata->source_software = SDR_SOFTWARE_UNKNOWN;
}

/**
 * @brief Processes a specific chunk type identified by its ID string.
 */
static bool process_specific_chunk(SNDFILE *infile, SdrMetadata *metadata, const char* chunk_id_str) {
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

    chunk_data_buffer = (unsigned char*)malloc(chunk_info_query.datalen);
    if (!chunk_data_buffer) return false;

    chunk_info_query.data = chunk_data_buffer;
    if (sf_get_chunk_data(iterator, &chunk_info_query) != SF_ERR_NO_ERROR) {
        free(chunk_data_buffer);
        return false;
    }

    // --- Attempt Parsing based on Chunk ID ---
    if (strcmp(chunk_id_str, SDRC_AUXI_CHUNK_ID_STR) == 0) {
        // Try parsing as XML first; if that fails, try as binary (for older formats)
        if (!_parse_auxi_xml_expat(chunk_data_buffer, chunk_info_query.datalen, metadata)) {
            parsed_successfully = _parse_binary_auxi_data(chunk_data_buffer, chunk_info_query.datalen, metadata);
        } else {
            parsed_successfully = true;
        }
    }

    free(chunk_data_buffer);
    return parsed_successfully;
}

/**
 * @brief Attempts to parse SDR-specific metadata chunks from a WAV file.
 */
bool parse_sdr_metadata_chunks(SNDFILE *infile, const SF_INFO *sfinfo, SdrMetadata *metadata) {
    if (!infile || !sfinfo || !metadata) return false;
    (void)sfinfo; // sfinfo is unused for now but kept for API consistency
    return process_specific_chunk(infile, metadata, SDRC_AUXI_CHUNK_ID_STR);
}

/**
 * @brief Attempts to parse SDR metadata (frequency, timestamp) from a filename.
 */
bool parse_sdr_metadata_from_filename(const char* base_filename, SdrMetadata *metadata) {
    if (!base_filename || !metadata) return false;

    bool parsed_something_new = false;
    bool inferred_sdrsharp = false;

    // Parse Frequency (_(\d+)Hz) - Only if not already present
    if (!metadata->center_freq_hz_present) {
        const char *hz_ptr = strcasestr(base_filename, "Hz");
        if (hz_ptr) {
            const char *start_ptr = base_filename;
            const char *underscore_ptr = NULL;
            const char *temp_ptr = start_ptr;
            while ((temp_ptr = strchr(temp_ptr, '_')) != NULL && temp_ptr < hz_ptr) {
                underscore_ptr = temp_ptr; temp_ptr++;
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

    // Parse Full Timestamp (_YYYYMMDD_HHMMSSZ_) - Only if not already present
    if (!metadata->timestamp_unix_present) {
        const char *match_start = strstr(base_filename, "_");
        while (match_start) {
            int year, month, day, hour, min, sec;
            if (strlen(match_start) >= 17 && match_start[9] == '_' && match_start[16] == 'Z' &&
                sscanf(match_start, "_%4d%2d%2d_%2d%2d%2dZ", &year, &month, &day, &hour, &min, &sec) == 6) {
                struct tm t = {0};
                t.tm_year = year - 1900; t.tm_mon = month - 1; t.tm_mday = day;
                t.tm_hour = hour; t.tm_min = min; t.tm_sec = sec;
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

    // Update Software Type if Inferred and not already set by auxi chunk
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

/**
 * @brief Converts an SdrSoftwareType enum value to a human-readable string.
 */
const char* sdr_software_type_to_string(SdrSoftwareType type) {
    switch (type) {
        case SDR_SOFTWARE_UNKNOWN: return "Unknown";
        case SDR_CONSOLE:          return "SDR Console";
        case SDR_SHARP:            return "SDR#";
        case SDR_UNO:              return "SDRuno";
        case SDR_CONNECT:          return "SDRconnect";
        default:                   return "Invalid Type";
    }
}

// --- Private Helper Functions ---

/**
 * @brief Portable implementation of timegm (UTC mktime).
 * @note The POSIX implementation using setenv is not thread-safe. This is acceptable
 *       here as metadata parsing happens in a single thread at startup, but this
 *       function should not be used concurrently from multiple threads without locking.
 */
static time_t timegm_portable(struct tm *tm) {
    if (!tm) return -1;
    tm->tm_isdst = 0;
#ifdef _WIN32
    return _mkgmtime(tm);
#else
    time_t result;
    char *tz_orig = getenv("TZ");
    setenv("TZ", "", 1); // Set timezone to UTC
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

/**
 * @brief Parses SDRuno/SDRconnect binary auxi chunk data.
 */
static bool _parse_binary_auxi_data(const unsigned char *chunk_data, sf_count_t chunk_size, SdrMetadata *metadata) {
    const size_t min_req_size = sizeof(SdrUnoSystemTime) + 16 + 4;
    if (!chunk_data || !metadata || chunk_size < (sf_count_t)min_req_size) {
        return false;
    }

    bool time_parsed = false;
    bool freq_parsed = false;

    // Parse Start Time
    SdrUnoSystemTime st;
    memcpy(&st, chunk_data, sizeof(SdrUnoSystemTime));
    struct tm t = {0};
    t.tm_year = st.wYear - 1900; t.tm_mon = st.wMonth - 1; t.tm_mday = st.wDay;
    t.tm_hour = st.wHour; t.tm_min = st.wMinute; t.tm_sec = st.wSecond;
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

    // Parse Center Frequency
    uint32_t freq_hz_int;
    memcpy(&freq_hz_int, chunk_data + 32, sizeof(uint32_t)); // Offset 32
    if (freq_hz_int > 0 && !metadata->center_freq_hz_present) {
         metadata->center_freq_hz = (double)freq_hz_int;
         metadata->center_freq_hz_present = true;
         freq_parsed = true;
    }

    return time_parsed || freq_parsed;
}

// --- Data-Driven XML Parsing Implementation ---

/**
 * @brief This table drives the XML parsing. It maps attribute names to their type,
 *        and their location within the SdrMetadata struct. This makes adding new
 *        metadata fields a simple one-line change.
 */
static const AttributeParser attribute_parsers[] = {
    {"SoftwareName",    ATTR_TYPE_STRING,         offsetof(SdrMetadata, software_name),    offsetof(SdrMetadata, software_name_present),    sizeof(((SdrMetadata*)0)->software_name)},
    {"SoftwareVersion", ATTR_TYPE_STRING,         offsetof(SdrMetadata, software_version), offsetof(SdrMetadata, software_version_present), sizeof(((SdrMetadata*)0)->software_version)},
    {"RadioModel",      ATTR_TYPE_STRING,         offsetof(SdrMetadata, radio_model),      offsetof(SdrMetadata, radio_model_present),      sizeof(((SdrMetadata*)0)->radio_model)},
    {"RadioCenterFreq", ATTR_TYPE_DOUBLE,         offsetof(SdrMetadata, center_freq_hz),   offsetof(SdrMetadata, center_freq_hz_present),   0},
    {"UTCSeconds",      ATTR_TYPE_TIME_T_SECONDS, offsetof(SdrMetadata, timestamp_unix),   offsetof(SdrMetadata, timestamp_unix_present),   0},
    {"CurrentTimeUTC",  ATTR_TYPE_TIME_T_STRING,  offsetof(SdrMetadata, timestamp_str),    offsetof(SdrMetadata, timestamp_str_present),    sizeof(((SdrMetadata*)0)->timestamp_str)}
};
static const size_t num_attribute_parsers = sizeof(attribute_parsers) / sizeof(attribute_parsers[0]);

/**
 * @brief Expat callback triggered for each XML start element.
 *        It iterates through the element's attributes, looks them up in the
 *        `attribute_parsers` table, and populates the SdrMetadata struct.
 */
static void XMLCALL expat_start_element_handler(void *userData, const XML_Char *name, const XML_Char **atts) {
    SdrMetadata *metadata = (SdrMetadata *)userData;
    if (strcmp(name, "Definition") != 0) return;

    // Iterate through all attributes of the <Definition> tag
    for (int i = 0; atts[i] != NULL; i += 2) {
        const char *attr_name = atts[i];
        const char *attr_value = atts[i+1];

        // Find the corresponding parser in our table
        for (size_t j = 0; j < num_attribute_parsers; ++j) {
            if (strcmp(attr_name, attribute_parsers[j].name) == 0) {
                const AttributeParser *parser = &attribute_parsers[j];
                char *data_ptr = (char*)metadata + parser->offset;
                bool *present_flag_ptr = (bool*)((char*)metadata + parser->present_flag_offset);
                errno = 0;

                switch (parser->type) {
                    case ATTR_TYPE_STRING:
                        snprintf(data_ptr, parser->buffer_size, "%s", attr_value);
                        *present_flag_ptr = true;
                        break;
                    case ATTR_TYPE_DOUBLE:
                        {
                            char *endptr;
                            double d = strtod(attr_value, &endptr);
                            if (errno == 0 && *endptr == '\0' && isfinite(d)) {
                                *(double*)data_ptr = d;
                                *present_flag_ptr = true;
                            }
                        }
                        break;
                    case ATTR_TYPE_TIME_T_SECONDS:
                        // Only parse if we don't already have a more precise string-based time
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
                        // Also attempt to parse this string into a unix timestamp
                        struct tm t = {0};
                        int year, month, day, hour, min, sec;
                        if (sscanf(attr_value, "%d-%d-%d %d:%d:%d", &day, &month, &year, &hour, &min, &sec) == 6) {
                            t.tm_year = year - 1900; t.tm_mon = month - 1; t.tm_mday = day;
                            t.tm_hour = hour; t.tm_min = min; t.tm_sec = sec;
                            time_t timestamp = timegm_portable(&t);
                            if (timestamp != (time_t)-1) {
                                metadata->timestamp_unix = timestamp;
                                metadata->timestamp_unix_present = true;
                            }
                        }
                        break;
                }
                break; // Found parser, move to next attribute
            }
        }
    }
}

/**
 * @brief Parses XML data from an auxi chunk using Expat.
 * @note This function is tolerant: it returns true if any useful data was parsed,
 *       even if Expat reports errors later in the chunk.
 */
static bool _parse_auxi_xml_expat(const unsigned char *chunk_data, sf_count_t chunk_size, SdrMetadata *metadata) {
    if (!chunk_data || chunk_size <= 0 || !metadata) return false;

    XML_Parser parser = XML_ParserCreate(NULL);
    if (!parser) return false;

    XML_SetUserData(parser, metadata);
    XML_SetElementHandler(parser, expat_start_element_handler, NULL);

    // Parse the chunk. We don't check the return status here because we want to
    // accept files that might have malformed XML but still contain the valid
    // <Definition> tag at the beginning.
    XML_Parse(parser, (const char*)chunk_data, chunk_size, 1);

    // Check if any metadata was actually populated by the callbacks
    bool any_data_parsed = metadata->software_name_present ||
                           metadata->radio_model_present ||
                           metadata->center_freq_hz_present ||
                           metadata->timestamp_unix_present;

    XML_ParserFree(parser);

    // If we parsed a software name, set the enum type for easier reference
    if (any_data_parsed && metadata->software_name_present) {
         if (strstr(metadata->software_name, "SDR Console") != NULL) {
              metadata->source_software = SDR_CONSOLE;
         }
    }

    return any_data_parsed;
}
