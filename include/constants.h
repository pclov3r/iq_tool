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
// Set to 0 to disable progress updates entirely.
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
 * @def IO_SDR_INPUT_BUFFER_BYTES
 * @brief The size of the ring buffer between the SDR capture thread and the reader thread.
 *
 * Purpose: To absorb latency spikes from the OS or SDR driver and prevent sample
 * drops during heavy processing. Critical for stability in buffered SDR mode.
 */
#define IO_SDR_INPUT_BUFFER_BYTES (256 * 1024 * 1024) // 256 MB

/**
 * @def IO_OUTPUT_WRITER_BUFFER_BYTES
 * @brief The size of the ring buffer between the post-processor thread and the writer thread.
 *
 * Purpose: A large size is critical for absorbing I/O latency spikes from the
 * filesystem (e.g., from antivirus scans), preventing the real-time pipeline from stalling.
 */
#define IO_OUTPUT_WRITER_BUFFER_BYTES (1024 * 1024 * 1024) // 1 GB

/**
 * @def IO_OUTPUT_WRITER_CHUNK_SIZE
 * @brief The size of the local buffer in the writer thread for disk writes.
 */
#define IO_OUTPUT_WRITER_CHUNK_SIZE (1024 * 1024) // 1 MB

/**
 * @def IO_WRITER_BUFFER_HIGH_WATER_MARK
 * @brief The fullness threshold (as a fraction, 0.0-1.0) for the writer buffer
 *        that triggers back-pressure on the reader thread.
 */
#define IO_WRITER_BUFFER_HIGH_WATER_MARK 0.95f

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
 * @def RESAMPLER_OUTPUT_SAFETY_MARGIN
 * @brief A safety margin for the resampler's output buffer calculation.
 */
#define RESAMPLER_OUTPUT_SAFETY_MARGIN  128

// =============================================================================
// == Tier 3: DSP Algorithm Quality & Tuning
// =============================================================================

// The default stop-band attenuation for the liquid-dsp resampler, in dB.
#define RESAMPLER_QUALITY_ATTENUATION_DB 60.0f

// Defines the default sharpness of user-defined FIR filters.
#define DEFAULT_FILTER_TRANSITION_FACTOR 0.25f

// The number of separate components in a complex sample (I and Q).
#define COMPLEX_SAMPLE_COMPONENTS 2

// The cutoff frequency for the DC blocking high-pass filter.
#define DC_BLOCK_CUTOFF_HZ 50.0f

// --- Filter Design & Analysis Tuning ---
#define FILTER_MINIMUM_TAPS 21
#define FILTER_GAIN_ZERO_THRESHOLD 1e-9f
#define FILTER_FREQ_RESPONSE_POINTS 2048

// --- I/Q Correction Algorithm Tuning ---
#define IQ_CORRECTION_FFT_SIZE           1024
#define IQ_CORRECTION_INTERVAL_MS        500
#define IQ_BASE_INCREMENT                0.0001f
#define IQ_MAX_PASSES                    25
#define IQ_CORRECTION_POWER_THRESHOLD_DB 20.0f
#define IQ_CORRECTION_SMOOTHING_FACTOR   0.05f

// --- Output AGC Tuning Parameters ---

// 1. DX Profile (RMS-Based)
// Strategy: Slow tracking to compensate for atmospheric fading.
// Target: 0.5 (-6 dBFS RMS). Safe for general analog signals.
#define AGC_DX_BANDWIDTH         1e-4f
#define AGC_DX_TARGET            0.5f

// 2. Local Profile (RMS-Based)
// Strategy: Fast tracking for strong analog signals (Voice/Music).
// Target: 0.5 (-6 dBFS RMS).
#define AGC_LOCAL_BANDWIDTH      1e-2f
#define AGC_LOCAL_TARGET         0.5f

// 3. Digital Profile (Adaptive Tracking)
// Strategy: Smart Limiter with Hysteresis.
// Goal: Keep peaks within a "Stability Window" to preserve MER.

// The default target peak level (0.9 = -1 dB).
// This leaves slight headroom for inter-sample peaks during DAC reconstruction.
#define AGC_DIGITAL_PEAK_TARGET       0.9f

// The lower bound of the stability window, as a fraction of the target.
// 0.6 = -4.4 dB relative to target (-5.4 dB total).
// Gain will NOT change if the signal is between Target and (Target * 0.6).
#define AGC_DIGITAL_STABILITY_WINDOW  0.6f

// The noise floor threshold, as a fraction of the target.
// 0.1 = -20 dB relative to target.
// If signal drops below this, AGC assumes silence and holds gain (Noise Gate).
#define AGC_DIGITAL_NOISE_THRESHOLD   0.1f

// The "Slew Rate" (Tracking Speed).
// Defines how fast the gain moves towards the target when outside the window.
// 0.01 = Adjusts 1% of the error per block.
#define AGC_DIGITAL_SLEW_RATE         0.5f

// Startup Transient Detection
// If Crest Factor (Peak/Average) > 10.0, the block is considered to have a transient.
#define AGC_DIGITAL_CREST_FACTOR_THRESHOLD 10.0f

// Robust Average Calculation
// When a transient is detected, we exclude samples > (Average * 3.0).
#define AGC_DIGITAL_ROBUST_EXCLUSION_FACTOR 3.0f

// Robust Average Multiplier
// Scales the Robust Average to estimate the effective peak amplitude of the signal body.
// The value 4.1297f was derived empirically from real-world I/Q recordings containing
// a significant startup transient. This calibration targets optimal gain for digital
// modes but may require future refinement to generalize across all signal conditions.
#define AGC_DIGITAL_ROBUST_AVG_MULTIPLIER   4.1290f

// Runtime Safety Clamp
// We allow peaks to saturate up to 3.0x (300%) before triggering an emergency gain cut.
// This prevents the AGC from reacting to short transients during operation.
#define AGC_DIGITAL_SAFETY_CLAMP            3.0f

// =============================================================================
// == Tier 4: SDR Hardware Interaction & Tuning
// =============================================================================
// Default values and tuning parameters specific to each SDR device.

#if defined(WITH_RTLSDR)
#define RTLSDR_DEFAULT_SAMPLE_RATE 2400000.0
#endif

#if defined(WITH_SDRPLAY)
#define SDRPLAY_DEFAULT_SAMPLE_RATE_HZ 2000000.0
#define SDRPLAY_DEFAULT_BANDWIDTH_HZ   1536000.0
#define SDRPLAY_DEFAULT_IF_GAIN_DB     -50
#endif

#if defined(WITH_HACKRF)
#define HACKRF_DEFAULT_SAMPLE_RATE 8000000.0
#define HACKRF_DEFAULT_LNA_GAIN    16
#define HACKRF_DEFAULT_VGA_GAIN    0
#endif

#if defined(WITH_BLADERF)
#define BLADERF_DEFAULT_SAMPLE_RATE_HZ 2000000
#define BLADERF_DEFAULT_BANDWIDTH_HZ   1500000
// --- BladeRF Stream Tuning ---
#define BLADERF_SYNC_CONFIG_TIMEOUT_MS   3500
#define BLADERF_SYNC_RX_TIMEOUT_MS       5000
#define BLADERF_TRANSFER_SIZE_SECONDS    0.25

// --- BladeRF Adaptive Streaming Profiles ---
#define BLADERF_PROFILE_LOWLATENCY_NUM_BUFFERS        32
#define BLADERF_PROFILE_LOWLATENCY_BUFFER_SIZE        16384
#define BLADERF_PROFILE_LOWLATENCY_NUM_TRANSFERS      16

#define BLADERF_PROFILE_BALANCED_NUM_BUFFERS          64
#define BLADERF_PROFILE_BALANCED_BUFFER_SIZE          32768
#define BLADERF_PROFILE_BALANCED_NUM_TRANSFERS        32

#define BLADERF_PROFILE_HIGHTHROUGHPUT_NUM_BUFFERS    64
#define BLADERF_PROFILE_HIGHTHROUGHPUT_BUFFER_SIZE    65536
#define BLADERF_PROFILE_HIGHTHROUGHPUT_NUM_TRANSFERS  32
#endif // defined(WITH_BLADERF)

#define SPYSERVER_DEFAULT_SAMPLE_RATE_HZ 600000.0
#define SPYSERVER_MAX_BUFFER_BYTES (128 * 1024 * 1024) // 128 MB maximum buffer size
#define SPYSERVER_PREBUFFER_TARGET_SECONDS 2.5f // Buffer 2.5 seconds of I/Q data before starting pipeline
#define SPYSERVER_PREBUFFER_MIN_BYTES 65536 // Minimum floor of 64KB for pre-buffering
#define SPYSERVER_PREBUFFER_MAX_FILL_RATIO 0.8f // Cap pre-buffering at 80% of capacity to prevent immediate overrun

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
#define MAX_ALLOWED_FFT_BLOCK_SIZE (1024 * 1024)
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

#endif // CONSTANTS_H_
