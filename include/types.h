#ifndef TYPES_H_
#define TYPES_H_

#include <stdint.h>
#include <stdbool.h>
#include <complex.h> // Needs to be included before liquid
#include <math.h>    // For M_PI, isfinite etc.
#include <stddef.h>  // For size_t
#include <sndfile.h> // For SNDFILE, SF_INFO
#include <time.h>    // Include for time_t used in SdrMetadata

// --- Platform Includes ---
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h> // For HANDLE, LARGE_INTEGER (needed for GetFileSizeEx)
#include <liquid.h>
#else
#include <liquid/liquid.h>
#endif

// --- Project Includes ---
#include "metadata.h"
#include <pthread.h> // For pthread types (pthread_t, pthread_mutex_t)
#include "queue.h"   // For Queue type (forward declared in queue.h)


// Ensure M_PI is defined
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef _USE_MATH_DEFINES // For MSVC
#define _USE_MATH_DEFINES
#endif

// --- Enums ---
typedef enum {
    MODE_CU8, MODE_CS16_FM, MODE_CS16_AM, MODE_INVALID
} output_mode_t;

// --- Structs ---

/**
 * @brief Holds configuration settings derived from command-line arguments.
 */
typedef struct {
    // Arguments as provided by user
    char *input_filename_arg;
    char *output_filename_arg;
    char *mode_name;
    float scale_value;
    bool scale_provided;
    bool output_to_stdout;
    double freq_shift_hz;
    bool freq_shift_requested;
    double center_frequency_target_hz;
    bool set_center_frequency_target_hz;
    bool shift_after_resample;

    // Processed configuration
    output_mode_t mode;
    double target_rate;

    // Effective paths (resolved, used by processing functions)
#ifdef _WIN32
    wchar_t *effective_input_filename_w;
    wchar_t *effective_output_filename_w;
    char *effective_input_filename_utf8;
    char *effective_output_filename_utf8;
#else
    char *effective_input_filename;  // Points to input_filename_arg on POSIX
    char *effective_output_filename; // Points to output_filename_arg or NULL on POSIX
#endif
} AppConfig;


/**
 * @brief Represents a chunk of work passing through the processing pipeline.
 */
typedef struct {
    // Pointers to buffer segments within the main pools
    void* raw_input_buffer;
    liquid_float_complex* complex_buffer_scaled;
    liquid_float_complex* complex_buffer_shifted;
    liquid_float_complex* complex_buffer_resampled;
    unsigned char* output_buffer;

    // State for this specific chunk
    sf_count_t frames_read;
    unsigned int frames_to_write;
    unsigned long long sequence_number;
    bool is_last_chunk;
} WorkItem;


/**
 * @brief Holds shared resources, handles, and state for the application pipeline.
 */
typedef struct {
    // --- Input/Output Handles (Single Thread Access) ---
    SNDFILE *infile;
    FILE *outfile;
#ifdef _WIN32
    HANDLE h_outfile;
#endif

    // --- DSP Objects (Single Thread Access) ---
    msresamp_crcf resampler;
    nco_crcf shifter_nco;
    double actual_nco_shift_hz;

    // --- Input File Info ---
    SF_INFO sfinfo;
    int input_bit_depth;
    int input_format_subtype;
    size_t input_bytes_per_sample;

    // --- Buffer Sizing Info ---
    unsigned int max_out_samples;
    size_t output_bytes_per_sample_pair;

    // --- Metadata ---
    SdrMetadata sdr_info;
    bool sdr_info_present;

    // --- Buffer Pools (Allocated once) ---
    WorkItem* work_item_pool;
    void* raw_input_pool;
    liquid_float_complex* complex_scaled_pool;
    liquid_float_complex* complex_shifted_pool;
    liquid_float_complex* complex_resampled_pool;
    unsigned char* output_pool;

    // --- Queues (Thread-safe communication) ---
    Queue* free_pool_q;
    Queue* input_q;
    Queue* output_q;

    // --- Threading & Synchronization ---
    pthread_t reader_thread;
    pthread_t processor_thread;
    pthread_t writer_thread;

    pthread_mutex_t progress_mutex;
    bool error_occurred;
    unsigned long long total_frames_read;
    unsigned long long total_output_frames;

    // --- Final Result Info ---
    long long final_output_size_bytes;

} AppResources;


#endif // TYPES_H_
