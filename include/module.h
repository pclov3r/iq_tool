/**
 * @file module.h
 * @brief Defines the abstract interface for all I/Q data input sources.
 *
 * This file is the cornerstone of the modular input system. It defines a
 * generic interface (`InputModuleInterface`) using a structure of function
 * pointers. Any concrete input source, whether a file reader (WAV, raw) or a
 * live SDR device (RTL-SDR, SDRplay), must provide an implementation of this
 * interface.
 *
 * It also defines the common data structures used to describe an input source's
 * properties and metadata.
 */

#ifndef MODULE_H_
#define MODULE_H_

#include "argparse.h"     // For argparse_option struct
#include "common_types.h" // For SampleFormat
#include "config/constants.h"    // For APP_MAX_SUMMARY_ITEMS
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

typedef enum {
  MODULE_TYPE_INPUT,
  MODULE_TYPE_OUTPUT,
  MODULE_TYPE_DSP,
} ModuleType;

// --- Forward Declarations ---
// These break circular dependencies and allow us to use pointers to these
// structs without needing their full definitions in this header.
struct AppConfig;
struct AppContext;
struct InputSummaryInfo;
typedef struct InputSummaryInfo OutputSummaryInfo;
struct ModuleContext;
struct SampleChunk;

// --- Data Structures ---

/**
 * @struct InputSourceInfo
 * @brief Holds basic, essential information about the input source.
 */
typedef struct InputSourceInfo {
  int64_t frames;  ///< Total number of I/Q frames in the source. -1 for a live
                   ///< stream.
  int sample_rate; ///< The native sample rate of the source in Hz.
  size_t demod_audio_buffer_size; ///< Downstream demodulator audio buffer size
                                  ///< dictated by this input source.
} InputSourceInfo;

/**
 * @struct ModuleContext
 * @brief A container passing the main application state to input source
 * functions.
 */
typedef struct ModuleContext {
  const struct AppConfig *config;
  struct AppContext *app;
} ModuleContext;

// --- The Core Interface Definition ---

/**
 * @struct InputModuleInterface
 * @brief The "vtable" of function pointers that defines the module interface.
 */
typedef bool (*QueueSamples)(void *process_chain_context, const void *data,
                             size_t num_samples, SampleFormat format);

typedef struct InputModuleInterface {
  /**
   * @brief Performs initial setup (e.g., open file, select SDR device, set SDR
   * parameters).
   * @param context The application context.
   * @return true on success, false on failure.
   */
  bool (*initialize)(struct ModuleContext *context);

  /**
   * @brief Starts the data stream. This is a blocking call that runs in the
   * reader thread.
   * @param context The application context.
   * @return NULL on normal exit.
   */
  void *(*push_samples_to_queue)(struct ModuleContext *context,
                                 QueueSamples queue_samples,
                                 void *process_chain_context);

  /**
   * @brief Reads a chunk of data synchronously from a file-based source.
   */
  size_t (*read_chunk)(struct ModuleContext *context, void *buffer,
                       size_t bytes_to_read);

  /**
   * @brief Gracefully stops the data stream (e.g., cancels an async SDR read).
   * @param context The application context.
   */
  void (*stop_sample_queue_push)(struct ModuleContext *context);

  /**
   * @brief Releases all app allocated by the input source.
   * @param context The application context.
   */
  void (*cleanup)(struct ModuleContext *context);

  /**
   * @brief Populates a summary struct with details specific to this input
   * source.
   * @param context A read-only pointer to the application context.
   * @param info A pointer to the summary struct to be populated.
   */
  void (*get_summary_info)(const struct ModuleContext *context,
                           struct InputSummaryInfo *info);

  /**
   * @brief Passes keypress events to the module.
   * @param context The application context.
   * @param key The integer key code.
   */
  void (*on_keypress)(struct ModuleContext *context, int key);

  /**
   * @brief Validates and post-processes command-line options specific to this
   * module.
   * @param config A pointer to the application configuration, which can be
   * modified.
   * @return true if the options are valid, false otherwise.
   */
  bool (*validate_options)(struct AppContext *app);

  /**
   * @brief Validates generic options (e.g., --sdr-rf-freq) in the context of
   * this module.
   * @param config A read-only pointer to the application configuration.
   * @return true if the generic options are valid for this module, false
   * otherwise.
   */
  bool (*validate_generic_options)(const struct AppConfig *config);

  // Optional function for file-based sources to perform initial I/Q correction.
  bool (*pre_stream_iq_correction)(struct ModuleContext *context);

} InputModuleInterface;

/**
 * @struct OutputModuleInterface
 * @brief The "vtable" for output modules (writers).
 */
typedef struct OutputModuleInterface {
  // Validates module-specific options (e.g., WAV only supports cs16/cu8)
  bool (*validate_options)(struct AppContext *app);

  // Returns any CLI options specific to this output format
  const struct argparse_option *(*get_cli_options)(int *count);

  // Performs all one-time setup for the writer (opens files, etc.)
  bool (*initialize)(struct ModuleContext *context);

  void (*reset)(struct ModuleContext *context);
  void (*flush)(struct ModuleContext *context);

  /**
   * @brief Writes a single chunk of data directly to the output.
   * This is used for special cases like raw_passthrough mode.
   * @param context The application context.
   * @param buffer Pointer to the data to write.
   * @param bytes_to_write The number of bytes to write.
   * @return The number of bytes successfully written.
   */
  size_t (*write_chunk)(struct ModuleContext *context, const void *buffer,
                        size_t bytes_to_write);

  // Finalizes the output (e.g., updates WAV headers) and closes handles
  void (*cleanup)(struct ModuleContext *context);

  // Populates a summary struct with output details

  void (*get_summary_info)(const ModuleContext *context,
                           OutputSummaryInfo *info);
  void (*on_keypress)(ModuleContext *context, int key);
} OutputModuleInterface;

/**
 * @struct DspModuleInterface
 * @brief The "vtable" for DSP processing blocks (filters, resamplers, AGC).
 */
typedef struct DspModuleInterface {
  const char *name;

  // Determines if this module should be active in the current flowgraph stage
  bool (*is_active)(struct AppContext *app, const char *stage_tag);

  // Performs all one-time setup for the DSP block (allocates liquid-dsp
  // objects)
  void *(*initialize)(struct ModuleContext *context);

  // Processes a chunk of samples. The module may modify the chunk in-place,
  // or modify the chunk metadata (like sample_rate or frames_read).
  struct SampleChunk *(*process)(void *state, struct SampleChunk *chunk);

  // Resets internal state (e.g., clears history buffers, resets AGC gain)
  void (*reset)(void *state);

  // Validates module-specific options
  bool (*validate_options)(struct AppContext *app);

  // Returns any CLI options specific to this DSP module
  const struct argparse_option *(*get_cli_options)(int *count);

  // Appends to the summary info block before processing starts (optional)
  void (*get_summary_info)(void *state, OutputSummaryInfo *info);

  // Frees all allocated memory
  void (*cleanup)(void *state);
} DspModuleInterface;


#endif // MODULE_H_

