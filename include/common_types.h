/**
 * @file common_types.h
 * @brief Defines fundamental, low-level data types and enums for the application.
 *
 * This header contains the most basic and widely-used type definitions, such as
 * the complex float type, sample format enums, and output type enums. It is
 * designed to be a low-level dependency with no includes from other parts of
 * this project, ensuring it can be safely included by any module without
 * creating circular dependencies.
 */

#ifndef COMMON_TYPES_H_
#define COMMON_TYPES_H_

#include <stdint.h>
#include <stdbool.h>
#include <complex.h>

// --- Core Data Types ---

/**
 * @typedef complex_float_t
 * @brief The standard internal representation for I/Q samples throughout the DSP pipeline.
 */
typedef float complex complex_float_t;


// --- Enumerations ---

/**
 * @enum format_t
 * @brief Enumerates all supported raw I/Q sample formats.
 */
typedef enum {
    FORMAT_UNKNOWN,
    U8,
    S8,
    U16,
    S16,
    U32,
    S32,
    F32,
    CU8,
    CS8,
    CU16,
    CS16,
    CU32,
    CS32,
    CF32,
    SC16Q11
} format_t;

/**
 * @enum OutputType
 * @brief Enumerates the supported output file container formats.
 */
typedef enum {
    OUTPUT_TYPE_RAW,
    OUTPUT_TYPE_WAV,
    OUTPUT_TYPE_WAV_RF64
} OutputType;

/**
 * @enum FilterType
 * @brief Enumerates the types of user-configurable FIR filters.
 */
typedef enum {
    FILTER_TYPE_NONE,
    FILTER_TYPE_LOWPASS,
    FILTER_TYPE_HIGHPASS,
    FILTER_TYPE_PASSBAND,
    FILTER_TYPE_STOPBAND
} FilterType;

/**
 * @enum FilterImplementationType
 * @brief Enumerates the actual underlying implementation of a filter object.
 */
typedef enum {
    FILTER_IMPL_NONE,
    FILTER_IMPL_FIR_SYMMETRIC,
    FILTER_IMPL_FIR_ASYMMETRIC,
    FILTER_IMPL_FFT_SYMMETRIC,
    FILTER_IMPL_FFT_ASYMMETRIC
} FilterImplementationType;

/**
 * @enum FilterTypeRequest
 * @brief Represents the user's explicit request for a filter implementation type.
 */
typedef enum {
    FILTER_TYPE_AUTO, // Default, let the application decide
    FILTER_TYPE_FIR,
    FILTER_TYPE_FFT
} FilterTypeRequest;

/**
 * @enum PipelineMode
 * @brief Defines the high-level operational mode of the processing pipeline.
 */
typedef enum {
    PIPELINE_MODE_REALTIME_SDR,     // SDR input to stdout, prioritizing low latency.
    PIPELINE_MODE_BUFFERED_SDR,     // SDR input to file, prioritizing throughput and stability.
    PIPELINE_MODE_FILE_PROCESSING   // File input to file/stdout, self-paced.
} PipelineMode;

#endif // COMMON_TYPES_H_
