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

// Defines the interval in seconds for printing progress updates to the console.
#define PROGRESS_UPDATE_INTERVAL_SECONDS 1

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
#define MEM_ARENA_ALIGNMENT 32

// Ensure arena alignment is a power of 2 for aligned_alloc
_Static_assert((MEM_ARENA_ALIGNMENT & (MEM_ARENA_ALIGNMENT - 1)) == 0, "MEM_ARENA_ALIGNMENT must be a power of 2");

/**
 * @def MEM_ARENA_SIZE_BYTES
 * @brief The size of the single memory arena for all startup allocations.
 *
 * Purpose: To hold all DSP objects, configuration strings, and other setup data,
 * eliminating hundreds of small `malloc` calls at startup.
 * Trade-off: Must be large enough to hold all initialization data. 16MB is safe.
 */
#define MEM_ARENA_SIZE_BYTES (16 * 1024 * 1024) // 16 MB

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
// generating massive FFT blocks that blow out the L2 cache and pipeline buffers.
#define FILTER_MAXIMUM_AUTO_TAPS 8192

// Used by initialization.c to budget memory when taps aren't explicitly defined.
#define FILTER_SAFETY_DEFAULT_TAPS FILTER_MAXIMUM_AUTO_TAPS

#define FILTER_GAIN_ZERO_THRESHOLD 1e-9f
#define FILTER_FREQ_RESPONSE_POINTS 2048

// --- I/Q Correction Algorithm Tuning ---
#define IQ_CORRECTION_FFT_SIZE           4096
#define IQ_CORRECTION_INTERVAL_MS        500
#define IQ_BASE_INCREMENT                0.0001f
#define IQ_MAX_PASSES                    25
#define IQ_CORRECTION_POWER_THRESHOLD_DB 20.0f
#define IQ_CORRECTION_SMOOTHING_FACTOR   0.05f

// --- Output AGC Tuning Parameters ---

// 1. DX Profile
// Strategy: Very slow RMS tracking to ride out atmospheric fading on weak signals.
// Target: 0.5 (-6 dBFS RMS). Safe headroom for general analog content.
#define AGC_DX_BANDWIDTH         1e-4f
#define AGC_DX_TARGET            0.5f

// 2. Local Profile
// Strategy: Fast RMS tracking for strong, stable analog signals (voice/music).
// Target: 0.5 (-6 dBFS RMS).
#define AGC_LOCAL_BANDWIDTH      1e-2f
#define AGC_LOCAL_TARGET         0.5f

// 3. Digital Profile
// Strategy: Harris/LMS block-level AGC operating in the dB domain.
//           Inspired by Fred Harris & Gregory Smith, "On the Design,
//           Implementation, and Performance of a Microprocessor-Controlled
//           AGC System for a Digital Receiver", and documented in
//           Richard G. Lyons, "Understanding Digital Signal Processing",
//           3rd ed., Section 13.30.
//
// The algorithm operates entirely in dB (treating the AGC as a linear
// system), applies a deadband so signals already in a good range are
// passed through untouched, and applies only a clean linear gain scalar
// to the samples — no nonlinear soft limiting.
//
// Signal chain:
//   [1] Pre-AGC impulse blanker  — zeros samples whose magnitude exceeds
//                                   AGC_DIGITAL_BLANKER_THRESHOLD.
//   [2] Harris/LMS gain loop     — block RMS measurement, dB-domain LMS
//                                   update with deadband and gain rails.
//                                   Output is always a linear multiply.

/**
 * @def AGC_DIGITAL_HARRIS_TARGET_DBFS
 * @brief Target RMS output level in dBFS for the Harris/LMS AGC.
 *
 * -18 dBFS gives high-PAPR OFDM signals (10-13 dB PAPR) comfortable
 * headroom below full scale. A signal arriving at -14 dBFS with a 6 dB
 * deadband has an error of +4 dB which falls inside the deadband, so
 * the gain stays at 0 dB and the signal passes through untouched.
 *
 */
#define AGC_DIGITAL_HARRIS_TARGET_DBFS      -18.0f

/**
 * @def AGC_DIGITAL_HARRIS_DEADBAND_DB
 * @brief Deadband half-width in dB for the Harris/LMS AGC.
 *
 * If |error| <= deadband, gain is frozen and samples pass through with
 * the current gain unchanged. Signals within [TARGET-DEADBAND, TARGET+DEADBAND]
 * dBFS receive no gain adjustment at all.
 *
 * With the default target of -18 dBFS and deadband of 6 dB, signals
 * in the range [-24, -12] dBFS are passed through untouched. This covers
 * most well-received digital signals without any intervention.
 *
 * Harris's original paper used 1 dB for discrete hardware VGA steps.
 * A wider deadband is appropriate for software AGC with infinite gain
 * resolution — err on the side of not touching the signal.
 */
#define AGC_DIGITAL_HARRIS_DEADBAND_DB       6.0f

/**
 * @def AGC_DIGITAL_HARRIS_ALPHA
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
#define AGC_DIGITAL_HARRIS_ALPHA             0.2f

/**
 * @def AGC_DIGITAL_HARRIS_GAIN_MIN_DB
 * @brief Minimum gain the Harris/LMS AGC may apply, in dB.
 *
 * Negative values allow attenuation of hot signals.
 * -20 dB allows meaningful attenuation without the loop going to extremes.
 */
#define AGC_DIGITAL_HARRIS_GAIN_MIN_DB      -20.0f

/**
 * @def AGC_DIGITAL_HARRIS_GAIN_MAX_DB
 * @brief Maximum gain the Harris/LMS AGC may apply, in dB.
 *
 * +40 dB (100x linear) is enough to rescue a genuinely weak signal.
 * Caps gain to prevent amplifying a noise floor into something a decoder
 * mistakes for a signal.
 */
#define AGC_DIGITAL_HARRIS_GAIN_MAX_DB       40.0f

// Impulse blanker threshold (absolute IQ magnitude).
// Any sample whose magnitude exceeds this value is zeroed before entering
// the AGC. Legitimate normalised signals should never exceed 1.0; hardware
// impulse artefacts commonly exceed 5-10. A value of 2.0 gives comfortable
// margin between the two without risk of blanking valid signal content.
#define AGC_DIGITAL_BLANKER_THRESHOLD        2.0f

// How often to emit periodic AGC runtime status at debug level (seconds).
// Applies to all profiles. 5 seconds is frequent enough to observe gain
// riding during a fade without flooding the log during normal operation.
#define AGC_LOG_INTERVAL_SEC                 5.0f

// =============================================================================
// == Tier 4: SDR Hardware Interaction & Tuning
// =============================================================================
// Default values and tuning parameters specific to each SDR device.

// --- RTLSDR Constants ---
#define RTLSDR_DBM_OFFSET              -80.0f
#define RTLSDR_DEFAULT_ATTENUATION     60.0f
#define RTLSDR_DEFAULT_SAMPLE_RATE     2400000.0
#define RTLSDR_PASSTHROUGH_BUFFER_SIZE 16384

// --- SDRPLAY Unconditional Constants ---
#define SDRPLAY_DBM_OFFSET             -25.0f
#define SDRPLAY_DEFAULT_ATTENUATION    80.0f
#define SDRPLAY_DEFAULT_SAMPLE_RATE_HZ 2000000.0
#define SDRPLAY_DEFAULT_BANDWIDTH_HZ   1536000.0
#define SDRPLAY_DEFAULT_IF_GAIN_DB     -50

// Define a safe max chunk size (16384 samples * 2 channels * 2 bytes = 64KB)
#define MAX_SDRPLAY_CONVERSION_SAMPLES 16384

// --- HACKRF Constants ---
#define HACKRF_DBM_OFFSET           -85.0f
#define HACKRF_DEFAULT_ATTENUATION   60.0f
#define HACKRF_DEFAULT_SAMPLE_RATE   8000000.0
#define HACKRF_DEFAULT_LNA_GAIN      16
#define HACKRF_DEFAULT_VGA_GAIN      0

// --- BLADERF Constants ---
#define BLADERF_DBM_OFFSET             -25.0f
#define BLADERF_DEFAULT_ATTENUATION    80.0f
#define BLADERF_DEFAULT_SAMPLE_RATE_HZ 2000000
#define BLADERF_DEFAULT_BANDWIDTH_HZ   1500000

// --- BladeRF Async Stream Tuning ---
#define BLADERF_TRANSFER_SIZE_SECONDS    0.020

// --- BladeRF Adaptive Streaming Profiles ---
#define BLADERF_PROFILE_LOWLATENCY_NUM_BUFFERS        32
#define BLADERF_PROFILE_LOWLATENCY_NUM_TRANSFERS      16

#define BLADERF_PROFILE_BALANCED_NUM_BUFFERS          64
#define BLADERF_PROFILE_BALANCED_NUM_TRANSFERS        32

#define BLADERF_PROFILE_HIGHTHROUGHPUT_NUM_BUFFERS    96
#define BLADERF_PROFILE_HIGHTHROUGHPUT_NUM_TRANSFERS  48

// --- AIRSPY Constants ---
#define AIRSPY_DBM_OFFSET           -50.0f
#define AIRSPY_DEFAULT_ATTENUATION   80.0f
#define AIRSPY_DEFAULT_SAMPLE_RATE   2500000.0
#define AIRSPY_DEFAULT_SAMPLE_FORMAT CS16
#define AIRSPY_DEFAULT_GAIN_VALUE    10
#define AIRSPY_DEFAULT_LNA_GAIN      5
#define AIRSPY_DEFAULT_MIXER_GAIN    5
#define AIRSPY_DEFAULT_VGA_GAIN      5

// --- AIRSPY HF+ Constants ---
#define AIRSPYHF_DBM_OFFSET           -15.0f
#define AIRSPYHF_DEFAULT_ATTENUATION   100.0f
#define AIRSPYHF_DEFAULT_SAMPLE_RATE   768000

// --- SPYSERVER Constants --
#define SPYSERVER_DEFAULT_SAMPLE_RATE_HZ 600000.0

// Absolute hard limit for the ring buffer size (128 MB)
#define SPYSERVER_MAX_BUFFER_BYTES (128 * 1024 * 1024)

// Start processing only after buffering this much data
#define SPYSERVER_PREBUFFER_TARGET_SECONDS 2.5f

// Minimum data floor required to trigger pre-buffering
#define SPYSERVER_PREBUFFER_MIN_BYTES 65536

// Total buffer capacity multiplier relative to target (Safety Margin)
#define SPYSERVER_BUFFER_HEADROOM_FACTOR 4.0f

// Absolute minimum capacity for the ring buffer (1 MB)
#define SPYSERVER_RING_BUFFER_MIN_BYTES (1024 * 1024)

// Cap pre-buffering at 80% of capacity to prevent immediate overrun
#define SPYSERVER_PREBUFFER_MAX_FILL_RATIO 0.8f

// --- Format-Based Offsets for WAV/RAW Files ---
#define FORMAT_DBM_OFFSET_8BIT    -80.0f
#define FORMAT_DBM_OFFSET_16BIT   -65.0f
#define FORMAT_DBM_OFFSET_24BIT   -130.0f
#define FORMAT_DBM_OFFSET_32BIT   0.0f

// --- Dynamic DSP Filter Attenuation ---
#define FORMAT_ATTENUATION_8BIT   60.0f
#define FORMAT_ATTENUATION_16BIT  100.0f
#define FORMAT_ATTENUATION_24BIT  144.0f
#define FORMAT_ATTENUATION_32BIT  150.0f

// =============================================================================
// == Tier 5: Sanity Checks & Hard Limits
// =============================================================================

#define MIN_ACCEPTABLE_RATIO      0.001f
#define MAX_ACCEPTABLE_RATIO      1000.0f
#define SHIFT_FACTOR_LIMIT        5.0
#define MAX_FILTER_CHAIN          5
#define MAX_PRESETS               128
#define MAX_LINE_LENGTH           1024
#define MAX_SUMMARY_ITEMS         16

// The absolute maximum number of samples allowed in a single pipeline chunk.
// Prevents OOM crashes if user requests extreme upsampling + extreme filter taps.
#define PIPELINE_MAX_CHUNK_SAMPLES (4 * 1024 * 1024)
#define MAX_PATH_BUFFER           4096

// =============================================================================
// == Tier 6: Application Lifecycle Tuning
// =============================================================================

/**
 * @def SDR_INITIALIZE_TIMEOUT_MS
 * @brief The maximum time to wait for an SDR driver to respond during initial opening.
 */
#define SDR_INITIALIZE_TIMEOUT_MS 10000

// The interval in milliseconds at which the watchdog thread wakes up to check the SDR heartbeat.
#define WATCHDOG_INTERVAL_MS 2000

// The maximum time in milliseconds that can elapse without an SDR heartbeat before the
// watchdog triggers a shutdown. This must be longer than any SDR's internal timeouts.
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
