#ifndef CONSTANTS_H_
#define CONSTANTS_H_

/**
 * @file constants.h
 * @brief Central configuration for application resource usage and performance tuning.
 */

// =============================================================================
// == Tier 1: High-Level Application Behavior
// =============================================================================

#define APP_NAME "iq_tool"
#define PRESETS_FILENAME "iq_tool_presets.conf"
#define PROGRESS_UPDATE_INTERVAL_SECONDS 1

// =============================================================================
// == Tier 2: Core Memory & Pipeline Architecture
// =============================================================================

#define MEM_ARENA_ALIGNMENT 32
#define MEM_ARENA_SIZE_BYTES (16 * 1024 * 1024)
#define IO_SDR_INPUT_BUFFER_BYTES (256 * 1024 * 1024)
#define IO_OUTPUT_WRITER_BUFFER_BYTES (1024 * 1024 * 1024)
#define IO_OUTPUT_WRITER_CHUNK_SIZE (1024 * 1024)
#define IO_WRITER_BUFFER_HIGH_WATER_MARK 0.95f
#define PIPELINE_NUM_CHUNKS 512
#define PIPELINE_CHUNK_BASE_SAMPLES 16384
#define RESAMPLER_OUTPUT_SAFETY_MARGIN  128

// =============================================================================
// == Tier 3: DSP Algorithm Quality & Tuning
// =============================================================================

#define RESAMPLER_QUALITY_ATTENUATION_DB 60.0f
#define DEFAULT_FILTER_TRANSITION_FACTOR 0.25f
#define COMPLEX_SAMPLE_COMPONENTS 2
#define DC_BLOCK_CUTOFF_HZ 10.0f

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

// 3. Digital Profile (Peak-Based)
// Strategy: Scan for peaks, calculate gain, and LOCK.
// Target: 0.9 (-1 dBFS Peak).
// Why 0.9? Since we are locking based on the absolute maximum peak,
// we can safely push the signal very close to full scale (1.0)
// to maximize bit depth usage without fear of clipping.
#define AGC_DIGITAL_PEAK_TARGET  0.9f
#define AGC_DIGITAL_LOCK_TIME    2.0f // Seconds to scan before locking

// =============================================================================
// == Tier 4: SDR Hardware Interaction & Tuning
// =============================================================================

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
#define BLADERF_SYNC_CONFIG_TIMEOUT_MS   3500
#define BLADERF_SYNC_RX_TIMEOUT_MS       5000
#define BLADERF_TRANSFER_SIZE_SECONDS    0.25
#define BLADERF_PROFILE_LOWLATENCY_NUM_BUFFERS        32
#define BLADERF_PROFILE_LOWLATENCY_BUFFER_SIZE        16384
#define BLADERF_PROFILE_LOWLATENCY_NUM_TRANSFERS      16
#define BLADERF_PROFILE_BALANCED_NUM_BUFFERS          64
#define BLADERF_PROFILE_BALANCED_BUFFER_SIZE          32768
#define BLADERF_PROFILE_BALANCED_NUM_TRANSFERS        32
#define BLADERF_PROFILE_HIGHTHROUGHPUT_NUM_BUFFERS    64
#define BLADERF_PROFILE_HIGHTHROUGHPUT_BUFFER_SIZE    65536
#define BLADERF_PROFILE_HIGHTHROUGHPUT_NUM_TRANSFERS  32
#endif

#define SPYSERVER_DEFAULT_SAMPLE_RATE_HZ 600000.0
#define SPYSERVER_STREAM_BUFFER_BYTES (16 * 1024 * 1024)
#define SPYSERVER_PREBUFFER_HIGH_WATER_MARK 0.5f

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

#define SDR_INITIALIZE_TIMEOUT_MS 10000
#define WATCHDOG_INTERVAL_MS 2000
#define WATCHDOG_TIMEOUT_MS 8000

#endif // CONSTANTS_H_
