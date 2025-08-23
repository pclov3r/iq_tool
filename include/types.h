#ifndef TYPES_H_
#define TYPES_H_

#include <stdint.h>
#include <stdbool.h>
#include <complex.h>
#include <stddef.h>
#include <pthread.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <liquid.h>
#else
#include <liquid/liquid.h>
#endif

#if __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
#include <stdatomic.h>
#else
#define _Atomic volatile
#endif

// --- Local Project Includes ---
#include "constants.h"
#include "file_write_buffer.h"
#include "sdr_packet_serializer.h"

// --- Forward Declarations ---
struct InputSourceOps;
struct AppConfig;
struct SampleChunk;
struct FileWriterContext;
struct AppResources;

// --- Centralized Core Type Definitions ---

/**
 * @struct MemoryArena
 * @brief Manages a single large block of memory for fast, contiguous setup-time allocations.
 */
typedef struct {
    void* memory;
    size_t capacity;
    size_t offset;
} MemoryArena;

/**
 * @struct ThreadSafeQueue
 * @brief A standard, blocking, thread-safe queue implementation.
 */
struct ThreadSafeQueue {
    void** buffer;
    size_t capacity;
    size_t count;
    size_t head;
    size_t tail;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty_cond;
    pthread_cond_t not_full_cond;
    bool shutting_down;
};

// Typedef for the queue struct.
typedef struct ThreadSafeQueue Queue;


// --- Enum and Type Definitions ---

typedef enum {
    FORMAT_UNKNOWN, S8, U8, S16, U16, S32, U32, F32,
    CS8, CU8, CS16, CU16, CS32, CU32, CF32, SC16Q11
} format_t;

typedef enum {
    OUTPUT_TYPE_RAW,
    OUTPUT_TYPE_WAV,
    OUTPUT_TYPE_WAV_RF64
} OutputType;

typedef enum {
    SDR_SOFTWARE_UNKNOWN,
    SDR_CONSOLE,
    SDR_SHARP,
    SDR_UNO,
    SDR_CONNECT,
} SdrSoftwareType;

typedef enum {
    FILTER_TYPE_NONE,
    FILTER_TYPE_LOWPASS,
    FILTER_TYPE_HIGHPASS,
    FILTER_TYPE_PASSBAND,
    FILTER_TYPE_STOPBAND
} FilterType;

typedef enum {
    FILTER_IMPL_NONE,
    FILTER_IMPL_FIR_SYMMETRIC,
    FILTER_IMPL_FIR_ASYMMETRIC,
    FILTER_IMPL_FFT_SYMMETRIC,
    FILTER_IMPL_FFT_ASYMMETRIC
} FilterImplementationType;

typedef enum {
    FILTER_TYPE_AUTO, // Value 0: Default, unset state.
    FILTER_TYPE_FIR,  // Value 1
    FILTER_TYPE_FFT   // Value 2
} FilterTypeRequest;


typedef enum {
    PIPELINE_MODE_REALTIME_SDR,
    PIPELINE_MODE_BUFFERED_SDR,
    PIPELINE_MODE_FILE_PROCESSING
} PipelineMode;


typedef float complex complex_float_t;

typedef union {
    char s8;
    unsigned char u8;
    short int s16;
    unsigned short int u16;
    int s32;
    unsigned int u32;
    float f32;
    char cs8[2];
    unsigned char cu8[2];
    short int cs16[2];
    unsigned short int cu16[2];
    int cs32[2];
    unsigned int cu32[2];
    float complex cf32;
} sample_t;

// --- Struct Definitions ---

// Definitions for the preset parser dispatch table
typedef enum {
    PRESET_KEY_STRDUP,
    PRESET_KEY_STRTOD,
    PRESET_KEY_STRTOF,
    PRESET_KEY_STRTOL,
    PRESET_KEY_BOOL,
    PRESET_KEY_OUTPUT_TYPE
} PresetKeyAction;

typedef struct {
    const char* key_name;
    PresetKeyAction action;
    size_t value_offset;
    size_t provided_flag_offset;
} PresetKeyHandler;

typedef struct {
    char* name;
    char* description;
    double target_rate;
    char* sample_format_name;
    OutputType output_type;
    float gain;
    bool gain_provided;
    bool dc_block_enable;
    bool dc_block_provided;
    bool iq_correction_enable;
    bool iq_correction_provided;

    // Filter Fields
    float lowpass_cutoff_hz;
    bool  lowpass_cutoff_hz_provided;
    float highpass_cutoff_hz;
    bool  highpass_cutoff_hz_provided;
    char* pass_range_str;
    bool  pass_range_str_provided;
    char* stopband_str;
    bool  stopband_str_provided;
    float transition_width_hz;
    bool  transition_width_hz_provided;
    int   filter_taps;
    bool  filter_taps_provided;
    float attenuation_db;
    bool  attenuation_db_provided;
    char* filter_type_str;
    bool  filter_type_str_provided;

} PresetDefinition;

typedef struct {
    int64_t frames;
    int samplerate;
} InputSourceInfo;

typedef struct {
    SdrSoftwareType source_software;
    char software_name[64];
    char software_version[64];
    char radio_model[128];
    bool software_name_present;
    bool software_version_present;
    bool radio_model_present;
    double center_freq_hz;
    bool center_freq_hz_present;
    time_t timestamp_unix;
    char timestamp_str[64];
    bool timestamp_unix_present;
    bool timestamp_str_present;
} SdrMetadata;

typedef struct {
    char label[64];
    char value[128];
} SummaryItem;

typedef struct {
    SummaryItem items[MAX_SUMMARY_ITEMS];
    int count;
} InputSummaryInfo;

typedef struct {
    bool (*open)(struct FileWriterContext* ctx, const struct AppConfig* config, struct AppResources* resources);
    size_t (*write)(struct FileWriterContext* ctx, const void* buffer, size_t bytes_to_write);
    void (*close)(struct FileWriterContext* ctx);
    long long (*get_total_bytes_written)(const struct FileWriterContext* ctx);
} FileWriterOps;

typedef struct FileWriterContext {
    void* private_data;
    FileWriterOps ops;
    long long total_bytes_written;
} FileWriterContext;

typedef struct {
    bool enable;
} IqCorrectionConfig;

typedef struct {
    bool enable;
} DcBlockConfig;

typedef enum {
    FREQUENCY_SHIFT_REQUEST_NONE,
    FREQUENCY_SHIFT_REQUEST_MANUAL,
    FREQUENCY_SHIFT_REQUEST_METADATA_CALC_TARGET
} FrequencyShiftRequestType;

typedef struct {
    FrequencyShiftRequestType type;
    double value;
} FrequencyShiftRequest;

typedef struct {
    FilterType type;
    float freq1_hz;
    float freq2_hz;
} FilterRequest;

typedef struct AppConfig {
    char *input_type_str;
    char *input_filename_arg;
    char *output_filename_arg;
    char *sample_type_name;
    char *output_type_name;
    bool output_type_provided;
    bool output_to_stdout;
    char *preset_name;
    float gain;
    bool gain_provided;
    float freq_shift_hz_arg;
    float wav_center_target_hz_arg;
    bool shift_after_resample;
    bool no_resample;
    bool raw_passthrough;
    float user_defined_target_rate_arg;
    bool user_rate_provided;
    IqCorrectionConfig iq_correction;
    DcBlockConfig dc_block;

    FrequencyShiftRequest frequency_shift_request;

    double freq_shift_hz;
    bool freq_shift_requested;

    double center_frequency_target_hz;
    bool set_center_frequency_target_hz;
    
    FilterRequest filter_requests[MAX_FILTER_CHAIN];
    int num_filter_requests;

    bool apply_user_filter_post_resample;

    // Filter arguments
    float lowpass_cutoff_hz_arg[MAX_FILTER_CHAIN];
    float highpass_cutoff_hz_arg[MAX_FILTER_CHAIN];
    const char* pass_range_str_arg[MAX_FILTER_CHAIN];
    const char* stopband_str_arg[MAX_FILTER_CHAIN];
    float transition_width_hz_arg;
    int filter_taps_arg;
    float attenuation_db_arg;

    FilterTypeRequest filter_type_request;
    const char* filter_type_str_arg;
    int filter_fft_size_arg;

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

    OutputType output_type;
    format_t output_format;
    double target_rate;
    bool help_requested;

    // Replaced allocated pointers with fixed-size buffers for Windows
    // to eliminate heap allocation during path resolution.
#ifdef _WIN32
    wchar_t effective_input_filename_w[MAX_PATH_BUFFER];
    wchar_t effective_output_filename_w[MAX_PATH_BUFFER];
    char effective_input_filename_utf8[MAX_PATH_BUFFER];
    char effective_output_filename_utf8[MAX_PATH_BUFFER];
#else
    char *effective_input_filename;
    char *effective_output_filename;
#endif

    PresetDefinition* presets;
    int num_presets;
} AppConfig;

typedef struct SampleChunk {
    // --- Buffers ---
    void* raw_input_data;
    complex_float_t* complex_pre_resample_data;
    complex_float_t* complex_resampled_data;
    complex_float_t* complex_post_resample_data;
    complex_float_t* complex_scratch_data;
    unsigned char* final_output_data;

    // --- Capacities ---
    size_t raw_input_capacity_bytes;
    size_t complex_buffer_capacity_samples;
    size_t final_output_capacity_bytes;

    // --- State Variables ---
    int64_t frames_read;
    unsigned int frames_to_write;
    bool is_last_chunk;
    bool stream_discontinuity_event;
    size_t input_bytes_per_sample_pair;
} SampleChunk;

typedef void (*ProgressUpdateFn)(unsigned long long current_output_frames, long long total_output_frames, unsigned long long current_bytes_written, void* udata);

typedef struct {
    float mag;
    float phase;
} IqCorrectionFactors;

typedef struct {
    IqCorrectionFactors factors_buffer[2];
    _Atomic int active_buffer_idx;
    fftplan fft_plan;
    complex_float_t* fft_buffer;
    complex_float_t* fft_shift_buffer;
    float* spectrum_buffer;
    float* window_coeffs;
    float average_power;
    float power_range;
    complex_float_t* optimization_accum_buffer;
    int samples_in_accum;
} IqCorrectionResources;

typedef struct {
    iirfilt_crcf dc_block_filter;
} DcBlockResources;

typedef struct AppResources {
    const struct AppConfig* config;
    msresamp_crcf resampler;
    nco_crcf pre_resample_nco;
    nco_crcf post_resample_nco;
    double actual_nco_shift_hz;
    bool is_passthrough;
    IqCorrectionResources iq_correction;
    DcBlockResources dc_block;
    struct InputSourceOps* selected_input_ops;
    InputSourceInfo source_info;
    format_t input_format;
    size_t input_bytes_per_sample_pair;
    SdrMetadata sdr_info;
    bool sdr_info_present;
    FileWriterContext writer_ctx;
    size_t output_bytes_per_sample_pair;

    FilterImplementationType user_filter_type_actual;
    void* user_fir_filter_object;
    unsigned int user_filter_block_size;

    void* input_module_private_data;

    // Memory Arena for all setup-time allocations
    MemoryArena setup_arena;

    // Consolidated pipeline buffer pool for cache locality
    void* pipeline_chunk_data_pool;
    SampleChunk* sample_chunk_pool;

    // Pre-allocated buffer for real-time deserializer
    void* sdr_deserializer_temp_buffer;
    size_t sdr_deserializer_buffer_size;

    // Pre-allocated buffer for the writer thread
    void* writer_local_buffer;

    unsigned int max_out_samples;

    pthread_t reader_thread_handle;
    pthread_t pre_processor_thread_handle;
    pthread_t resampler_thread_handle;
    pthread_t post_processor_thread_handle;
    pthread_t writer_thread_handle;
    pthread_t iq_optimization_thread_handle;

    PipelineMode pipeline_mode;
    FileWriteBuffer* sdr_input_buffer;
    pthread_t sdr_capture_thread_handle;


    Queue* free_sample_chunk_queue;
    Queue* raw_to_pre_process_queue;
    Queue* pre_process_to_resampler_queue;
    Queue* resampler_to_post_process_queue;
    Queue* iq_optimization_data_queue;
    Queue* stdout_queue;

    FileWriteBuffer* file_write_buffer;

    pthread_mutex_t progress_mutex;
    bool error_occurred;
    bool end_of_stream_reached;
    bool threads_started;

    _Atomic time_t last_overrun_log_time;
    _Atomic unsigned long long sdr_chunks_dropped_since_last_log;

    unsigned long long total_frames_read;
    unsigned long long total_output_frames;
    long long final_output_size_bytes;
    long long expected_total_output_frames;
    time_t start_time;

    ProgressUpdateFn progress_callback;
    void* progress_callback_udata;
} AppResources;

#endif // TYPES_H_
