/**
 * @file common_types.h
 * @brief Defines fundamental, low-level data types and enums for the application.
 */

#ifndef COMMON_TYPES_H_
#define COMMON_TYPES_H_

#include <stdint.h>
#include <stdbool.h>
#include <complex.h>

// --- Core Data Types ---
typedef float complex ComplexFloat;

// Enforce 8-byte sizing for binary compatibility with Liquid-DSP
_Static_assert(sizeof(ComplexFloat) == 8, "ComplexFloat must be exactly 8 bytes (2x 32-bit floats)");

// --- Enumerations ---

typedef enum {
    LIFECYCLE_STATE_START,
    LIFECYCLE_STATE_INPUT_INITIALIZED,
    LIFECYCLE_STATE_DC_BLOCK_CREATED,
    LIFECYCLE_STATE_IQ_CORRECTOR_CREATED,
    LIFECYCLE_STATE_FREQ_SHIFTER_CREATED,
    LIFECYCLE_STATE_RESAMPLER_CREATED,
    LIFECYCLE_STATE_FILTER_CREATED,
    LIFECYCLE_STATE_BUFFERS_ALLOCATED,
    LIFECYCLE_STATE_THREADS_CREATED,
    LIFECYCLE_STATE_IO_BUFFERS_CREATED,
    LIFECYCLE_STATE_OUTPUT_STREAM_OPEN,
    LIFECYCLE_STATE_FULLY_INITIALIZED
} AppLifecycleState;

typedef enum {
    FORMAT_UNKNOWN,
    U8, S8, U16, S16, U24, S24, U32, S32, F32,
    CU8, CS8, CU16, CS16, CS24, CU32, CS32, CF32, SC16Q11
} SampleFormat;

typedef enum {
    PAYLOAD_IQ,
    PAYLOAD_AUDIO
} OutputPayload;

typedef enum {
    FILTER_TYPE_NONE,
    FILTER_TYPE_LOWPASS,
    FILTER_TYPE_HIGHPASS,
    FILTER_TYPE_PASSBAND,
    FILTER_TYPE_STOPBAND
} FilterType;

typedef enum {
    FILTER_IMPL_NONE,
    FILTER_IMPL_FIR_SYMMETRIC,
    FILTER_IMPL_FIR_ASYMMETRIC,
    FILTER_IMPL_FFT_SYMMETRIC,
    FILTER_IMPL_FFT_ASYMMETRIC
} FilterImplementationType;

typedef enum {
    FILTER_TYPE_AUTO,
    FILTER_TYPE_FIR,
    FILTER_TYPE_FFT
} FilterTypeRequest;

typedef enum {
    PIPELINE_MODE_ASYNCHRONOUS_PUSH,
    PIPELINE_MODE_SYNCHRONOUS_PULL
} PipelineMode;

#endif // COMMON_TYPES_H_
