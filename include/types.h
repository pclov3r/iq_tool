// types.h

#ifndef TYPES_H_
#define TYPES_H_

#include <stdint.h>
#include <stdbool.h>
#include <complex.h>
#include <stddef.h>
#include <sndfile.h>
#include <pthread.h>
#include <time.h>

// --- Platform & Library Specific Includes ---
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <liquid.h>
#else
#include <liquid/liquid.h>
#endif

#include "config.h"
#include "queue.h"

#if defined(WITH_SDRPLAY)
#include "sdrplay_api.h"
#endif

#if defined(WITH_HACKRF)
#include <hackrf.h>
#endif

// --- Forward Declarations to resolve circular dependencies ---
struct InputSourceOps;
struct AppConfig;
struct AppResources;
struct WorkItem;
struct FileWriterContext;


// --- Enums ---

/** @brief Defines the output sample format type. */
typedef enum {
    SAMPLE_TYPE_CU8,  // Unsigned 8-bit complex
    SAMPLE_TYPE_CS16, // Signed 16-bit complex
    SAMPLE_TYPE_CS8,  // Signed 8-bit complex
    SAMPLE_TYPE_INVALID
} sample_format_t;

/** @brief Defines the output container type. */
typedef enum {
    OUTPUT_TYPE_RAW,
    OUTPUT_TYPE_WAV,
    OUTPUT_TYPE_WAV_RF64
} OutputType;

/** @brief Identifies the software that created a WAV file from metadata. */
typedef enum {
    SDR_SOFTWARE_UNKNOWN,
    SDR_CONSOLE,
    SDR_SHARP,
    SDR_UNO,
    SDR_CONNECT,
} SdrSoftwareType;

/** @brief Defines the internal representation of the input PCM format. */
typedef enum {
    PCM_FORMAT_UNKNOWN,
    PCM_FORMAT_S16, // Signed 16-bit
    PCM_FORMAT_U16, // Unsigned 16-bit
    PCM_FORMAT_S8,  // Signed 8-bit
    PCM_FORMAT_U8   // Unsigned 8-bit
} pcmformat;


// --- Structs & Typedefs ---

/** @brief A convenient alias for a single-precision complex float. */
typedef float complex complex_float_t;

/** @brief Represents a single preset loaded from the configuration file. */
typedef struct {
    char* name;
    char* description;
    double target_rate;
    char* sample_format_name;
    OutputType output_type;
} PresetDefinition;

/** @brief Stores basic information about the input source. */
typedef struct {
    int64_t frames;
    int samplerate;
} InputSourceInfo;

/** @brief Stores metadata parsed from WAV file chunks or filenames. */
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

#define MAX_SUMMARY_ITEMS 16

/** @brief A single key-value pair for the configuration summary. */
typedef struct {
    char label[64];
    char value[128];
} SummaryItem;

/** @brief A collection of summary items for display. */
typedef struct {
    SummaryItem items[MAX_SUMMARY_ITEMS];
    int count;
} InputSummaryInfo;

/** @brief Defines the function pointer interface for file writer operations. */
typedef struct {
    bool (*open)(struct FileWriterContext* ctx, const struct AppConfig* config, struct AppResources* resources);
    size_t (*write)(struct FileWriterContext* ctx, const void* buffer, size_t bytes_to_write);
    void (*close)(struct FileWriterContext* ctx);
    long long (*get_total_bytes_written)(const struct FileWriterContext* ctx);
} FileWriterOps;

/** @brief The context for a file writer, holding its state and operations. */
typedef struct FileWriterContext {
    void* private_data;
    FileWriterOps ops;
    long long total_bytes_written;
} FileWriterContext;

/** @brief Holds all application configuration settings parsed from the command line. */
typedef struct AppConfig {
    // --- Input & Output Arguments ---
    char *input_type_str;
    char *input_filename_arg;
    char *output_filename_arg;
    char *sample_type_name;
    char *output_type_name;
    bool output_type_provided;
    bool output_to_stdout;

    // --- Processing Arguments ---
    char *preset_name;
    float scale_value;
    bool scale_provided;
    double freq_shift_hz;
    bool freq_shift_requested;
    double center_frequency_target_hz;
    bool set_center_frequency_target_hz;
    bool shift_after_resample;
    bool native_8bit_path;
    bool no_resample;
    double user_defined_target_rate;
    bool user_rate_provided;

    // --- SDR General Arguments ---
#if defined(WITH_SDRPLAY) || defined(WITH_HACKRF)
    struct {
        double rf_freq_hz;
        bool rf_freq_provided;
        bool bias_t_enable;
    } sdr;
#endif

    // --- SDRplay-Specific Arguments ---
#if defined(WITH_SDRPLAY)
    struct {
        bool use_sdrplay_input;
        int device_index;
        int gain_level;
        bool gain_level_provided;
        sdrplay_api_RspDx_HdrModeBwT hdr_bw_mode;
        bool hdr_bw_mode_provided;
        bool use_hdr_mode;
        char *antenna_port_name;
        double sample_rate_hz;
        bool sample_rate_provided;
        double bandwidth_hz;
        bool bandwidth_provided;
    } sdrplay;
#endif

    // --- HackRF-Specific Arguments ---
#if defined(WITH_HACKRF)
    struct {
        bool use_hackrf_input;
        uint32_t lna_gain;
        bool lna_gain_provided;
        uint32_t vga_gain;
        bool vga_gain_provided;
        bool amp_enable;
        double sample_rate_hz;
        bool sample_rate_provided;
    } hackrf;
#endif

    // --- Resolved/Derived Configuration ---
    OutputType output_type;
    sample_format_t sample_format;
    double target_rate;
    bool help_requested; // FIX: Flag to signal a clean exit for --help

    // --- Effective Paths (Platform-Specific) ---
#ifdef _WIN32
    wchar_t *effective_input_filename_w;
    wchar_t *effective_output_filename_w;
    char *effective_input_filename_utf8;
    char *effective_output_filename_utf8;
#else
    char *effective_input_filename;
    char *effective_output_filename;
#endif

    // --- Loaded Presets ---
    PresetDefinition* presets;
    int num_presets;

} AppConfig;

/** @brief Represents a single chunk of work passing through the pipeline. */
typedef struct WorkItem {
    void* raw_input_buffer;
    complex_float_t* complex_buffer_scaled;
    complex_float_t* complex_buffer_shifted;
    complex_float_t* complex_buffer_resampled;
    unsigned char* output_buffer;
    int64_t frames_read;
    unsigned int frames_to_write;
    bool is_last_chunk;
} WorkItem;

/** @brief Function pointer type for sample conversion functions. */
typedef bool (*sample_converter_t)(struct AppConfig *config, struct AppResources *resources, struct WorkItem *item);

/** @brief Function pointer type for the progress update callback. */
typedef void (*ProgressUpdateFn)(unsigned long long current_read_frames, long long total_input_frames, unsigned long long total_output_frames, void* udata);

/** @brief Holds all runtime resources, state, and handles for the application. */
typedef struct AppResources {
    // --- DSP Components ---
    msresamp_crcf resampler;
    nco_crcf shifter_nco;
    double actual_nco_shift_hz;
    bool is_passthrough; // True if --no-resample is used

    // --- Input Source Info & State ---
    struct InputSourceOps* selected_input_ops;
    InputSourceInfo source_info;
    int input_bit_depth;
    pcmformat input_pcm_format;
    size_t input_bytes_per_sample;
    SdrMetadata sdr_info;
    bool sdr_info_present;
    sample_converter_t converter;
    SNDFILE *infile; // Specific to WAV input

    // --- Output State ---
    FileWriterContext writer_ctx;
    size_t output_bytes_per_sample_pair;

    // --- SDR Device Handles ---
#if defined(WITH_SDRPLAY)
    sdrplay_api_DeviceT *sdr_device;
    sdrplay_api_DeviceParamsT *sdr_device_params;
    bool sdr_api_is_open;
#endif
#if defined(WITH_HACKRF)
    hackrf_device* hackrf_dev;
#endif

    // --- Buffer Pools ---
    WorkItem* work_item_pool;
    void* raw_input_pool;
    complex_float_t* complex_scaled_pool;
    complex_float_t* complex_shifted_pool;
    complex_float_t* complex_resampled_pool;
    unsigned char* output_pool;
    unsigned int max_out_samples;

    // --- Threading & Synchronization ---
    pthread_t reader_thread;
    pthread_t processor_thread;
    pthread_t writer_thread;
    Queue* free_pool_q;
    Queue* input_q;
    Queue* output_q;
    pthread_mutex_t progress_mutex;
    pthread_mutex_t dsp_mutex;
    bool error_occurred;

    // --- Progress Tracking & Statistics ---
    unsigned long long total_frames_read;
    unsigned long long total_output_frames;
    long long final_output_size_bytes;
    time_t start_time;
    ProgressUpdateFn progress_callback;
    void* progress_callback_udata;

} AppResources;

#endif // TYPES_H_
