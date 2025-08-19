// src/config.h

#ifndef CONFIG_H_
#define CONFIG_H_

// --- Application Constants ---
#define APP_NAME "iq_resample_tool"
#define PRESETS_FILENAME "iq_resample_tool_presets.conf"

// --- Pipeline & Buffer Configuration ---
#define STOPBAND_ATTENUATION_DB         60.0f

// Defines the interval in seconds for printing progress updates to the console.
// Set to 0 to disable progress updates entirely.
#define PROGRESS_UPDATE_INTERVAL_SECONDS 1

#define NUM_BUFFERS                     512 // Increased for stability against pipeline stalls
#define BUFFER_SIZE_SAMPLES             131072
#define RESAMPLER_OUTPUT_SAFETY_MARGIN  128 // Extra samples for resampler output buffer

// Size of the decoupled I/O ring buffer. A large size is critical for absorbing
// I/O latency spikes from the OS/antivirus on Windows.
#define IO_RING_BUFFER_CAPACITY         (1024 * 1024 * 1024) // 1 GB

// The size of the local buffer in the writer thread. Data is read from the
// ring buffer into this local buffer before being written to disk.
#define WRITER_THREAD_CHUNK_SIZE        (1024 * 1024) // 1 MB

// Defines the size of the input ring buffer used only in buffered SDR mode to
// prevent sample drops during heavy processing.
#define SDR_INPUT_BUFFER_CAPACITY       (256 * 1024 * 1024) // 256 MB

// --- DSP & Sanity Check Limits ---
#define MIN_ACCEPTABLE_RATIO      0.001f
#define MAX_ACCEPTABLE_RATIO      1000.0f
#define SHIFT_FACTOR_LIMIT        5.0
#define DC_BLOCK_CUTOFF_HZ        10.0f

// --- FIR Filter Defaults ---
// Defines the default sharpness of the filter. The transition width will be
// this fraction of the filter's characteristic frequency (cutoff or bandwidth).
// A smaller value results in a sharper, higher-quality (but more CPU-intensive) filter.
#define DEFAULT_FILTER_TRANSITION_FACTOR 0.25f
#define MAX_FILTER_CHAIN                 5 // Allow up to 5 filters to be chained together.

// --- I/Q Correction Algorithm Tuning ---
#define IQ_CORRECTION_FFT_SIZE           1024
#define IQ_CORRECTION_DEFAULT_PERIOD     2000000 // Samples between optimization runs
#define IQ_BASE_INCREMENT                0.0001f // Step size for the optimizer
#define IQ_MAX_PASSES                    25      // Iterations per optimization run
#define IQ_CORRECTION_PEAK_THRESHOLD_DB -60.0f   // Signal power threshold to trigger optimization
#define IQ_CORRECTION_SMOOTHING_FACTOR   0.05f   // Smoothing factor for updating correction params

// --- Parsing & Resource Limits ---
#define MAX_PRESETS               128
#define MAX_LINE_LENGTH           1024
#define MAX_SUMMARY_ITEMS         16 // Max number of lines in the pre-run summary display.

// --- Path Buffer Constants ---
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#define MAX_PATH_LEN MAX_PATH
#ifndef PATH_MAX
#define MAX_PATH_BUFFER 4096
#else
#define MAX_PATH_BUFFER PATH_MAX
#endif

// --- Default SDR Configurations ---

#if defined(WITH_RTLSDR)
#define RTLSDR_DEFAULT_SAMPLE_RATE 2400000.0
#endif

#if defined(WITH_SDRPLAY)
#define SDRPLAY_DEFAULT_SAMPLE_RATE_HZ 2000000.0
#define SDRPLAY_DEFAULT_BANDWIDTH_HZ   1536000.0
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
// These profiles are selected at runtime based on the sample rate to ensure
// stability and performance across the device's full range.

// Profile for sample rates < 1 MSPS (Low-Latency)
#define BLADERF_PROFILE_LOWLATENCY_NUM_BUFFERS        32
#define BLADERF_PROFILE_LOWLATENCY_BUFFER_SIZE        16384
#define BLADERF_PROFILE_LOWLATENCY_NUM_TRANSFERS      16

// Profile for sample rates between 1 MSPS and 5 MSPS (Balanced)
#define BLADERF_PROFILE_BALANCED_NUM_BUFFERS          64
#define BLADERF_PROFILE_BALANCED_BUFFER_SIZE          32768
#define BLADERF_PROFILE_BALANCED_NUM_TRANSFERS        32

// Profile for sample rates between 5 MSPS and 40 MSPS (High-Throughput)
#define BLADERF_PROFILE_HIGHTHROUGHPUT_NUM_BUFFERS    128
#define BLADERF_PROFILE_HIGHTHROUGHPUT_BUFFER_SIZE    65536
#define BLADERF_PROFILE_HIGHTHROUGHPUT_NUM_TRANSFERS  64

// Profile for sample rates > 40 MSPS on Windows (Optimized for Throughput)
#define BLADERF_PROFILE_WIN_OPTIMIZED_NUM_BUFFERS     64
#define BLADERF_PROFILE_WIN_OPTIMIZED_BUFFER_SIZE     131072
#define BLADERF_PROFILE_WIN_OPTIMIZED_NUM_TRANSFERS   64

// Parameters for calculating the Optimized profile on Linux for sample rates > 40 MSPS
#define BLADERF_PROFILE_LINUX_OPTIMIZED_BUFFER_SIZE   65536
#define BLADERF_PROFILE_LINUX_MEM_BUDGET_FACTOR       0.75  // Use 75% of available usbfs memory
#define BLADERF_PROFILE_LINUX_MAX_TRANSFERS           256   // Sanity cap on calculated transfers
#endif // defined(WITH_BLADERF)

#endif // CONFIG_H_
