/**
 * @file app_context.h
 * @brief Defines the primary application state and resource management structures.
 *
 * This header contains the definitions for the two most important structs that
 * represent the entire state of the application:
 *
 * 1.  `AppConfig`: Holds all user-configurable settings parsed from the
 *     command line and preset files. This struct is generally treated as
 *     read-only after initial setup.
 *
 * 2.  `AppResources`: Holds all allocated resources, state variables, DSP
 *     objects, and thread handles used during the application's lifecycle.
 */

#ifndef APP_CONTEXT_H_
#define APP_CONTEXT_H_

#include "common_types.h"
#include "pipeline_types.h"
#include "file_writer.h"
#include "input_source.h"
#include "memory_arena.h"
#include "presets_loader.h"
#include "constants.h"

#ifdef _WIN32
#include <liquid.h>
#else
#include <liquid/liquid.h>
#endif

// --- C99 Compatibility for C11 Atomics ---
#if __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
#include <stdatomic.h>
#else
#define _Atomic volatile
#endif

// --- Forward Declarations ---
struct FileWriteBuffer;

// --- Type Definitions ---

/**
 * @struct IqCorrectionConfig
 * @brief Configuration for the I/Q imbalance corrector.
 */
typedef struct {
    bool enable;
} IqCorrectionConfig;

/**
 * @struct DcBlockConfig
 * @brief Configuration for the DC offset blocking filter.
 */
typedef struct {
    bool enable;
} DcBlockConfig;

/**
 * @struct FilterRequest
 * @brief Holds the parameters for a single user-requested filter stage.
 */
typedef struct {
    FilterType type;
    float freq1_hz;
    float freq2_hz;
} FilterRequest;

/**
 * @struct AppConfig
 * @brief Stores all user-defined configuration settings for the application.
 */
typedef struct AppConfig {
    // --- Input & Output ---
    char*       input_type_str;
    char*       input_filename_arg;
    char*       output_filename_arg;
    char*       sample_type_name;
    char*       output_type_name;
    bool        output_type_provided;
    bool        output_to_stdout;
    char*       preset_name;

    // --- Core DSP ---
    float       gain;
    bool        gain_provided;
    float       freq_shift_hz_arg;
    bool        shift_after_resample;
    bool        no_resample;
    bool        raw_passthrough;
    float       user_defined_target_rate_arg;
    bool        user_rate_provided;
    IqCorrectionConfig iq_correction;
    DcBlockConfig      dc_block;

    // --- Internal State from CLI Parsing ---
    FilterRequest filter_requests[MAX_FILTER_CHAIN];
    int         num_filter_requests;
    bool        apply_user_filter_post_resample;

    // --- Filter Arguments ---
    float       lowpass_cutoff_hz_arg[MAX_FILTER_CHAIN];
    float       highpass_cutoff_hz_arg[MAX_FILTER_CHAIN];
    const char* pass_range_str_arg[MAX_FILTER_CHAIN];
    const char* stopband_str_arg[MAX_FILTER_CHAIN];
    float       transition_width_hz_arg;
    int         filter_taps_arg;
    float       attenuation_db_arg;
    FilterTypeRequest filter_type_request;
    const char* filter_type_str_arg;
    int         filter_fft_size_arg;

    // --- SDR-Specific Arguments ---
#if defined(ANY_SDR_SUPPORT_ENABLED)
    struct {
        double rf_freq_hz;
        float  rf_freq_hz_arg;
        bool   rf_freq_provided;
        double sample_rate_hz;
        float  sample_rate_hz_arg;
        bool   sample_rate_provided;
        bool   bias_t_enable;
    } sdr;
#endif

    // --- Resolved Final Configuration ---
    OutputType  output_type;
    format_t    output_format;
    double      target_rate;
    bool        help_requested;

    // --- Platform-Specific File Paths ---
#ifdef _WIN32
    wchar_t effective_input_filename_w[MAX_PATH_BUFFER];
    wchar_t effective_output_filename_w[MAX_PATH_BUFFER];
    char    effective_input_filename_utf8[MAX_PATH_BUFFER];
    char    effective_output_filename_utf8[MAX_PATH_BUFFER];
#else
    char*   effective_input_filename;
    char*   effective_output_filename;
#endif

    // --- Loaded Presets ---
    PresetDefinition* presets;
    int               num_presets;
} AppConfig;


// --- Resource Management Structs ---

/**
 * @typedef ProgressUpdateFn
 * @brief A function pointer type for progress update callbacks.
 */
typedef void (*ProgressUpdateFn)(unsigned long long current_output_frames, long long total_output_frames, unsigned long long current_bytes_written, void* udata);

/**
 * @struct IqCorrectionFactors
 * @brief Holds the current gain and phase correction factors for I/Q imbalance.
 */
typedef struct {
    float mag;
    float phase;
} IqCorrectionFactors;

/**
 * @struct IqCorrectionResources
 * @brief Holds all allocated objects and state for the I/Q correction module.
 */
typedef struct {
    IqCorrectionFactors factors_buffer[2];
    _Atomic int         active_buffer_idx;
    pthread_mutex_t     iq_factors_mutex;
    fftplan             fft_plan;
    complex_float_t*    fft_buffer;
    complex_float_t*    fft_shift_buffer;
    float*              spectrum_buffer;
    float*              window_coeffs;
    float               average_power;
    float               power_range;
    complex_float_t*    optimization_accum_buffer;
    int                 samples_in_accum;
    double              last_optimization_time;
} IqCorrectionResources;

/**
 * @struct DcBlockResources
 * @brief Holds the filter object for the DC blocking module.
 */
typedef struct {
    iirfilt_crcf dc_block_filter;
} DcBlockResources;

/**
 * @struct AppResources
 * @brief The master struct holding all runtime state and allocated resources for the application.
 */
typedef struct AppResources {
    const AppConfig* config;

    // --- DSP Objects ---
    msresamp_crcf   resampler;
    nco_crcf        pre_resample_nco;
    nco_crcf        post_resample_nco;
    double          nco_shift_hz;
    bool            is_passthrough;
    IqCorrectionResources iq_correction;
    DcBlockResources      dc_block;
    FilterImplementationType user_filter_type_actual;
    void*           user_fir_filter_object;
    unsigned int    user_filter_block_size;
    complex_float_t* pre_fft_remainder_buffer;
    complex_float_t* post_fft_remainder_buffer;

    // --- Input/Output State ---
    struct InputSourceOps* selected_input_ops;
    InputSourceInfo source_info;
    format_t        input_format;
    size_t          input_bytes_per_sample_pair;
    SdrMetadata     sdr_info;
    bool            sdr_info_present;
    FileWriterContext writer_ctx;
    size_t          output_bytes_per_sample_pair;
    void*           input_module_private_data;

    // --- Memory Management ---
    MemoryArena     setup_arena;
    void*           pipeline_chunk_data_pool;
    SampleChunk*    sample_chunk_pool;
    void*           sdr_deserializer_temp_buffer;
    size_t          sdr_deserializer_buffer_size;
    void*           writer_local_buffer;
    unsigned int    max_out_samples;

    // --- Threading & Pipeline ---
    pthread_t       reader_thread_handle;
    pthread_t       pre_processor_thread_handle;
    pthread_t       resampler_thread_handle;
    pthread_t       post_processor_thread_handle;
    pthread_t       writer_thread_handle;
    pthread_t       iq_optimization_thread_handle;
    PipelineMode    pipeline_mode;
    struct FileWriteBuffer* sdr_input_buffer;
    pthread_t       sdr_capture_thread_handle;
    Queue*          free_sample_chunk_queue;
    Queue*          raw_to_pre_process_queue;
    Queue*          pre_process_to_resampler_queue;
    Queue*          resampler_to_post_process_queue;
    Queue*          iq_optimization_data_queue;
    Queue*          stdout_queue;
    struct FileWriteBuffer* file_write_buffer;

    // --- Progress & State Tracking ---
    pthread_mutex_t progress_mutex;
    bool            error_occurred;
    bool            end_of_stream_reached;
    bool            threads_started;
    unsigned long long total_frames_read;
    unsigned long long total_output_frames;
    long long       final_output_size_bytes;
    long long       expected_total_output_frames;
    time_t          start_time;
    ProgressUpdateFn progress_callback;
    void*           progress_callback_udata;
} AppResources;

#endif // APP_CONTEXT_H_
