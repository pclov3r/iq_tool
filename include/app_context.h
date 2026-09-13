/**
 * @file app_context.h
 * @brief Defines the primary application state, broken down into logical
 * sub-contexts.
 */

#ifndef APP_CONTEXT_H_
#define APP_CONTEXT_H_

#include "common_types.h"
#include "config/constants.h"
#include "config/process_chain_default.h"
#include "mem_arena.h"
#include "module.h"
#include "presets_loader.h"
#include "process_chain_types.h"
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

typedef struct OutputAgcConfig {
  bool enable;
  float target_level;
  float target_level_arg;
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
    char *type_name;
    char *path_arg;
#ifdef _WIN32
    wchar_t effective_path_w[APP_MAX_PATH_BUFFER];
    char effective_path_utf8[APP_MAX_PATH_BUFFER];
#else
    char *effective_path;
#endif
  } input;

  // --- Output Configuration ---
  struct {
    char *module_name;
    char *path_arg;
    char *sample_format_str;
    bool format_provided;
    OutputPayload payload;
    SampleFormat sample_format;
#ifdef _WIN32
    wchar_t effective_path_w[APP_MAX_PATH_BUFFER];
    char effective_path_utf8[APP_MAX_PATH_BUFFER];
#else
    char *effective_path;
#endif
  } output;

  // --- Output Sample Rate ---
  struct {
    double rate_hz;
    double user_arg;
    bool provided;
  } output_sample_rate;

  // --- Baseband Sample Rate (for Demodulators) ---
  struct {
    double rate_hz;
    double user_arg;
    bool provided;
  } baseband_sample_rate;

  // --- Baseband Sample Format (for Demodulators) ---
  struct {
    char *format_str;
    SampleFormat format;
    bool provided;
  } baseband_sample_format;

  // --- Audio Configuration ---
  struct {
    char *path_arg; // Raw command line argument
#ifdef _WIN32
    char effective_path_utf8[APP_MAX_PATH_BUFFER];
    wchar_t effective_path_w[APP_MAX_PATH_BUFFER];
#else
    char *effective_path;
#endif
    bool writer_rf64;
    bool mute;
  } audio;

  // --- DSP Configuration ---
  struct {
    float input_gain;
    bool input_gain_provided;
    float output_gain;
    bool output_gain_provided;
    float baseband_gain;
    bool baseband_gain_provided;
    double frequency_shift_hz;
    bool shift_after_resample;
    bool raw_passthrough;

    IqCorrectionConfig iq_correction;
    DcBlockConfig dc_block;
    OutputAgcConfig output_agc;
    OutputAgcConfig baseband_agc;

    struct {
      FilterRequest requests[FILTER_MAX_CHAIN];
      int count;
      bool apply_post_resample;
      FilterTypeRequest type_req;
      struct {
        float lowpass[FILTER_MAX_CHAIN];
        float highpass[FILTER_MAX_CHAIN];
        const char *pass_range[FILTER_MAX_CHAIN];
        const char *stopband[FILTER_MAX_CHAIN];
        float transition_width;
        int taps;
        float attenuation;
        const char *type_str;
        int fft_size;
      } args;
    } filter;
  } dsp;

  // --- I/Q File Metadata (WAV/RAW/etc) ---
  struct {
    double rf_freq_hz;
    bool rf_freq_provided;
  } iq_file_metadata;

  // --- SDR General ---
  struct {
    double rf_freq_hz;
    double rf_freq_hz_arg;
    bool rf_freq_provided;
    double frequency_offset_hz;
    double frequency_offset_arg;
    double sample_rate_hz;
    double sample_rate_hz_arg;
    bool sample_rate_provided;
    bool bias_t_enable;
  } sdr_general;

  // --- Global / Misc ---
  char *preset_name;
  bool help_requested;
  PresetDefinition *presets;
  int num_presets;
} AppConfig;

// =========================================================
// == PART 2: RUNTIME CONTEXTS (Dynamic State)
// =========================================================

// --- 1. Infrastructure Context (The Plumbing) ---
typedef struct ProcessChainInfrastructure {
  MemoryArena setup_arena;
  void *chunk_data_pool;
  struct SampleChunk **sample_chunk_pool;

  size_t alloc_size_samples;
  size_t read_chunk_size;
  size_t num_chunks;
  unsigned int max_out_samples;

  size_t input_buffer_size;

  Queue *free_sample_chunk_queue;
  Queue *reader_output_queue;
  Queue **active_queues;
  int num_active_queues;
  Queue *writer_input_queue;

  struct RingBuffer *source_input_buffer;

  WaitEvent *shutdown_event;
} ProcessChainInfrastructure;

// --- 2. Module Context (Drivers & IO) ---
typedef struct ModuleState {
  InputModuleInterface *input_api;
  void *input_private_data;
  InputSourceInfo source_info;
  SampleFormat input_format;
  size_t input_bytes_per_iq_sample;

  OutputModuleInterface *output_api;
  void *output_private_data;
  size_t output_bytes_per_iq_sample;
  QueueSamples queue_samples;
  void *process_chain_context;
} ModuleState;

// --- 3. DSP Context (Math & Signal Processing) ---

typedef struct DspContext {
  const struct AppConfig *config; // Injected for DSP access

  void *states[DSP_MAX_MODULES];

  double nco_shift_hz;
  double process_chain_sample_rate_hz;
  SampleFormat process_chain_sample_format;
  float process_chain_gain;
  OutputAgcConfig process_chain_agc;
} DspContext;

// --- 4. Runtime Context (Metrics & Telemetry) ---
typedef void (*ProgressUpdateFn)(unsigned long long current_output_frames,
                                 long long total_output_frames,
                                 unsigned long long current_bytes_written,
                                 void *udata);

typedef struct RuntimeState {

  _Atomic double last_source_heartbeat_time;
  atomic_bool error_occurred;
  atomic_bool end_of_stream_reached;

  atomic_uint_least64_t total_frames_read;
  atomic_uint_least64_t total_output_frames;
  atomic_int_least64_t final_output_size_bytes;
  atomic_int_least64_t expected_total_output_frames;
  time_t start_time;

  ProgressUpdateFn progress_callback;
  void *progress_callback_udata;
} RuntimeState;

// =========================================================
// == The Main Container (formerly AppResources)
// =========================================================
typedef struct AppContext {
  const struct AppConfig *config;

  ProcessChainInfrastructure process_chain;
  DspContext dsp;
  ModuleState module;
  RuntimeState stats;

  ProcessChainMode process_chain_mode;
} AppContext;

#endif // APP_CONTEXT_H_
