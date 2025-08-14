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

#if __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
#include <stdatomic.h>
#else
#define _Atomic volatile
#endif

#include "config.h"
#include "queue.h"

#if defined(WITH_SDRPLAY)
#include "sdrplay_api.h"
#endif

#if defined(WITH_HACKRF)
#include <hackrf.h>
#endif

#if defined(WITH_BLADERF)
#include <libbladeRF.h>
#endif

#if defined(WITH_RTLSDR)
#include <rtl-sdr.h>
#endif

struct InputSourceOps;
struct AppConfig;
struct SampleChunk;
struct FileWriterContext;

// --- FIX: Forward-declare AppResources to break the circular dependency ---
struct AppResources;

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
    // --- FIX: Reverted this signature to its original, correct form ---
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
    double freq_shift_hz;
    float freq_shift_hz_arg;
    bool freq_shift_requested;
    double center_frequency_target_hz;
    float wav_center_target_hz_arg;
    bool set_center_frequency_target_hz;
    bool shift_after_resample;
    bool no_resample;
    bool raw_passthrough;
    float user_defined_target_rate_arg;
    bool user_rate_provided;
    IqCorrectionConfig iq_correction;
    DcBlockConfig dc_block;

#if defined(ANY_SDR_SUPPORT_ENABLED)
    struct {
        double rf_freq_hz;
        float sdr_rf_freq_hz_arg;
        bool rf_freq_provided;
        bool bias_t_enable;
    } sdr;
#endif

#if defined(WITH_RTLSDR)
    struct {
        int device_index;
        double sample_rate_hz;
        float rtlsdr_sample_rate_hz_arg;
        bool sample_rate_provided;
        int gain;
        bool gain_provided;
        float rtlsdr_gain_db_arg;
        int ppm;
        bool ppm_provided;
        int direct_sampling_mode;
        bool direct_sampling_provided;
    } rtlsdr;
#endif

#if defined(WITH_SDRPLAY)
    struct {
        int device_index;
        int gain_level;
        bool gain_level_provided;
        sdrplay_api_RspDx_HdrModeBwT hdr_bw_mode;
        bool hdr_bw_mode_provided;
        float sdrplay_hdr_bw_hz_arg;
        bool use_hdr_mode;
        char *antenna_port_name;
        double sample_rate_hz;
        float sdrplay_sample_rate_hz_arg;
        bool sample_rate_provided;
        double bandwidth_hz;
        float sdrplay_bandwidth_hz_arg;
        bool bandwidth_provided;
    } sdrplay;
#endif

#if defined(WITH_HACKRF)
    struct {
        uint32_t lna_gain;
        bool lna_gain_provided;
        long hackrf_lna_gain_arg;
        uint32_t vga_gain;
        bool vga_gain_provided;
        long hackrf_vga_gain_arg;
        bool amp_enable;
        double sample_rate_hz;
        float hackrf_sample_rate_hz_arg;
        bool sample_rate_provided;
    } hackrf;
#endif

#if defined(WITH_BLADERF)
    struct {
        int device_index;
        int channel;
        int gain;
        bool gain_provided;
        long bladerf_gain_arg;
        uint32_t sample_rate_hz;
        bool sample_rate_provided;
        float bladerf_sample_rate_hz_arg;
        uint32_t bandwidth_hz;
        bool bandwidth_provided;
        float bladerf_bandwidth_hz_arg;
        char *fpga_file_path;
        unsigned int num_buffers;
        unsigned int buffer_size;
        unsigned int num_transfers;
    } bladerf;
#endif

    struct {
        double sample_rate_hz;
        float raw_file_sample_rate_hz_arg;
        bool sample_rate_provided;
        char *format_str;
        bool format_provided;
    } raw_file;

    OutputType output_type;
    format_t output_format;
    double target_rate;
    bool help_requested;

#ifdef _WIN32
    wchar_t *effective_input_filename_w;
    wchar_t *effective_output_filename_w;
    char *effective_input_filename_utf8;
    char *effective_output_filename_utf8;
#else
    char *effective_input_filename;
    char *effective_output_filename;
#endif

    PresetDefinition* presets;
    int num_presets;
} AppConfig;

typedef struct SampleChunk {
    void* raw_input_data;
    complex_float_t* complex_pre_resample_data;
    complex_float_t* complex_resampled_data;
    complex_float_t* complex_post_resample_data;
    unsigned char* final_output_data;
    int64_t frames_read;
    unsigned int frames_to_write;
    bool is_last_chunk;
    bool stream_discontinuity_event;
} SampleChunk;

typedef void (*ProgressUpdateFn)(unsigned long long current_output_frames, long long total_output_frames, unsigned long long unused_arg, void* udata);

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
    SNDFILE *infile;
    FileWriterContext writer_ctx;
    size_t output_bytes_per_sample_pair;

#if defined(WITH_RTLSDR)
    rtlsdr_dev_t *rtlsdr_dev;
    char rtlsdr_manufact[256];
    char rtlsdr_product[256];
    char rtlsdr_serial[256];
#endif

#if defined(WITH_SDRPLAY)
    sdrplay_api_DeviceT *sdr_device;
    sdrplay_api_DeviceParamsT *sdr_device_params;
    bool sdr_api_is_open;
#endif

#if defined(WITH_HACKRF)
    hackrf_device* hackrf_dev;
#endif

#if defined(WITH_BLADERF)
    struct bladerf *bladerf_dev;
    char bladerf_board_name[16];
    char bladerf_serial[33];
    char bladerf_display_name[128];
    bool bladerf_initialized_successfully;
#endif

    SampleChunk* sample_chunk_pool;
    void* raw_input_data_pool;
    complex_float_t* complex_pre_resample_data_pool;
    complex_float_t* complex_post_resample_data_pool;
    complex_float_t* complex_resampled_data_pool;
    unsigned char* final_output_data_pool;
    unsigned int max_out_samples;

    pthread_t reader_thread_handle;
    pthread_t pre_processor_thread_handle;
    pthread_t resampler_thread_handle;
    pthread_t post_processor_thread_handle;
    pthread_t writer_thread_handle;
    pthread_t iq_optimization_thread_handle;

    Queue* free_sample_chunk_queue;
    Queue* raw_to_pre_process_queue;
    Queue* pre_process_to_resampler_queue;
    Queue* resampler_to_post_process_queue;
    Queue* final_output_queue;
    Queue* iq_optimization_data_queue;

    pthread_mutex_t progress_mutex;
    bool error_occurred;
    bool end_of_stream_reached;
    bool threads_started;

    unsigned long long total_frames_read;
    unsigned long long total_output_frames;
    long long final_output_size_bytes;
    long long expected_total_output_frames;
    time_t start_time;

    ProgressUpdateFn progress_callback;
    void* progress_callback_udata;
} AppResources;

#endif // TYPES_H_
