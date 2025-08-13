// include/config.h

#ifndef CONFIG_H_
#define CONFIG_H_

// --- Application Constants ---

// Resampler configuration
#define STOPBAND_ATTENUATION_DB   60.0f

// --- FIX: Set interval to 1 to update on every chunk ---
#define PROGRESS_UPDATE_INTERVAL  1     // How many chunks to process before updating the console
#define NUM_BUFFERS               128   // Maximize application pipeline depth
#define BUFFER_SIZE_SAMPLES       131072 // 2^17, larger chunks reduce overhead

// Resampling ratio limits
#define MIN_ACCEPTABLE_RATIO      0.001f
#define MAX_ACCEPTABLE_RATIO      1000.0f

// Path length constant (fallback for non-Windows)
#ifndef MAX_PATH
#define MAX_PATH 260
#endif

// Sanity check limit for frequency shift magnitude relative to sample rate
#define SHIFT_FACTOR_LIMIT 5.0

#define MAX_PATH_LEN MAX_PATH

// --- Application Naming and Configuration File ---
#define APP_NAME "iq_resample_tool"
#define PRESETS_FILENAME "iq_resample_tool_presets.conf"

// --- General Path Buffer Size ---
#ifndef PATH_MAX
#define MAX_PATH_BUFFER 4096
#else
#define MAX_PATH_BUFFER PATH_MAX
#endif

// --- I/Q Correction Configuration ---
#define IQ_CORRECTION_FFT_SIZE      1024
#define IQ_CORRECTION_DEFAULT_PERIOD 2000000

// --- DC Block Configuration ---
#define DC_BLOCK_CUTOFF_HZ          10.0f

#if defined(WITH_RTLSDR)
// --- Default RTL-SDR Configuration ---
#define RTLSDR_DEFAULT_SAMPLE_RATE 2400000.0
#endif

#if defined(WITH_SDRPLAY)
// --- Default SDRplay Configuration ---
#define SDRPLAY_DEFAULT_SAMPLE_RATE_HZ 2000000.0
#define SDRPLAY_DEFAULT_BANDWIDTH_HZ   1536000.0
#endif

#if defined(WITH_HACKRF)
// --- Default HackRF Configuration ---
#define HACKRF_DEFAULT_SAMPLE_RATE 8000000.0
#endif

#if defined(WITH_BLADERF)
// --- Default BladeRF Configuration ---
#define BLADERF_DEFAULT_SAMPLE_RATE 2000000.0
#define BLADERF_DEFAULT_BANDWIDTH 1500000.0

// --- BladeRF Stream Buffering Profiles ---
#define BLADERF_DEFAULT_NUM_BUFFERS     128
#define BLADERF_DEFAULT_BUFFER_SIZE     65536
#define BLADERF_DEFAULT_NUM_TRANSFERS   64

#define LINUX_BLADERF_USBFS_16MB_NUM_BUFFERS      64
#define LINUX_BLADERF_USBFS_16MB_BUFFER_SIZE      65536
#define LINUX_BLADERF_USBFS_16MB_NUM_TRANSFERS    32
#endif // defined(WITH_BLADERF)


#endif // CONFIG_H_
