/**
 * @file sample_format_table.h
 * @brief Defines the unified registry and lookup functions for I/Q sample formats.
 *
 * This module provides a single source of truth for the properties of all
 * supported I/Q sample formats (e.g., byte sizes, names, default calibrations).
 * It replaces scattered switch statements with highly optimized O(1) jump tables.
 */

#ifndef SAMPLE_FORMAT_TABLE_H_
#define SAMPLE_FORMAT_TABLE_H_

#include "common_types.h"
#include <stddef.h>

/**
 * @struct SampleFormatInfo
 * @brief Contains metadata and DSP properties for a specific I/Q sample format.
 */
typedef struct {
    SampleFormat format_enum;         ///< The internal enumeration identifier.
    const char*  name_str;            ///< The short command-line identifier (e.g., "cs16", "cf32").
    const char*  description_str;     ///< A human-readable description for UI and logging.
    size_t       bytes_per_iq_sample; ///< Total memory footprint in bytes for one complete I/Q pair.
    float        default_filter_attenuation_db;      ///< Recommended default stop-band attenuation for DSP filters.
} SampleFormatInfo;

// --- Function Prototypes ---

/**
 * @brief Retrieves format metadata using its internal enumeration value.
 *
 * This function utilizes an O(1) jump table, making it extremely fast and
 * safe to call inside hot paths or tight loops.
 *
 * @param format The internal SampleFormat enum to look up.
 * @return A read-only pointer to the format's metadata, or NULL if invalid.
 */
const SampleFormatInfo* get_format_info_by_enum(SampleFormat format);

/**
 * @brief Retrieves format metadata by parsing a command-line string.
 *
 * Performs a case-insensitive search through the registered formats.
 * Primarily used during application startup to resolve user arguments.
 *
 * @param name The string identifier to look up (e.g., "cu8").
 * @return A read-only pointer to the format's metadata, or NULL if not found.
 */
const SampleFormatInfo* get_format_info_by_name(const char* name);

/**
 * @brief Utility wrapper to quickly fetch the byte size of a single I/Q frame.
 *
 * @param format The internal SampleFormat enum to look up.
 * @return The size in bytes of one I/Q pair, or 0 if the format is unknown.
 */
size_t get_bytes_per_iq_sample(SampleFormat format);

#endif // SAMPLE_FORMAT_TABLE_H_
