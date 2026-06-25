/**
 * @file constants.h
 * @brief Central configuration for application resource usage and performance tuning.
 *
 * This file defines the key parameters that govern the memory footprint, latency,
 * and processing quality of the iq_tool. Values here represent a balance
 * between performance, memory usage, and stability. Adjust these values to tune
 * the application for specific hardware or use cases.
 */

#ifndef CONSTANTS_H_
#define CONSTANTS_H_

// =============================================================================
// == Tier 1: High-Level Application Behavior
// =============================================================================

#define APP_NAME "iq_tool"
#define PRESETS_FILENAME "iq_tool_presets.conf"

// Defines the interval in seconds for console status updates and warning throttles to the console.
#define CONSOLE_UPDATE_INTERVAL_SEC 1.0

// =============================================================================
// == Tier 2: Core Memory & Pipeline Architecture
// =============================================================================
// These are the most critical parameters for controlling the application's
// memory footprint and real-time performance.

/**
 * @def MEM_ARENA_ALIGNMENT
 * @brief The memory alignment boundary for all allocations within the memory arena.
 *
 * Purpose: To ensure that all pointers returned by the arena are aligned to a
 * boundary suitable for high-performance SIMD (SSE/AVX) instructions, which
 * are heavily used by DSP libraries like liquid-dsp.
 */
#define MEM_ARENA_ALIGNMENT 128

/**
 * @def MEM_ARENA_SIZE_BYTES
 * @brief The size of the single memory arena for all startup and buffer allocations.
 *
 * Purpose: To hold all DSP objects, configuration strings, and other setup data,
 * eliminating hundreds of small malloc calls at startup.
 * Trade-off: Must hold large SDR buffers. 1 GB is safe as allocation is virtual.
 */
#define MEM_ARENA_SIZE_BYTES (1024 * 1024 * 1024) // 1 GB

/**
 * @def INPUT_BUFFER_DURATION_SEC
 * @brief Target duration (in seconds) for the input ring buffer.
 *
 * Purpose: Provides consistent latency tolerance across all sample rates.
 * The actual buffer size is calculated as: sample_rate × duration × bytes_per_sample
 *
 * Trade-off: 5 seconds provides good protection against driver hiccups while
 * remaining reasonable in memory usage. At 2.4 MHz this is ~48 MB, at 20 MHz ~400 MB.
 */
#define INPUT_BUFFER_DURATION_SEC 5.0f

/**
 * @def INPUT_BUFFER_MIN_BYTES
 * @brief Minimum size for the input buffer (safety floor).
 *
 * Prevents excessively small buffers on very low sample rate signals.
 */
#define INPUT_BUFFER_MIN_BYTES (4 * 1024 * 1024) // 4 MB

/**
 * @def INPUT_BUFFER_MAX_BYTES
 * @brief Maximum size for the input buffer (safety ceiling).
 *
 * Prevents excessive RAM usage on very high sample rate signals.
 */
#define INPUT_BUFFER_MAX_BYTES (512 * 1024 * 1024) // 512 MB

/**
 * @def PIPELINE_TARGET_BLOCK_SAMPLES
 * @brief The target number of samples for a processing block.
 *
 * This is calculated to ensure that the working set (Input Buffer + Output Buffer)
 * fits comfortably inside a standard CPU L2 Cache (256KB).
 *
 * Calculation: 12,288 samples * 8 bytes/sample (complex float) * 2 buffers (Ping/Pong)
 * = ~192 KB. This leaves ~64KB for instructions, stack, and OS overhead.
 */
#define PIPELINE_TARGET_BLOCK_SAMPLES 12288

/**
 * @def PIPELINE_MIN_READ_SAMPLES
 * @brief The minimum number of samples to read from the source per cycle.
 *
 * Prevents excessive mutex locking overhead during extreme upsampling scenarios
 * (e.g. where the calculated input requirement might be < 10 samples).
 */
#define PIPELINE_MIN_READ_SAMPLES 256

/**
 * @def PIPELINE_TARGET_BUFFER_DURATION_SEC
 * @brief Target amount of time to buffer inside the processing chain.
 *
 * Instead of a hardcoded number of chunks, we calculate the depth dynamically
 * to ensure we have enough buffer to survive disk stalls (e.g. 2 seconds).
 */
#define PIPELINE_TARGET_BUFFER_DURATION_SEC 2.0f

/**
 * @def PIPELINE_MIN_CHUNKS
 * @brief Minimum number of chunks in the pipeline (Sanity Floor).
 */
#define PIPELINE_MIN_CHUNKS 64

/**
 * @def PIPELINE_MAX_CHUNKS
 * @brief Maximum number of chunks in the pipeline (Sanity Ceiling).
 * Prevents excessive RAM usage on very low sample rate signals.
 */
#define PIPELINE_MAX_CHUNKS 16384

/**
 * @def PIPELINE_BUFFER_PADDING_SAMPLES
 * @brief Safety padding to absorb SIMD/AVX pre-fetches and floating point
 * rounding jitter. (Resampler bursts and FFT blocks are explicitly calculated).
 */
#define PIPELINE_BUFFER_PADDING_SAMPLES  64

// =============================================================================
// == Tier 3: DSP Algorithm Quality & Tuning
// =============================================================================

// Defines the default sharpness of user-defined FIR filters.
#define DEFAULT_FILTER_TRANSITION_FACTOR 0.25f

// The number of separate components in a complex sample (I and Q).
#define COMPLEX_SAMPLE_COMPONENTS 2

// The cutoff frequency for the DC blocking high-pass filter.
#define DC_BLOCK_CUTOFF_HZ 50.0f

// --- Filter Design & Analysis Tuning ---
#define FILTER_MINIMUM_TAPS 21

// Hard limit for auto-calculated filters. Prevents the auto-designer from
// The absolute maximum number of taps allowed when the engine auto-calculates filter length based on transition width.
#define FILTER_MAXIMUM_AUTO_TAPS 65536

// Used by initialization.c to budget memory when taps aren't explicitly defined.
#define FILTER_SAFETY_DEFAULT_TAPS FILTER_MAXIMUM_AUTO_TAPS

// Minimum gain threshold to safely trigger normalization and prevent divide-by-zero.
#define FILTER_GAIN_ZERO_THRESHOLD 1e-9f

// Number of frequency bins evaluated to find the absolute peak gain and prevent clipping.
#define FILTER_FREQ_RESPONSE_POINTS 2048

// --- I/Q Correction Algorithm Tuning ---
#define IQ_CORRECTION_FFT_SIZE           4096
#define IQ_CORRECTION_INTERVAL_MS        500
#define IQ_BASE_INCREMENT                0.0001f
#define IQ_MAX_PASSES                    25
#define IQ_CORRECTION_POWER_THRESHOLD_DB 20.0f
#define IQ_CORRECTION_SMOOTHING_FACTOR   0.05f

// --- Output AGC Tuning Parameters ---

// Harris/LMS AGC Logic
//
// The algorithm operates entirely in dB (treating the AGC as a linear
// system), applies a deadband so signals already in a good range are
// passed through untouched, and applies only a clean linear gain scalar
// to the samples — no nonlinear soft limiting.
//
// Signal chain:
//   [1] Pre-AGC impulse blanker  — zeros samples whose magnitude exceeds
//                                   AGC_BLANKER_THRESHOLD.
//   [2] Harris/LMS gain loop     — block RMS measurement, dB-domain LMS
//                                   update with deadband and gain rails.
//                                   Output is always a linear multiply.

/**
 * @def AGC_HARRIS_TARGET_DBFS
 * @brief Target RMS output level in dBFS for the Harris/LMS AGC.
 *
 * -18 dBFS gives high-PAPR signals with high peak-to-average power ratios comfortable
 * headroom below full scale. A signal arriving at -14 dBFS with a 6 dB
 * deadband has an error of +4 dB which falls inside the deadband, so
 * the gain stays at 0 dB and the signal passes through untouched.
 *
 */
#define AGC_HARRIS_TARGET_DBFS      -18.0f

/**
 * @def AGC_HARRIS_DEADBAND_DB
 * @brief Deadband half-width in dB for the Harris/LMS AGC.
 *
 * If |error| <= deadband, gain is frozen and samples pass through with
 * the current gain unchanged. Signals within [TARGET-DEADBAND, TARGET+DEADBAND]
 * dBFS receive no gain adjustment at all.
 *
 * With the default target of -18 dBFS and deadband of 1 dB, signals
 * in the range [-19, -17] dBFS are passed through untouched. This covers
 * the natural volume variations of most signals without constant intervention.
 *
 * Harris's original paper used 1 dB for discrete hardware VGA steps.
 * While software AGC has infinite gain resolution, using a tight 1 dB
 * deadband ensures weak signals are properly amplified to the target, while
 * still saving CPU cycles when the signal is stable.
 */
#define AGC_HARRIS_DEADBAND_DB       1.0f

/**
 * @def AGC_HARRIS_ALPHA
 * @brief LMS loop filter coefficient for the Harris/LMS AGC (0 < alpha < 1).
 *
 * Controls convergence speed. Smaller = slower and more stable.
 *
 * This AGC operates once per block rather than once per sample, so it
 * ticks many times per second at typical block sizes. A much smaller
 * value than Harris's original 0.8 (for infrequently-called hardware
 * VGA) is therefore appropriate.
 *
 * At alpha 0.2 and a 20 dB error (very weak signal), gain moves 4 dB
 * per block — converging in roughly 5 blocks. For a 6 dB error (just
 * outside the deadband), gain moves 1.2 dB per block.
 */
#define AGC_HARRIS_ALPHA             0.2f

/**
 * @def AGC_HARRIS_GAIN_MIN_DB
 * @brief Minimum gain the Harris/LMS AGC may apply, in dB.
 *
 * Negative values allow attenuation of hot signals.
 * -20 dB allows meaningful attenuation without the loop going to extremes.
 */
#define AGC_HARRIS_GAIN_MIN_DB      -20.0f

/**
 * @def AGC_HARRIS_GAIN_MAX_DB
 * @brief Maximum gain the Harris/LMS AGC may apply, in dB.
 *
 * +40 dB (100x linear) is enough to rescue a genuinely weak signal.
 * Caps gain to prevent amplifying a noise floor into something a decoder
 * mistakes for a signal.
 */
#define AGC_HARRIS_GAIN_MAX_DB       40.0f

// Impulse blanker threshold (absolute IQ magnitude).
// Any sample whose magnitude exceeds this value is zeroed before entering
// the AGC. Legitimate normalised signals should never exceed 1.0; hardware
// impulse artefacts commonly exceed 5-10. A value of 2.0 gives comfortable
// margin between the two without risk of blanking valid signal content.
#define AGC_BLANKER_THRESHOLD        2.0f

// How often to emit periodic AGC runtime status at debug level (seconds).
// Applies to all profiles. 5 seconds is frequent enough to observe gain
// riding during a fade without flooding the log during normal operation.
#define AGC_LOG_INTERVAL_SEC                 5.0f

// --- Dynamic DSP Filter Attenuation ---
#define DEFAULT_FILTER_ATTENUATION_8BIT_DB   60.0f
#define DEFAULT_FILTER_ATTENUATION_16BIT_DB  100.0f
#define DEFAULT_FILTER_ATTENUATION_24BIT_DB  144.0f
#define DEFAULT_FILTER_ATTENUATION_32BIT_DB  150.0f

// =============================================================================
// == Tier 4: Sanity Checks & Hard Limits
// =============================================================================

#define RESAMPLER_MIN_RATIO      0.001f
#define RESAMPLER_MAX_RATIO      1000.0f
#define FILTER_MAX_CHAIN          5
#define PRESETS_MAX_COUNT               128
#define PRESETS_MAX_LINE_LENGTH           1024
#define APP_MAX_SUMMARY_ITEMS         16

// The absolute maximum number of samples allowed in a single pipeline chunk.
// Prevents OOM crashes if user requests extreme upsampling + extreme filter taps.
#define PIPELINE_MAX_CHUNK_SAMPLES (4 * 1024 * 1024)
#define APP_MAX_PATH_BUFFER           4096

// =============================================================================
// == Tier 5: Application Lifecycle Tuning
// =============================================================================

/**
 * @def SDR_INITIALIZE_TIMEOUT_MS
 * @brief The maximum time to wait for an SDR driver to respond during initial opening.
 */
#define SDR_INITIALIZE_TIMEOUT_MS 10000

// The interval in milliseconds at which the watchdog thread wakes up to check the source heartbeat.
#define WATCHDOG_INTERVAL_MS 2000

// The maximum time in milliseconds that can elapse without an source heartbeat before the
// watchdog triggers a shutdown. This must be longer than any source's internal timeouts.
#define WATCHDOG_TIMEOUT_MS 8000

/**
 * @def NETWORK_SOCKET_TIMEOUT_MS
 * @brief The receive/send timeout for network sockets in milliseconds.
 *
 * Prevents the reader thread from hanging indefinitely if the server disappears
 * without closing the TCP connection (half-open socket).
 */
#define NETWORK_SOCKET_TIMEOUT_MS 5000

#endif // CONSTANTS_H_
