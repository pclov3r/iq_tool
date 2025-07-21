#ifndef METADATA_H_
#define METADATA_H_

#include <stdbool.h>
#include <time.h>   // For time_t
#include <stdint.h> // For int64_t
#include <math.h>   // For isfinite

#include <sndfile.h>
#include "config.h" // For MAX_PATH_LEN

// --- Enums ---

typedef enum {
    SDR_SOFTWARE_UNKNOWN,
    SDR_CONSOLE,
    SDR_SHARP,
    SDR_UNO,
    SDR_CONNECT,
} SdrSoftwareType;

// --- Structs ---

/**
 * @brief Holds metadata parsed from an SDR capture file.
 * @note This struct only contains fields that are actively used by this application
 *       for display or calculation purposes.
 */
typedef struct {
    // --- Identification ---
    SdrSoftwareType source_software;
    char software_name[64];
    char software_version[64];
    char radio_model[128];
    bool software_name_present;
    bool software_version_present;
    bool radio_model_present;

    // --- Radio Parameters ---
    double center_freq_hz;
    bool center_freq_hz_present;

    // --- Timestamp Info ---
    time_t timestamp_unix;
    char timestamp_str[64];
    bool timestamp_unix_present;
    bool timestamp_str_present;

} SdrMetadata;

// --- Function Declarations ---

/**
 * @brief Initializes the SdrMetadata struct to default/unknown values.
 * @param metadata Pointer to the SdrMetadata struct to initialize.
 */
void init_sdr_metadata(SdrMetadata *metadata);

/**
 * @brief Attempts to parse SDR-specific metadata chunks from a WAV file.
 * @param infile An open libsndfile handle.
 * @param sfinfo Pointer to the SF_INFO struct for the file.
 * @param metadata Pointer to the SdrMetadata struct to populate.
 * @return true if any relevant metadata was found and parsed, false otherwise.
 */
bool parse_sdr_metadata_chunks(SNDFILE *infile, const SF_INFO *sfinfo, SdrMetadata *metadata);

/**
 * @brief Attempts to parse SDR metadata (frequency, timestamp) from a filename.
 * @param base_filename The filename (without path) to parse.
 * @param metadata Pointer to the SdrMetadata struct to populate.
 * @return true if any relevant metadata was found and parsed, false otherwise.
 */
bool parse_sdr_metadata_from_filename(const char* base_filename, SdrMetadata *metadata);

/**
 * @brief Converts an SdrSoftwareType enum value to a human-readable string.
 * @param type The enum value to convert.
 * @return A constant string representing the software type.
 */
const char* sdr_software_type_to_string(SdrSoftwareType type);


#endif // METADATA_H_
