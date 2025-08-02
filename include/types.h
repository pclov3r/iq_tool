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

// --- Forward Declarations ---
struct InputSourceOps;
struct AppConfig;
struct AppResources;
struct WorkItem;
struct FileWriterContext;


// --- Enums ---

typedef enum {
    SAMPLE_TYPE_CU8,
    SAMPLE_TYPE_CS16,
    SAMPLE_TYPE_CS8,
    SAMPLE_TYPE_INVALID
} sample_format_t;

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
    PCM_FORMAT_UNKNOWN,
    PCM_FORMAT_S16,
    PCM_FORMAT_U16,
    PCM_FORMAT_S8,
    PCM_FORMAT_U8
} pcmformat;


// --- Structs ---

typedef float complex complex_float_t;

// +++ NEW: Definition for a single preset loaded from the config file +++
// Note that the members are char* instead of const char* because they will
// be dynamically allocated by strdup() when parsing the file.
typedef struct {
    char* name;
    char* description;
    double target_rate;
    char* sample_format_name;
    OutputType output_type;
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

#define MAX_SUMMARY_ITEMS 16

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


typedef struct AppConfig {
    char *input_type_str;
    char *input_filename_arg;
    char *output_filename_arg;
    char *sample_type_name;
    char *preset_name;
    float scale_value;
    bool scale_provided;
    bool output_to_stdout;
    double freq_shift_hz;
    bool freq_shift_requested;
    double center_frequency_target_hz;
    bool set_center_frequency_target_hz;
    bool shift_after_resample;
    bool native_8bit_path;
    bool no_resample;
    char *output_type_name;
    bool output_type_provided;
    OutputType output_type;

#if defined(WITH_SDRPLAY) || defined(WITH_HACKRF)
    struct {
        double rf_freq_hz;
        bool rf_freq_provided;
        bool bias_t_enable;
    } sdr;
#endif

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

    sample_format_t sample_format;
    double target_rate;

    double user_defined_target_rate;
    bool user_rate_provided;

#ifdef _WIN32
    wchar_t *effective_input_filename_w;
    wchar_t *effective_output_filename_w;
    char *effective_input_filename_utf8;
    char *effective_output_filename_utf8;
#else
    char *effective_input_filename;
    char *effective_output_filename;
#endif

    // +++ NEW: Fields to store the presets loaded from the config file +++
    PresetDefinition* presets;
    int num_presets;

} AppConfig;

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

typedef bool (*sample_converter_t)(struct AppConfig *config, struct AppResources *resources, struct WorkItem *item);

typedef void (*ProgressUpdateFn)(unsigned long long current_read_frames, long long total_input_frames, unsigned long long total_output_frames, void* udata);


typedef struct AppResources {
    SNDFILE *infile;
    msresamp_crcf resampler;
    nco_crcf shifter_nco;
    double actual_nco_shift_hz;
    InputSourceInfo source_info;
    int input_bit_depth;
    pcmformat input_pcm_format;
    size_t input_bytes_per_sample;
    unsigned int max_out_samples;
    size_t output_bytes_per_sample_pair;
    SdrMetadata sdr_info;
    bool sdr_info_present;
    bool is_passthrough;

    struct InputSourceOps* selected_input_ops;

    sample_converter_t converter;

    FileWriterContext writer_ctx;

#if defined(WITH_SDRPLAY)
    sdrplay_api_DeviceT *sdr_device;
    sdrplay_api_DeviceParamsT *sdr_device_params;
    bool sdr_api_is_open;
#endif
#if defined(WITH_HACKRF)
    hackrf_device* hackrf_dev;
#endif
    WorkItem* work_item_pool;
    void* raw_input_pool;
    complex_float_t* complex_scaled_pool;
    complex_float_t* complex_shifted_pool;
    complex_float_t* complex_resampled_pool;
    unsigned char* output_pool;
    pthread_t reader_thread;
    pthread_t processor_thread;
    pthread_t writer_thread;
    Queue* free_pool_q;
    Queue* input_q;
    Queue* output_q;
    pthread_mutex_t progress_mutex;
    pthread_mutex_t dsp_mutex;
    bool error_occurred;
    unsigned long long total_frames_read;
    unsigned long long total_output_frames;
    long long final_output_size_bytes;
    time_t start_time;

    ProgressUpdateFn progress_callback;
    void* progress_callback_udata;

} AppResources;

#endif // TYPES_H_
