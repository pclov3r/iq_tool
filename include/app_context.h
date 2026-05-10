/**
 * @file app_context.h
 * @brief Defines the primary application state, broken down into logical sub-contexts.
 */

#ifndef APP_CONTEXT_H_
#define APP_CONTEXT_H_

#include "common_types.h"
#include "pipeline_types.h"
#include "module.h"
#include "mem_arena.h"
#include "presets_loader.h"
#include "constants.h"
#include "resampler.h"
#include "wait_event.h"
#include <stdatomic.h>

// --- Forward Declarations ---
struct RingBuffer;

// =========================================================
// == PART 1: CONFIGURATION (Static Settings)
// =========================================================

typedef struct {
    bool enable;
} IqCorrectionConfig;

typedef struct {
    bool enable;
} DcBlockConfig;

typedef struct {
    bool       enable;
    AgcProfile profile;
    float      target_level;
    char*      profile_str_arg;
    float      target_level_arg;
} OutputAgcConfig;

typedef struct {
    FilterType type;
    float freq1_hz;
    float freq2_hz;
} FilterRequest;

/**
 * @struct AppConfig
 * @brief Stores all user-defined configuration settings.
 */
typedef struct AppConfig {
    // --- Input Configuration ---
    struct {
        char* type_name;
        char* path_arg;
    #ifdef _WIN32
        wchar_t effective_path_w[MAX_PATH_BUFFER];
        char    effective_path_utf8[MAX_PATH_BUFFER];
    #else
        char*   effective_path;
    #endif
    } input;

    // --- Output Configuration ---
    struct {
        char* module_name;
        char* path_arg;
        char* format_name;
        char* type_name;
        bool  type_provided;
        OutputType type;
        SampleFormat   format;
    #ifdef _WIN32
        wchar_t effective_path_w[MAX_PATH_BUFFER];
        char    effective_path_utf8[MAX_PATH_BUFFER];
    #else
        char*   effective_path;
    #endif
    } output;

    // --- Output Sample Rate ---
    struct {
        double target_rate;
        double user_arg;
        bool   provided;
    } output_rate;

    // --- DSP Configuration ---
    struct {
        float input_gain;
        bool  input_gain_provided;
        float output_gain;
        bool  output_gain_provided;
        float dbm_offset;
        float dbm_offset_arg;
        bool  dbm_offset_provided;
        double freq_shift_hz;
        int   shift_after_resample;
        int   raw_passthrough;

        IqCorrectionConfig iq_correction;
        DcBlockConfig      dc_block;
        OutputAgcConfig    agc;

        struct {
            FilterRequest requests[MAX_FILTER_CHAIN];
            int           count;
            bool          apply_post_resample;
            FilterTypeRequest type_req;
            struct {
                float       lowpass[MAX_FILTER_CHAIN];
                float       highpass[MAX_FILTER_CHAIN];
                const char* pass_range[MAX_FILTER_CHAIN];
                const char* stopband[MAX_FILTER_CHAIN];
                float       transition_width;
                int         taps;
                float       attenuation;
                const char* type_str;
                int         fft_size;
            } args;
        } filter;
    } dsp;

    // --- SDR General ---
    struct {
        double rf_freq_hz;
        double rf_freq_hz_arg;
        bool   rf_freq_provided;
        double frequency_offset_hz;
        double frequency_offset_arg;
        double sample_rate_hz;
        double sample_rate_hz_arg;
        bool   sample_rate_provided;
        bool   bias_t_enable;
    } sdr_general;

    // --- Global / Misc ---
    char* preset_name;
    bool  help_requested;
    PresetDefinition* presets;
    int               num_presets;
} AppConfig;


// =========================================================
// == PART 2: RUNTIME CONTEXTS (Dynamic State)
// =========================================================

// --- 1. Infrastructure Context (The Plumbing) ---
typedef struct PipelineInfrastructure {
    MemoryArena  setup_arena;
    void*        chunk_data_pool;
    struct SampleChunk** sample_chunk_pool;

    size_t       alloc_size_samples;
    size_t       read_chunk_size;
    size_t       num_chunks;
    unsigned int max_out_samples;

    size_t input_buffer_size;

    void*  sdr_deserializer_temp_buffer;
    size_t sdr_deserializer_buffer_size;

    Queue* free_sample_chunk_queue;
    Queue* reader_output_queue;
    Queue* pre_processor_input_queue;
    Queue* pre_processor_output_queue;
    Queue* resampler_input_queue;
    Queue* resampler_output_queue;
    Queue* post_processor_input_queue;
    Queue* post_processor_output_queue;
    Queue* writer_input_queue;
    Queue* iq_optimization_data_queue;

    struct RingBuffer* sdr_input_buffer;

    WaitEvent* shutdown_event;
} PipelineInfrastructure;

// --- 2. Module Context (Drivers & IO) ---
typedef struct ModuleState {
    InputModuleInterface*  input_api;
    void*                  input_private_data;
    InputSourceInfo        source_info;
    SampleFormat               input_format;
    size_t                 input_bytes_per_iq_sample;
    float                  input_dbm_offset;

    OutputModuleInterface* output_api;
    void*                  output_private_data;
    size_t                 output_bytes_per_iq_sample;
    QueueSamples           queue_samples;
    void*                  pipeline_ctx;
} ModuleState;

// --- 3. DSP Context (Math & Signal Processing) ---

struct liquid_agc_s;
struct harris_agc_s;
struct liquid_filter_s;
struct freq_shifter_s;
struct dc_blocker_s;
struct iq_state_s;

typedef struct AgcContext {
    struct liquid_agc_s* object; /* liquid-dsp agc_crcf handle. DX/LOCAL profiles only. */
    struct harris_agc_s* harris_object; /* Harris/LMS state block. DIGITAL profile only.       */
    float    current_gain;  /* Most recently applied linear gain scalar.           */
    uint64_t samples_seen;  /* Total samples processed. Used for log interval.     */
} AgcContext;

typedef struct FilterContext {
    struct liquid_filter_s* object;
    int              type_actual;
    unsigned int     block_size;
    ComplexFloat* pre_fft_remainder_buffer;
    unsigned int     pre_fft_remainder_len;
    ComplexFloat* post_fft_remainder_buffer;
    unsigned int     post_fft_remainder_len;
    ComplexFloat* fft_scratch_buffer;
} FilterContext;

typedef struct IqCorrectionResources {
    struct iq_state_s* internal_state;
    _Atomic double          last_optimization_time;
} IqCorrectionResources;

typedef struct DcBlockResources {
    struct dc_blocker_s* dc_block_filter;
} DcBlockResources;

typedef struct DspContext {
    const struct AppConfig* config; // Injected for DSP access
    IqCorrectionResources iq_correct;
    DcBlockResources      dc_block;
    AgcContext            agc;
    FilterContext         filter;

    Resampler* resampler;
    struct freq_shifter_s* pre_resample_nco;
    struct freq_shifter_s* post_resample_nco;

    float  resample_ratio;
    double nco_shift_hz;
    bool   is_passthrough;
} DspContext;

// --- 4. Runtime Context (Metrics & Telemetry) ---
typedef void (*ProgressUpdateFn)(unsigned long long current_output_frames, long long total_output_frames, unsigned long long current_bytes_written, void* udata);


typedef struct RuntimeState {

    _Atomic double last_sdr_heartbeat_time;
    atomic_bool   error_occurred;
    atomic_bool   end_of_stream_reached;

    atomic_uint_least64_t total_frames_read;
    atomic_uint_least64_t total_output_frames;
    atomic_int_least64_t final_output_size_bytes;
    atomic_int_least64_t expected_total_output_frames;
    time_t             start_time;

    ProgressUpdateFn   progress_callback;
    void*              progress_callback_udata;
} RuntimeState;

// =========================================================
// == The Main Container (formerly AppResources)
// =========================================================
typedef struct AppContext {
    const struct AppConfig* config; 

    PipelineInfrastructure pipeline;
    DspContext             dsp;
    ModuleState            module;
    RuntimeState           stats;

    PipelineMode    pipeline_mode;
    struct {
        bool reader;
        bool pre_processor;
        bool resampler;
        bool post_processor;
        bool writer;
        bool iq_optimizer;
        bool sdr_capture;
        bool sdr_watchdog;
    } threads_to_create;
} AppContext;

#endif // APP_CONTEXT_H_
