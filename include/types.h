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

/**
 * @brief Defines all supported sample formats, inspired by convert-samples.
 */
typedef enum {
    FORMAT_UNKNOWN,
    S8,
    U8,
    S16,
    U16,
    S32,
    U32,
    F32,
    CS8,
    CU8,
    CS16,
    CU16,
    CS32,
    CU32,
    CF32
} format_t;


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


// --- Structs & Typedefs ---

/** @brief A convenient alias for a single-precision complex float. */
typedef float complex complex_float_t;

/**
 * @brief A union to hold any type of sample, inspired by convert-samples.
 */
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

// --- I/Q Correction Configuration (for AppConfig) ---
typedef struct {
    bool enable;
} IqCorrectionConfig;

// --- DC Block Configuration (for AppConfig) ---
typedef struct {
    bool enable;
} DcBlockConfig;


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
    float gain;
    bool gain_provided;
    double freq_shift_hz;
    bool freq_shift_requested;
    double center_frequency_target_hz;
    bool set_center_frequency_target_hz;
    bool shift_after_resample;
    bool no_resample;
    double user_defined_target_rate;
    bool user_rate_provided;

    // --- I/Q Correction Configuration ---
    IqCorrectionConfig iq_correction;

    // --- DC Block Configuration ---
    DcBlockConfig dc_block;

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

    // --- Raw File Input Arguments ---
    struct {
        double sample_rate_hz;
        bool sample_rate_provided;
        char *format_str;
        bool format_provided;
    } raw_file;

    // --- Resolved/Derived Configuration ---
    OutputType output_type;
    format_t output_format;
    double target_rate;
    bool help_requested;

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

/** @brief Function pointer type for the progress update callback. */
typedef void (*ProgressUpdateFn)(unsigned long long current_read_frames, long long total_input_frames, unsigned long long total_output_frames, void* udata);

// --- I/Q Correction Resources (for AppResources) ---
// This struct holds the state for the SDR#-style I/Q correction algorithm.
typedef struct {
    // --- Correction Parameters ---
    float current_mag;
    float current_phase;

    // --- FFT & Buffers ---
    fftplan fft_plan;
    complex_float_t* fft_buffer;
    complex_float_t* fft_shift_buffer;
    float*           spectrum_buffer;
    float*           window_coeffs;

    // --- Optimization State ---
    float average_power;
    float power_range;

    // --- Accumulator for Optimization ---
    complex_float_t* optimization_accum_buffer;
    int              samples_in_accum;

} IqCorrectionResources;


// --- DC Block Resources (for AppResources) ---
typedef struct {
    iirfilt_crcf dc_block_filter; // liquid-dsp IIR filter object
} DcBlockResources;


/** @brief Holds all runtime resources, state, and handles for the application. */
typedef struct AppResources {
    const struct AppConfig* config;
    // --- DSP Components ---
    msresamp_crcf resampler;
    nco_crcf shifter_nco;
    double actual_nco_shift_hz;
    bool is_passthrough; // True if --no-resample is used

    // --- I/Q Correction Resources ---
    IqCorrectionResources iq_correction;

    // --- DC Block Resources ---
    DcBlockResources dc_block;

    // --- Input Source Info & State ---
    struct InputSourceOps* selected_input_ops;
    InputSourceInfo source_info;
    format_t input_format;
    size_t input_bytes_per_sample_pair;
    SdrMetadata sdr_info;
    bool sdr_info_present;
    SNDFILE *infile; // Used for both WAV and Raw File input

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
