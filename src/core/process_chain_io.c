/**
 * @file process_chain_stages.c
 * @brief Implements the high-speed data processing stages for the DSP
 * process_chain.
 */

#include "process_chain_io.h"
#include "app_context.h"
#include "config/constants.h"
#include "input/common.h"
#include "log.h"
#include "module_registry.h"
#include "packet_serializer.h"
#include "platform.h"
#include "process_chain_context.h"
#include "queue.h"
#include "ring_buffer.h"
#include "sample_conversion_functions.h"
#include "signal_handler.h"
#include "utilities.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

// --- ProcessChain Thread Function Implementations (Private to this module) ---
// Universal Ingest Callback
static bool process_chain_queue_samples(void *context, const void *data,
                                        size_t num_samples,
                                        SampleFormat format) {
  AppContext *app = (AppContext *)context;
  source_update_heartbeat(app);
  if (is_shutdown_requested() ||
      atomic_load_explicit(&app->stats.error_occurred, memory_order_relaxed))
    return false;

  if (!packet_serializer_write_packet(app->process_chain.source_input_buffer,
                                      num_samples, data, format)) {
    static double last_drop_log_time = 0.0;
    static size_t accumulated_drops = 0;

    accumulated_drops += num_samples;
    double current_time = utility_get_time();

    if (current_time - last_drop_log_time >= CONSOLE_UPDATE_INTERVAL_SEC) {
      log_warn("ProcessChain input overrun! Dropped %zu samples.",
               accumulated_drops);
      accumulated_drops = 0;
      last_drop_log_time = current_time;
    }
    return false;
  }
  return true;
}

void *process_chain_thread_source(void *arg) {
  platform_set_thread_priority(PRIORITY_REALTIME, "Source");

  ProcessChainContext *args = (ProcessChainContext *)arg;
  AppContext *app = args->app;
  ModuleContext context = {.config = args->config, .app = app};

  app->module.input_api->push_samples_to_queue(
      &context, process_chain_queue_samples, app);

  if (app->process_chain.source_input_buffer) {
    ring_buffer_signal_end_of_stream(app->process_chain.source_input_buffer);
  }

  log_debug("Source capture thread is exiting.");
  return NULL;
}

void *process_chain_thread_reader(void *arg) {
  platform_set_thread_priority(PRIORITY_NORMAL, "Reader");

  ProcessChainContext *args = (ProcessChainContext *)arg;
  AppContext *app = args->app;
  AppConfig *config = args->config;

  switch (app->process_chain_mode) {
  case PROCESS_CHAIN_MODE_ASYNCHRONOUS_PUSH: {
    log_debug("Reader thread starting.");

    // --- STATEFUL SIPPING LOGIC ---
    // Initialize the serializer state for this thread.
    SerializerState state;
    memset(&state, 0, sizeof(state));

    while (!is_shutdown_requested() &&
           !atomic_load_explicit(&app->stats.error_occurred,
                                 memory_order_relaxed)) {
      SampleChunk *item = (SampleChunk *)queue_dequeue(
          app->process_chain.free_sample_chunk_queue);
      if (!item)
        break;

      bool is_reset = false;

      // Call the serializer with the state and the calculated elastic request
      // size
      int64_t frames_read = packet_serializer_read_packet(
          app->process_chain.source_input_buffer, item, &state, &is_reset,
          app->process_chain.read_chunk_size);

      if (frames_read < 0) {
        request_forceful_shutdown(
            "Reader: Fatal error parsing source buffer stream.", app);
        queue_enqueue(app->process_chain.free_sample_chunk_queue, item);
        break;
      }

      if (frames_read == 0 && !is_reset) {
        item->is_last_chunk = true;
        item->frames_read = 0;
        queue_enqueue(app->process_chain.reader_output_queue, item);
        break;
      }

      item->frames_read = frames_read;
      item->frames_to_write = (unsigned int)frames_read;
      item->stream_discontinuity_event = is_reset;
      item->is_last_chunk = false;

      item->current_buffer = item->buffer_a;

      if (config->dsp.raw_passthrough && item->frames_read > 0) {
        size_t bytes = item->frames_read * item->input_bytes_per_iq_sample;
        memcpy(item->final_output_data, item->raw_input_data, bytes);
      } else if (!config->dsp.raw_passthrough && item->frames_read > 0) {
        sample_convert_block_to_cf32(
            item->raw_input_data, item->current_buffer, item->frames_read,
            item->packet_sample_format, config->dsp.input_gain);
      }

      if (item->frames_read > 0) {
        atomic_fetch_add_explicit(&app->stats.total_frames_read,
                                  item->frames_read, memory_order_relaxed);
      }

      if (!queue_enqueue(app->process_chain.reader_output_queue, item)) {
        // The process_chain is shutting down, so we can't send data forward.
        // We drop the data, but we MUST return the memory to the pool.
        // We use forced enqueue to guarantee the pool accepts it.
        queue_enqueue_forced(app->process_chain.free_sample_chunk_queue, item);
        break;
      }
    }
    break;
  }

  case PROCESS_CHAIN_MODE_SYNCHRONOUS_PULL: {
    ModuleContext context = {.config = config, .app = app};
    InputModuleInterface *in_api = app->module.input_api;

    if (!in_api->read_chunk) {
      request_forceful_shutdown("Reader: File input module missing read_chunk.",
                                app);
    } else {
      while (!is_shutdown_requested() &&
             !atomic_load_explicit(&app->stats.error_occurred,
                                   memory_order_relaxed)) {
        SampleChunk *item = (SampleChunk *)queue_dequeue(
            app->process_chain.free_sample_chunk_queue);
        if (!item)
          break;

        size_t bytes_requested = app->process_chain.read_chunk_size *
                                 app->module.input_bytes_per_iq_sample;
        size_t capacity_bytes = item->raw_input_capacity_bytes;
        if (bytes_requested > capacity_bytes)
          bytes_requested = capacity_bytes;

        void *target_buffer = config->dsp.raw_passthrough
                                  ? item->final_output_data
                                  : item->raw_input_data;
        size_t bytes_read =
            in_api->read_chunk(&context, target_buffer, bytes_requested);

        item->frames_read = bytes_read / app->module.input_bytes_per_iq_sample;
        item->frames_to_write = (unsigned int)item->frames_read;
        item->packet_sample_format = app->module.input_format;
        item->input_bytes_per_iq_sample = app->module.input_bytes_per_iq_sample;
        item->stream_discontinuity_event = false;
        item->is_last_chunk = (bytes_read == 0); // EOF reached

        item->current_buffer = item->buffer_a;

        if (!config->dsp.raw_passthrough && item->frames_read > 0) {
          sample_convert_block_to_cf32(
              item->raw_input_data, item->current_buffer,
              item->frames_read, item->packet_sample_format,
              config->dsp.input_gain);
        }

        if (item->frames_read > 0) {
          atomic_fetch_add_explicit(&app->stats.total_frames_read,
                                    item->frames_read, memory_order_relaxed);
        }

        if (!queue_enqueue(app->process_chain.reader_output_queue, item)) {
          queue_enqueue_forced(app->process_chain.free_sample_chunk_queue,
                               item);
          break;
        }
        if (item->is_last_chunk)
          break;
      }
    }
    break;
  }
  }

  if (!is_shutdown_requested()) {
    log_debug("Reader thread finished naturally. End of stream reached.");
    atomic_store_explicit(&app->stats.end_of_stream_reached, true,
                          memory_order_release);
  } else {
    SampleChunk *last_item = (SampleChunk *)queue_try_dequeue(
        app->process_chain.free_sample_chunk_queue);
    if (last_item) {
      last_item->is_last_chunk = true;
      last_item->frames_read = 0;
      queue_enqueue(app->process_chain.reader_output_queue, last_item);
    }
  }

  log_debug("Reader thread is exiting.");
  return NULL;
}

void *process_chain_thread_writer(void *arg) {
  platform_set_thread_priority(PRIORITY_HIGHEST, "Writer");

  ProcessChainContext *args = (ProcessChainContext *)arg;
  AppContext *app = args->app;
  OutputModuleInterface *out_api = app->module.output_api;
  ModuleContext context = {.config = args->config, .app = app};

  if (!out_api || !out_api->write_chunk) {
    log_error("Output module does not implement write_chunk.");
    return NULL;
  }

  while (true) {
    SampleChunk *item =
        (SampleChunk *)queue_dequeue(app->process_chain.writer_input_queue);
    if (!item)
      break; // Shutdown signaled

    // 1. Process payload FIRST if it exists
    // This ensures the partial "tail" of a file is written before we exit.
    if (item->frames_to_write > 0 && !item->stream_discontinuity_event) {
      if (!args->config->dsp.raw_passthrough) {
        if (app->dsp.process_chain_gain != 1.0f) {
          float g = app->dsp.process_chain_gain;
          for (unsigned int i = 0; i < item->frames_to_write; i++) {
            item->current_buffer[i] *= g;
          }
        }
        sample_convert_cf32_to_block(
            item->current_buffer, item->final_output_data,
            item->frames_to_write, app->dsp.process_chain_sample_format);
      }
      size_t bytes_to_write =
          item->frames_to_write * app->module.output_bytes_per_iq_sample;
      size_t written = out_api->write_chunk(&context, item->final_output_data,
                                            bytes_to_write);

      if (written < bytes_to_write) {
        log_error("Fatal I/O Error: Output module failed to write the complete "
                  "chunk (Disk full? Broken Pipe?). Initiating shutdown.");
        atomic_store_explicit(&app->stats.error_occurred, true,
                              memory_order_relaxed);
        request_shutdown();
      }

      // Stats & Progress Reporting (Only for file outputs)
      if (args->config->output.path_arg != NULL &&
          app->stats.progress_callback && written > 0) {
        atomic_fetch_add_explicit(&app->stats.total_output_frames,
                                  item->frames_to_write, memory_order_relaxed);
        // Correctly track bytes and signal progress
        int64_t total_bytes =
            atomic_fetch_add_explicit(&app->stats.final_output_size_bytes,
                                      (int64_t)written, memory_order_relaxed) +
            (int64_t)written;

        app->stats.progress_callback(
            atomic_load_explicit(&app->stats.total_output_frames,
                                 memory_order_relaxed),
            atomic_load_explicit(&app->stats.expected_total_output_frames,
                                 memory_order_relaxed),
            total_bytes, app->stats.progress_callback_udata);
      }
    }

    // 2. Handle Reset Event (Source Overrun)
    if (item->stream_discontinuity_event) {
      if (out_api->reset)
        out_api->reset(&context);
    }

    // 3. Handle End-of-Stream
    if (item->is_last_chunk) {
      if (out_api->flush)
        out_api->flush(&context);
      queue_enqueue(app->process_chain.free_sample_chunk_queue, item);
      break; // Loop exit happens here, AFTER processing data/flush
    }

    // 4. Return regular chunks to the pool
    if (!queue_enqueue(app->process_chain.free_sample_chunk_queue, item))
      break;
  }

  // 5. Signal auxiliary threads to shutdown on normal exit
  if (app->process_chain.iq_estimation_data_queue) {
    queue_signal_shutdown(app->process_chain.iq_estimation_data_queue);
  }

  log_debug("Generic Writer thread is exiting.");
  return NULL;
}
