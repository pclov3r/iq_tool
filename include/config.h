#ifndef CONFIG_H_
#define CONFIG_H_

// --- Application Constants ---

// Resampler configuration
#define STOPBAND_ATTENUATION_DB   60.0f

// Buffer sizes and processing parameters
#define BUFFER_SIZE_SAMPLES       8192 // Samples PER I/Q PAIR for input chunk processing
#define PROGRESS_UPDATE_INTERVAL  50   // Update progress message every N write loops
#define NUM_BUFFERS               4    // Number of WorkItem buffers for the pipeline (Min: Stages+1=4)

// Default scaling factors if not provided by user
#define DEFAULT_SCALE_FACTOR_CS16 0.50f
#define DEFAULT_SCALE_FACTOR_CU8  0.02f

// Scaling factor for converting 8-bit input to internal float range
#define SCALE_8_TO_16             256.0f

// Resampling ratio limits
#define MIN_ACCEPTABLE_RATIO      0.001f
#define MAX_ACCEPTABLE_RATIO      1000.0f

// Path length constant (fallback for non-Windows)
#ifndef MAX_PATH // Define fallback for non-Windows or older SDKs
#define MAX_PATH 260
#endif

// Sanity check limit for frequency shift magnitude relative to sample rate
#define SHIFT_FACTOR_LIMIT 5.0

#define MAX_PATH_LEN MAX_PATH // Use consistent naming

// --- Application Naming and Configuration File ---
#define APP_NAME "iq_resample_tool"
#define PRESETS_FILENAME "iq_resample_tool_presets.conf"

// --- General Path Buffer Size ---
// Use a sufficiently large buffer for paths, considering POSIX PATH_MAX can be 4096
#ifndef PATH_MAX
#define MAX_PATH_BUFFER 4096 // Fallback for systems not defining PATH_MAX or for longer paths
#else
#define MAX_PATH_BUFFER PATH_MAX // Use system PATH_MAX if defined
#endif

// --- I/Q Correction Configuration ---
#define IQ_CORRECTION_FFT_SIZE      1024
#define IQ_CORRECTION_FFT_COUNT     4
#define IQ_CORRECTION_MAX_ITER      25
// The number of samples to process between running the optimization algorithm.
// A value of 2,000,000 corresponds to once per second at a 2 Msps sample rate.
#define IQ_CORRECTION_DEFAULT_PERIOD 2000000

// --- DC Block Configuration ---
#define DC_BLOCK_FILTER_ORDER       4
#define DC_BLOCK_CUTOFF_HZ          10.0f

#if defined(WITH_SDRPLAY)
// --- Default SDRplay Configuration ---
#define SDRPLAY_DEFAULT_SAMPLE_RATE_HZ 2000000.0
#define SDRPLAY_DEFAULT_BANDWIDTH_HZ   1536000.0
#endif

#if defined(WITH_HACKRF)
// --- Default HackRF Configuration ---
#define HACKRF_DEFAULT_SAMPLE_RATE 8000000.0
#endif


#endif // CONFIG_H_
