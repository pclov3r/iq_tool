#ifndef CONFIG_H_
#define CONFIG_H_

// --- Application Constants ---

// Target output sample rates for different modes
#define TARGET_RATE_CU8           1488375.0
#define TARGET_RATE_CS16_FM       744187.5
#define TARGET_RATE_CS16_AM       46511.71875

// Resampler configuration
#define STOPBAND_ATTENUATION_DB   60.0f

// Buffer sizes and processing parameters
#define BUFFER_SIZE_SAMPLES       8192 // Samples PER I/Q PAIR for input chunk processing
#define PROGRESS_UPDATE_INTERVAL  50 // Update progress message every N write loops
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

#endif // CONFIG_H_
