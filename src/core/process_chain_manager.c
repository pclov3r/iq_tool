/**
 * @file process_chain.c
 * @brief Implements the creation, execution, and destruction of the
 * application's DSP process_chain.
 *
 * This module is the central orchestrator for the application's concurrent
 * processing. It contains the master `process_chain_run` function, as well as
 * the private implementations for all the process_chain's concurrent stages and
 * utility threads.
 */

#include "process_chain_manager.h"
#include "app_context.h"
#include "config/constants.h"
#include "config/process_chain_default.h"
#include "input/common.h"
#include "log.h"
#include "module_registry.h"
#include "packet_serializer.h"
#include "platform.h" // Added for thread priority abstraction
#include "process_chain_context.h"
#include "process_chain_io.h"
#include "queue.h"
#include "ring_buffer.h"
#include "sample_format_table.h"
#include "signal_handler.h"
#include "thread_manager.h"
#include "utilities.h"
#include "utility_threads.h"
#include "wait_event.h"
#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <time.h>
#include <unistd.h>
#endif

// Helper macro to align a size up to the next power of 2 boundary
#define ALIGN_UP(size, align) (((size) + (align) - 1) & ~((align) - 1))

// --- Private Function Prototypes for Setup Helpers ---
static bool _init_queues_and_buffers(AppConfig *config, AppContext *app);
static void _destroy_queues_and_buffers(AppContext *app);

/**
 * @brief Creates, runs, and waits for the entire processing process_chain to
 * complete.
 *
 * This is the main high-level function that encapsulates the entire
 * process_chain lifecycle. It handles the creation of all DSP objects and
 * queues, spawns all necessary threads using the thread manager, waits for them
 * to finish, and then cleans up all process_chain-specific app.
 *
 * @param context A pointer to the ProcessChainContext, containing the
 * application config and app.
 * @return true if the process_chain ran and shut down cleanly, false if there
 * was a setup or execution error.
 */
static bool calculate_and_validate_resample_ratio(AppConfig *config,
                                                  AppContext *app,
                                                  float *out_ratio) {
  if (!config || !app || !out_ratio)
    return false;

  // --- Step 1: Determine Target Rate ---
  double target_rate_hz = 0.0;
  if (config->output.payload == PAYLOAD_AUDIO) {
    target_rate_hz = config->baseband_sample_rate.rate_hz;
  } else {
    target_rate_hz = config->output_sample_rate.rate_hz;
  }

  // --- Step 2: Handle Smart Default (Missing Rate) ---
  // If the user didn't specify a rate (0), use the hardware/file input rate.
  if (target_rate_hz <= 0.0) {
    target_rate_hz = (double)app->module.source_info.sample_rate;
    log_info(
        "No explicit process_chain output rate specified. Defaulting to native "
        "input rate: %.15g Hz",
        target_rate_hz);
  }

  // Set the unified process_chain sample rate, format, gain, and agc
  app->dsp.process_chain_sample_rate_hz = target_rate_hz;
  app->dsp.process_chain_sample_format =
      (config->output.payload == PAYLOAD_AUDIO)
          ? config->baseband_sample_format.format
          : config->output.sample_format;
  app->dsp.process_chain_gain = (config->output.payload == PAYLOAD_AUDIO)
                                    ? config->dsp.baseband_gain
                                    : config->dsp.output_gain;
  app->dsp.process_chain_agc = (config->output.payload == PAYLOAD_AUDIO)
                                   ? config->dsp.baseband_agc
                                   : config->dsp.output_agc;

  // --- Step 3: Calculate Ratio ---
  double input_rate_d = (double)app->module.source_info.sample_rate;
  float r = (float)(app->dsp.process_chain_sample_rate_hz / input_rate_d);

  // --- Step 4: Check for Passthrough Conditions ---
  if (config->dsp.raw_passthrough) {
    log_info("Raw Passthrough mode enabled: Bypassing all DSP blocks.");
    app->dsp.bypass_resampler = true;
    r = 1.0f; // Force ratio to 1.0 for buffer calcs
    app->dsp.process_chain_sample_format = app->module.input_format;
    app->dsp.process_chain_sample_rate_hz = input_rate_d;
  } else if (fabs(r - 1.0f) < 1e-6) {
    app->dsp.bypass_resampler = true;
    r = 1.0f; // Snap to exact 1.0
  } else {
    app->dsp.bypass_resampler = false;
  }

  // --- Step 4: Validate Ratio ---
  if (!isfinite(r) || r < PROCESS_CHAIN_MIN_RATE_SCALAR ||
      r > PROCESS_CHAIN_MAX_RATE_SCALAR) {
    log_error("Error: Calculated resampling ratio (%.6f) is invalid or outside "
              "acceptable range.",
              r);
    return false;
  }
  *out_ratio = r;

  if (app->module.source_info.frames > 0) {
    atomic_store_explicit(
        &app->stats.expected_total_output_frames,
        (long long)round((double)app->module.source_info.frames * (double)r),
        memory_order_relaxed);
  } else {
    app->stats.expected_total_output_frames = -1;
  }

  return true;
}

static bool allocate_processing_buffers(AppConfig *config, AppContext *app,
                                        float resample_ratio) {
  if (!config || !app)
    return false;

  // 1. Determine the "Fat Pipe" (highest data rate side)
  //    Upsampling (Ratio > 1.0): Output is the Fat Pipe.
  //    Downsampling (Ratio <= 1.0): Input is the Fat Pipe.
  bool upsampling = (resample_ratio > 1.0f);

  size_t target_block_samples =
      PROCESS_CHAIN_TARGET_BLOCK_SAMPLES; // 12,288 samples (~192KB)

  // 2. Adjust target for FFT requirements if necessary by querying DSP modules
  size_t req_block_size = 0;
  if (!config->dsp.raw_passthrough) {
    for (int i = 0; i < DEFAULT_PROCESS_CHAIN_LENGTH; i++) {
      const struct DspModuleInterface *mod = get_dsp_module(
          DEFAULT_PROCESS_CHAIN[i].module_name, &app->process_chain.setup_arena);
      if (mod && mod->is_active(app, DEFAULT_PROCESS_CHAIN[i].stage_tag) &&
          mod->get_required_chunk_size) {
        size_t mod_req_size = mod->get_required_chunk_size(config);
        if (mod_req_size > req_block_size) {
          req_block_size = mod_req_size;
        }
      }
    }
  }

  if (req_block_size > target_block_samples) {
    log_info(
        "DSP module required block size (%zu) exceeds optimal process_chain "
        "target (%zu).",
        req_block_size, target_block_samples);
    log_info(
        "Expanding internal chunk size to accommodate DSP requirements (may "
        "reduce CPU cache efficiency).");
    target_block_samples = req_block_size;
  }

  size_t calculated_input_samples = 0;

  if (upsampling) {
    // --- CASE A: UPSAMPLING ---
    // The Output is pinned to the Target.
    // Calculate Input required: Input = Target / Ratio.
    size_t raw_input_calc = (size_t)(target_block_samples / resample_ratio);

    // Sanity Floor: Prevent tiny read requests that cause excessive locking
    // overhead.
    if (raw_input_calc < PROCESS_CHAIN_MIN_READ_SAMPLES) {
      calculated_input_samples = PROCESS_CHAIN_MIN_READ_SAMPLES;
    } else {
      calculated_input_samples = raw_input_calc;
    }
  } else {
    // --- CASE B: DOWNSAMPLING / PASSTHROUGH ---
    // The Input is pinned to the Target.
    calculated_input_samples = target_block_samples;
  }

  // --- Calculate Elastic Maximum Buffer Size ---
  // The filter object processes in blocks. If a remainder exists from a
  // previous chunk, the output of the filter can momentarily exceed the input
  // size by up to the FFT block size.
  size_t max_pre_resample_samples = calculated_input_samples;
  if (config->dsp.filter.count > 0 && !config->dsp.filter.apply_post_resample) {
    max_pre_resample_samples += req_block_size;
  }

  // Determine the absolute maximum number of samples that could exist
  // post-resampling
  size_t max_post_resample_samples =
      (size_t)ceil((double)(max_pre_resample_samples + 32) * resample_ratio) +
      64;

  if (config->dsp.filter.count > 0 && config->dsp.filter.apply_post_resample) {
    max_post_resample_samples += req_block_size;
  }

  // The buffer must be large enough to hold the maximum size at ANY stage of
  // the process_chain
  size_t sample_allocation_count =
      (max_pre_resample_samples > max_post_resample_samples)
          ? max_pre_resample_samples
          : max_post_resample_samples;
  sample_allocation_count += PROCESS_CHAIN_BUFFER_PADDING_SAMPLES;

  // Store the results for runtime usage
  app->process_chain.read_chunk_size = calculated_input_samples;
  app->process_chain.alloc_size_samples = sample_allocation_count;

  // 3. Absolute Chunk Limit Safety Check
  if (app->process_chain.alloc_size_samples > PROCESS_CHAIN_MAX_CHUNK_SAMPLES) {
    log_error(
        "Calculated process_chain chunk size (%zu samples) exceeds safety "
        "limit (%d).",
        app->process_chain.alloc_size_samples, PROCESS_CHAIN_MAX_CHUNK_SAMPLES);
    log_error("Try reducing the output sample rate or manually lowering "
              "--filter-taps.");
    return false;
  }

  // Update legacy field used by some filters
  app->process_chain.max_out_samples =
      (unsigned int)app->process_chain.alloc_size_samples;

  // -------------------------------------------------------------------------
  // 4. Calculate Dynamic ProcessChain Depth ("Trays")
  // -------------------------------------------------------------------------
  double input_rate = (double)app->module.source_info.sample_rate;

  // FAIL FAST: If the input rate is unknown or invalid, we cannot safely
  // configure the process_chain.
  if (input_rate <= 0.0) {
    log_fatal("Internal Error: Input sample rate is invalid (%.15g Hz). Cannot "
              "calculate buffer depth.",
              input_rate);
    log_error("Please check the input source configuration.");
    return false;
  }

  // How much time does one chunk represent?
  double seconds_per_chunk =
      (double)app->process_chain.read_chunk_size / input_rate;

  // How many chunks do we need to hit the target duration?
  size_t calculated_chunks =
      (size_t)(PROCESS_CHAIN_TARGET_BUFFER_DURATION_SEC / seconds_per_chunk);

  // Apply Sanity Clamps
  if (calculated_chunks < PROCESS_CHAIN_MIN_CHUNKS)
    calculated_chunks = PROCESS_CHAIN_MIN_CHUNKS;
  if (calculated_chunks > PROCESS_CHAIN_MAX_CHUNKS)
    calculated_chunks = PROCESS_CHAIN_MAX_CHUNKS;

  app->process_chain.num_chunks = calculated_chunks;

  log_info(
      "ProcessChain Sizing: Read=%zu samples, Alloc=%zu samples, Depth=%zu "
      "chunks (%.2f sec buffer at %.15g Hz)",
      app->process_chain.read_chunk_size, app->process_chain.alloc_size_samples,
      app->process_chain.num_chunks,
      app->process_chain.num_chunks * seconds_per_chunk, input_rate);

  // --- Monolithic Tray Allocation (Contiguous Metadata + Data) ---
  size_t raw_stride = ALIGN_UP(app->process_chain.alloc_size_samples *
                                   app->module.input_bytes_per_iq_sample,
                               MEM_ARENA_ALIGNMENT);
  size_t complex_stride =
      ALIGN_UP(app->process_chain.alloc_size_samples * sizeof(ComplexFloat),
               MEM_ARENA_ALIGNMENT);
  app->module.output_bytes_per_iq_sample =
      get_bytes_per_iq_sample(app->dsp.process_chain_sample_format);
  size_t final_stride = ALIGN_UP(app->process_chain.alloc_size_samples *
                                     app->module.output_bytes_per_iq_sample,
                                 MEM_ARENA_ALIGNMENT);

  size_t struct_stride = ALIGN_UP(sizeof(SampleChunk), MEM_ARENA_ALIGNMENT);
  size_t total_tray_size =
      struct_stride + raw_stride + (complex_stride * 2) + final_stride;

  // Allocate the big data block
  app->process_chain.chunk_data_pool = aligned_alloc(
      MEM_ARENA_ALIGNMENT, app->process_chain.num_chunks * total_tray_size);
  if (!app->process_chain.chunk_data_pool) {
    log_fatal("Error: Failed to allocate process_chain chunk data pool.");
    return false;
  }

  // Allocate the Catalog (The array of pointers)
  app->process_chain.sample_chunk_pool = (SampleChunk **)mem_arena_alloc(
      &app->process_chain.setup_arena,
      app->process_chain.num_chunks * sizeof(SampleChunk *), true);

  for (size_t i = 0; i < app->process_chain.num_chunks; ++i) {
    // Calculate the base address for this specific tray
    uint8_t *tray_base =
        (uint8_t *)app->process_chain.chunk_data_pool + (i * total_tray_size);

    // The Catalog entry points to the struct at the front of the tray
    app->process_chain.sample_chunk_pool[i] = (SampleChunk *)tray_base;
    SampleChunk *item = app->process_chain.sample_chunk_pool[i];

    // The buffers follow immediately after the metadata struct
    uint8_t *data_ptr = tray_base + struct_stride;

    item->raw_input_data = data_ptr;
    data_ptr += raw_stride;
    item->pre_resample_buffer = (ComplexFloat *)data_ptr;
    data_ptr += complex_stride;
    if (app->dsp.bypass_resampler) {
      item->post_resample_buffer = item->pre_resample_buffer;
    } else {
      item->post_resample_buffer = (ComplexFloat *)data_ptr;
    }
    data_ptr += complex_stride;
    item->final_output_data = (unsigned char *)data_ptr;

    // Set capacities and metadata
    item->raw_input_capacity_bytes = raw_stride;
    item->complex_buffer_capacity_samples =
        app->process_chain.alloc_size_samples;
    item->final_output_capacity_bytes = final_stride;
    item->input_bytes_per_iq_sample = app->module.input_bytes_per_iq_sample;
  }

  // -------------------------------------------------------------------------
  // 5. Calculate Dynamic Ring Buffer Sizes
  // -------------------------------------------------------------------------

  // Calculate input buffer size (for buffered mode)
  if (app->process_chain_mode != PROCESS_CHAIN_MODE_SYNCHRONOUS_PULL) {
    size_t input_buffer_bytes =
        (size_t)(input_rate * INPUT_BUFFER_DURATION_SEC *
                 app->module.input_bytes_per_iq_sample);

    if (input_buffer_bytes < INPUT_BUFFER_MIN_BYTES)
      input_buffer_bytes = INPUT_BUFFER_MIN_BYTES;
    if (input_buffer_bytes > INPUT_BUFFER_MAX_BYTES)
      input_buffer_bytes = INPUT_BUFFER_MAX_BYTES;

    app->process_chain.input_buffer_size = input_buffer_bytes;

    log_info(
        "Input Buffer: Allocating %zu bytes (%.2f sec capacity) at %.15g Hz.",
        input_buffer_bytes, INPUT_BUFFER_DURATION_SEC, input_rate);
  }

  return true;
}

bool process_chain_setup_buffers(ProcessChainContext *context) {
  AppConfig *config = context->config;
  AppContext *app = context->app;

  // --- Step 0: Calculate Ratios & Allocate Memory Pools ---
  if (!calculate_and_validate_resample_ratio(config, app,
                                             &app->dsp.resample_ratio))
    return false;
  if (!allocate_processing_buffers(config, app, app->dsp.resample_ratio))
    return false;

  return true;
}

bool process_chain_execute(ProcessChainContext *context) {
  AppConfig *config = context->config;
  AppContext *app = context->app;
  bool success = false;

  // --- Step 2: Verify memory pools (Allocated during initialization) ---
  if (!app->process_chain.chunk_data_pool) {
    log_fatal(
        "ProcessChain memory pool not allocated. Initialization order error.");
    process_chain_close_dsp_modules(context);
    return false;
  }

  // --- Step 3: Create and wire all communication channels ---
  if (!_init_queues_and_buffers(config, app)) {
    log_fatal("Failed to initialize process_chain queues and buffers.");
    _destroy_queues_and_buffers(app);
    process_chain_close_dsp_modules(context);
    return false;
  }

  // --- Step 4: Initialize the generic thread manager ---
  ThreadManager manager;
  thread_manager_init(&manager, context);

  // --- Step 5: Spawn threads based on configuration (Direct Command Model) ---
  log_debug("Spawning process_chain threads...");
  bool threads_ok = true;
  if (app->process_chain_mode != PROCESS_CHAIN_MODE_SYNCHRONOUS_PULL) {
    if (!thread_manager_spawn_thread(&manager, "Source",
                                     process_chain_thread_source))
      threads_ok = false;
  }
  if (threads_ok && !thread_manager_spawn_thread(&manager, "Reader",
                                                 process_chain_thread_reader))
    threads_ok = false;

  // Start DSP chains
  if (threads_ok && !config->dsp.raw_passthrough) {
    MemoryArena *arena = &app->process_chain.setup_arena;
    const struct DspModuleInterface *dsp_modules[16];
    void *dsp_states[16];
    int num_dsp_modules = 0;
    for (int i = 0; i < DEFAULT_PROCESS_CHAIN_LENGTH; i++) {
      const struct DspModuleInterface *mod =
          get_dsp_module(DEFAULT_PROCESS_CHAIN[i].module_name, arena);
      if (mod && mod->is_active(app, DEFAULT_PROCESS_CHAIN[i].stage_tag)) {
        dsp_modules[num_dsp_modules] = mod;
        dsp_states[num_dsp_modules] = app->dsp.states[i];
        num_dsp_modules++;
      }
    }

    for (int i = 0; i < num_dsp_modules; i++) {
      const struct DspModuleInterface *single_chain[1] = {dsp_modules[i]};
      void *single_state[1] = {dsp_states[i]};
      if (!thread_manager_start_chain(
              &manager, dsp_modules[i]->name, single_chain, single_state, 1,
              app->process_chain.active_queues[i],
              app->process_chain.active_queues[i + 1])) {
        threads_ok = false;
        break;
      }
    }
  }

  if (threads_ok && !thread_manager_spawn_thread(&manager, "Writer",
                                                 process_chain_thread_writer))
    threads_ok = false;
  if (threads_ok && config->dsp.iq_correction.enable) {
    if (!thread_manager_spawn_thread(&manager, "I/Q Optimizer",
                                     process_chain_thread_iq_estimator))
      threads_ok = false;
  }
  if (threads_ok && module_is_live_source(config->input.type_name,
                                          &app->process_chain.setup_arena)) {
    if (!thread_manager_spawn_thread(&manager, "Source Watchdog",
                                     process_chain_thread_watchdog))
      threads_ok = false;
  }

  if (!threads_ok) {
    log_fatal("Failed to spawn one or more process_chain threads. Initiating "
              "shutdown.");
    request_shutdown(); // Signal any successfully started threads to stop
  }

  // --- Step 6: Wait for all spawned threads to complete ---
  thread_manager_join_all(&manager);
  log_debug("All process_chain threads have completed.");
  success =
      !atomic_load_explicit(&app->stats.error_occurred, memory_order_relaxed);

  // --- Step 7: Clean up all process_chain-specific app ---
  return success;
}

void process_chain_teardown_buffers(ProcessChainContext *context) {
  if (!context || !context->app)
    return;
  _destroy_queues_and_buffers(context->app);
}

void process_chain_get_summary_info(const AppContext *app,
                                    OutputSummaryInfo *info) {
  for (int i = 0; i < DEFAULT_PROCESS_CHAIN_LENGTH; i++) {
    const struct DspModuleInterface *mod =
        get_dsp_module(DEFAULT_PROCESS_CHAIN[i].module_name, NULL);
    if (mod &&
        mod->is_active((AppContext *)app, DEFAULT_PROCESS_CHAIN[i].stage_tag) &&
        mod->get_summary_info) {
      mod->get_summary_info(app->dsp.states[i], info);
    }
  }
}

void *process_chain_get_module_state(const AppContext *app,
                                     const char *module_name) {
  for (int i = 0; i < DEFAULT_PROCESS_CHAIN_LENGTH; i++) {
    if (strcmp(DEFAULT_PROCESS_CHAIN[i].module_name, module_name) == 0) {
      return app->dsp.states[i];
    }
  }
  return NULL;
}

// --- Private Helper Function Implementations ---

bool process_chain_init_dsp_modules(ProcessChainContext *context) {
  AppConfig *config = context->config;
  AppContext *app = context->app;
  float resample_ratio = app->dsp.resample_ratio;
  (void)resample_ratio; // Handled by resampler module directly now
  ModuleContext mctx = {.config = config, .app = app};

  if (config->dsp.raw_passthrough) {
    // In raw passthrough, do not initialize any DSP modules
    app->process_chain.shutdown_event =
        wait_event_create(&app->process_chain.setup_arena);
    return app->process_chain.shutdown_event != NULL;
  }

  for (int i = 0; i < DEFAULT_PROCESS_CHAIN_LENGTH; i++) {
    app->dsp.states[i] = NULL;
    const struct DspModuleInterface *mod = get_dsp_module(
        DEFAULT_PROCESS_CHAIN[i].module_name, &app->process_chain.setup_arena);
    if (mod && mod->is_active(app, DEFAULT_PROCESS_CHAIN[i].stage_tag)) {
      if (mod->initialize) {
        app->dsp.states[i] = mod->initialize(&mctx);
        if (!app->dsp.states[i])
          return false;
      }
    }
  }

  // Initialize the main shutdown event
  app->process_chain.shutdown_event =
      wait_event_create(&app->process_chain.setup_arena);
  if (!app->process_chain.shutdown_event) {
    log_fatal("Failed to create shutdown event.");
    return false;
  }
  return true;
}

void process_chain_close_dsp_modules(ProcessChainContext *context) {
  AppContext *app = context->app;
  if (!app) return;
  for (int i = DEFAULT_PROCESS_CHAIN_LENGTH - 1; i >= 0; i--) {
    const struct DspModuleInterface *mod = get_dsp_module(
        DEFAULT_PROCESS_CHAIN[i].module_name, &app->process_chain.setup_arena);
    if (mod && mod->is_active(app, DEFAULT_PROCESS_CHAIN[i].stage_tag)) {
      if (mod->cleanup && app->dsp.states[i]) {
        mod->cleanup(app->dsp.states[i]);
        app->dsp.states[i] = NULL;
      }
    }
  }
}

static bool _init_queues_and_buffers(AppConfig *config, AppContext *app) {
  MemoryArena *arena = &app->process_chain.setup_arena;
  size_t queue_capacity = app->process_chain.num_chunks;

  // Collect requested modules
  int num_dsp_modules = 0;

  if (!config->dsp.raw_passthrough) {
    for (int i = 0; i < DEFAULT_PROCESS_CHAIN_LENGTH; i++) {
      const struct DspModuleInterface *mod =
          get_dsp_module(DEFAULT_PROCESS_CHAIN[i].module_name,
                         &app->process_chain.setup_arena);
      if (mod && mod->is_active(app, DEFAULT_PROCESS_CHAIN[i].stage_tag)) {
        num_dsp_modules++;
      }
    }
  }

  app->process_chain.num_active_queues =
      (num_dsp_modules > 0) ? num_dsp_modules + 1 : 1;
  app->process_chain.active_queues = (Queue **)mem_arena_alloc(
      arena, app->process_chain.num_active_queues * sizeof(Queue *), true);

  // active_queues[0] is reader_output
  app->process_chain.reader_output_queue =
      (Queue *)mem_arena_alloc(arena, sizeof(Queue), true);
  if (!app->process_chain.reader_output_queue ||
      !queue_init(app->process_chain.reader_output_queue, queue_capacity,
                  arena))
    return false;
  app->process_chain.active_queues[0] = app->process_chain.reader_output_queue;

  // allocate intermediate queues
  for (int i = 1; i < app->process_chain.num_active_queues - 1; i++) {
    app->process_chain.active_queues[i] =
        (Queue *)mem_arena_alloc(arena, sizeof(Queue), true);
    if (!app->process_chain.active_queues[i] ||
        !queue_init(app->process_chain.active_queues[i], queue_capacity, arena))
      return false;
  }

  // allocate writer input queue
  if (app->process_chain.num_active_queues > 1) {
    app->process_chain.writer_input_queue =
        (Queue *)mem_arena_alloc(arena, sizeof(Queue), true);
    if (!app->process_chain.writer_input_queue ||
        !queue_init(app->process_chain.writer_input_queue, queue_capacity,
                    arena))
      return false;
    app->process_chain.active_queues[app->process_chain.num_active_queues - 1] =
        app->process_chain.writer_input_queue;
  } else {
    app->process_chain.writer_input_queue =
        app->process_chain.reader_output_queue;
  }

  app->process_chain.free_sample_chunk_queue =
      (Queue *)mem_arena_alloc(arena, sizeof(Queue), true);
  if (!queue_init(app->process_chain.free_sample_chunk_queue, queue_capacity,
                  arena))
    return false;

  if (config->dsp.iq_correction.enable) {
    app->process_chain.iq_estimation_data_queue =
        (Queue *)mem_arena_alloc(arena, sizeof(Queue), true);
    if (!queue_init(app->process_chain.iq_estimation_data_queue, queue_capacity,
                    arena))
      return false;

    app->process_chain.iq_estimation_free_queue =
        (Queue *)mem_arena_alloc(arena, sizeof(Queue), true);
    if (!queue_init(app->process_chain.iq_estimation_free_queue, queue_capacity,
                    arena))
      return false;

    for (int i = 0; i < 16; i++) {
      void *buffer = mem_arena_alloc(arena, 4096 * sizeof(ComplexFloat), false);
      queue_enqueue(app->process_chain.iq_estimation_free_queue, buffer);
    }
  }

  for (size_t i = 0; i < app->process_chain.num_chunks; ++i) {
    if (!queue_enqueue(app->process_chain.free_sample_chunk_queue,
                       app->process_chain.sample_chunk_pool[i])) {
      log_fatal("Failed to initially populate free item queue.");
      return false;
    }
  }

  if (app->process_chain_mode != PROCESS_CHAIN_MODE_SYNCHRONOUS_PULL) {
    if (app->process_chain.source_input_buffer == NULL) {
      app->process_chain.source_input_buffer =
          ring_buffer_create(app->process_chain.input_buffer_size, arena);
      if (!app->process_chain.source_input_buffer)
        return false;
    }
  }
  return true;
}

static void _destroy_queues_and_buffers(AppContext *app) {
  if (!app)
    return;

  if (app->process_chain.shutdown_event) {
    wait_event_destroy(app->process_chain.shutdown_event);
  }

  if (app->process_chain.source_input_buffer)
    ring_buffer_destroy(app->process_chain.source_input_buffer);

  if (app->process_chain.free_sample_chunk_queue)
    queue_destroy(app->process_chain.free_sample_chunk_queue);

  if (app->process_chain.active_queues) {
    for (int i = 0; i < app->process_chain.num_active_queues; i++) {
      if (app->process_chain.active_queues[i] &&
          (i != app->process_chain.num_active_queues - 1 ||
           app->process_chain.num_active_queues == 1 ||
           app->process_chain.active_queues[i] !=
               app->process_chain.active_queues[0])) {
        queue_destroy(app->process_chain.active_queues[i]);
      }
    }
  }

  if (app->process_chain.iq_estimation_data_queue)
    queue_destroy(app->process_chain.iq_estimation_data_queue);
  if (app->process_chain.iq_estimation_free_queue)
    queue_destroy(app->process_chain.iq_estimation_free_queue);

  if (app->process_chain.chunk_data_pool) {
    aligned_free(app->process_chain.chunk_data_pool);
    app->process_chain.chunk_data_pool = NULL;
  }
}
