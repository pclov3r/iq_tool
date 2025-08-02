#define _POSIX_C_SOURCE 200809L

#include "resample.h"
#include "setup.h"
#include "types.h"
#include "config.h"
#include "utils.h"
#include "spectrum_shift.h"
#include "signal_handler.h"
#include "log.h"
#include "sample_convert.h"
#include "input_source.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <pthread.h>

#ifdef _WIN32
#include <liquid.h>
#include <windows.h> // For Sleep()
#else
#include <liquid/liquid.h>
#include <time.h>    // For nanosleep()
#endif

// --- Add an external declaration for the global console mutex defined in main.c ---
// This is no longer directly used for printing in resample.c, but kept for other potential uses
// or if the callback itself needs it (which it does, in main.c).
extern pthread_mutex_t g_console_mutex;

// --- Forward Declarations for Thread Functions ---
static void* reader_thread_func(void* arg);
static void* processor_thread_func(void* arg);
static void* writer_thread_func(void* arg);

// --- Forward Declarations for Static Helpers ---
static bool resample_block(AppConfig *config, AppResources *resources, WorkItem *item, unsigned int *num_output_frames);
static void convert_complex_to_output(AppConfig *config, AppResources *resources, WorkItem *item, unsigned int num_frames);
static void handle_thread_error(const char* thread_name, const char* error_msg, AppResources *resources);

// Struct to pass arguments to threads
typedef struct {
    AppConfig* config;
    AppResources* resources;
} ThreadArgs;


static void handle_thread_error(const char* thread_name, const char* error_msg, AppResources *resources) {
    log_fatal("In %s thread: %s", thread_name, error_msg);

    pthread_mutex_lock(&resources->progress_mutex);
    bool already_error = resources->error_occurred;
    resources->error_occurred = true;
    pthread_mutex_unlock(&resources->progress_mutex);

    if (!already_error) {
        log_info("Signaling shutdown to other threads...");
        if (resources->input_q) queue_signal_shutdown(resources->input_q);
        if (resources->output_q) queue_signal_shutdown(resources->output_q);
        if (resources->free_pool_q) queue_signal_shutdown(resources->free_pool_q);
    }
}

bool run_processing_threads(AppConfig *config, AppResources *resources) {
    if (!config || !resources) return false;

    static ThreadArgs thread_args;
    thread_args.config = config;
    thread_args.resources = resources;

    if (pthread_create(&resources->reader_thread, NULL, reader_thread_func, &thread_args) != 0 ||
        pthread_create(&resources->processor_thread, NULL, processor_thread_func, &thread_args) != 0 ||
        pthread_create(&resources->writer_thread, NULL, writer_thread_func, &thread_args) != 0) {
        log_fatal("Failed to create one or more threads: %s", strerror(errno));
        handle_thread_error("Main", "Thread creation failed", resources);
        return false;
    }

    return true;
}

static bool resample_block(AppConfig *config, AppResources *resources, WorkItem *item, unsigned int *num_output_frames) {
    if (item->frames_read <= 0) {
        *num_output_frames = 0;
        return true;
    }

    complex_float_t *input_for_stage = (resources->shifter_nco != NULL && !config->shift_after_resample)
                                            ? item->complex_buffer_shifted
                                            : item->complex_buffer_scaled;

    // --- START: MODIFIED SECTION for Passthrough Logic ---
    if (resources->is_passthrough) {
        // In passthrough mode, the ratio is 1.0, so we just copy the data.
        *num_output_frames = (unsigned int)item->frames_read;
        if (*num_output_frames > 0) {
            memcpy(item->complex_buffer_resampled, input_for_stage, *num_output_frames * sizeof(complex_float_t));
        }
    } else {
        // In resampling mode, execute the liquid-dsp resampler.
        msresamp_crcf_execute(resources->resampler,
                              (liquid_float_complex*)input_for_stage,
                              (unsigned int)item->frames_read,
                              (liquid_float_complex*)item->complex_buffer_resampled,
                              num_output_frames);

        if (*num_output_frames > resources->max_out_samples) {
             log_fatal("Resampler buffer overflow detected! This should not happen.");
             return false;
        }
    }
    // --- END: MODIFIED SECTION ---

    return true;
}

static void convert_complex_to_output(AppConfig *config, AppResources *resources, WorkItem *item, unsigned int num_frames) {
    (void)resources;
    if (num_frames == 0) return;
    complex_float_t *final_complex_data = (config->shift_after_resample && resources->shifter_nco != NULL)
                                               ? item->complex_buffer_shifted
                                               : item->complex_buffer_resampled;

    if (config->sample_format == SAMPLE_TYPE_CU8) {
        uint8_t *out_ptr_u8 = (uint8_t*)item->output_buffer;
        for (unsigned int i = 0; i < num_frames; ++i) {
            *out_ptr_u8++ = float_to_uchar(crealf(final_complex_data[i]));
            *out_ptr_u8++ = float_to_uchar(cimagf(final_complex_data[i]));
        }
    } else if (config->sample_format == SAMPLE_TYPE_CS8) {
        int8_t *out_ptr_s8 = (int8_t*)item->output_buffer;
        for (unsigned int i = 0; i < num_frames; ++i) {
            *out_ptr_s8++ = float_to_schar(crealf(final_complex_data[i]));
            *out_ptr_s8++ = float_to_schar(cimagf(final_complex_data[i]));
        }
    } else { // Handles SAMPLE_TYPE_CS16
        int16_t *out_ptr_s16 = (int16_t*)item->output_buffer;
        for (unsigned int i = 0; i < num_frames; ++i) {
            float real_val = crealf(final_complex_data[i]);
            float imag_val = cimagf(final_complex_data[i]);
            real_val = fmaxf(-32768.0f, fminf(32767.0f, real_val));
            imag_val = fmaxf(-32768.0f, fminf(32767.0f, imag_val));
            *out_ptr_s16++ = (int16_t)lrintf(real_val);
            *out_ptr_s16++ = (int16_t)lrintf(imag_val);
        }
    }
}

static void* reader_thread_func(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    AppResources *resources = args->resources;
    AppConfig *config = args->config;

    InputSourceContext ctx = { .config = config, .resources = resources };

    resources->selected_input_ops->start_stream(&ctx);
    resources->selected_input_ops->stop_stream(&ctx);

    return NULL;
}

static void* processor_thread_func(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    AppResources *resources = args->resources;
    AppConfig *config = args->config;
    
    while (true) {
        WorkItem *item = (WorkItem*)queue_dequeue(resources->input_q);
        if (!item) break;
        if (item->is_last_chunk) {
            queue_enqueue(resources->output_q, item);
            break;
        }

        if (!convert_raw_input_to_complex(config, resources, item)) {
            handle_thread_error("Processor", "Failed to convert samples due to unhandled format", resources);
            queue_enqueue(resources->free_pool_q, item);
            break;
        }

        pthread_mutex_lock(&resources->dsp_mutex);
        shift_apply(config, resources, item, SHIFT_STAGE_PRE_RESAMPLE);
        unsigned int output_frames_this_chunk = 0;
        if (!resample_block(config, resources, item, &output_frames_this_chunk)) {
            pthread_mutex_unlock(&resources->dsp_mutex);
            handle_thread_error("Processor", "Resampling failed", resources);
            queue_enqueue(resources->free_pool_q, item);
            break;
        }
        item->frames_to_write = output_frames_this_chunk;
        shift_apply(config, resources, item, SHIFT_STAGE_POST_RESAMPLE);
        pthread_mutex_unlock(&resources->dsp_mutex);
        convert_complex_to_output(config, resources, item, item->frames_to_write);
        if (!queue_enqueue(resources->output_q, item)) {
             break;
        }
    }
    return NULL;
}

static void* writer_thread_func(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    AppResources *resources = args->resources;
    AppConfig *config = args->config;
    int loop_count = 0;

    while (true) {
        WorkItem *item = (WorkItem*)queue_dequeue(resources->output_q);
        if (!item) break;
        if (item->is_last_chunk) {
            queue_enqueue(resources->free_pool_q, item);
            break;
        }
        size_t output_bytes_this_chunk = item->frames_to_write * resources->output_bytes_per_sample_pair;
        if (output_bytes_this_chunk > 0) {
            size_t written_bytes = resources->writer_ctx.ops.write(&resources->writer_ctx, item->output_buffer, output_bytes_this_chunk);
            
            if (written_bytes != output_bytes_this_chunk) {
                if (config->output_to_stdout) {
                    log_info("stdout pipe closed. Shutting down.");
                    handle_thread_error("Writer", "stdout pipe closed", resources);
                } else {
                    char error_buf[256];
                    snprintf(error_buf, sizeof(error_buf), "File write error: %s", strerror(errno));
                    handle_thread_error("Writer", error_buf, resources);
                }
                queue_enqueue(resources->free_pool_q, item);
                break;
            }
        }
        if (item->frames_to_write > 0) {
            pthread_mutex_lock(&resources->progress_mutex);
            resources->total_output_frames += item->frames_to_write;
            unsigned long long current_total_read = resources->total_frames_read;
            pthread_mutex_unlock(&resources->progress_mutex);
            
            loop_count++;
            if (!config->output_to_stdout && (loop_count % PROGRESS_UPDATE_INTERVAL == 0)) {
                // Call the progress callback function provided by main.c
                if (resources->progress_callback) {
                    resources->progress_callback(current_total_read,
                                                resources->source_info.frames,
                                                resources->total_output_frames,
                                                resources->progress_callback_udata);
                }
            }
        }
        if (!queue_enqueue(resources->free_pool_q, item)) {
             break;
        }
    }
    return NULL;
}
