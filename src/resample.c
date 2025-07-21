#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <limits.h>
#include <sndfile.h>
#include <pthread.h>

#ifdef _WIN32
#include <liquid.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
#else
#include <liquid/liquid.h>
#include <unistd.h>
#include <libgen.h>
#include <sys/stat.h>
#endif

#include "resample.h"
#include "setup.h"
#include "types.h"
#include "config.h"
#include "platform.h"
#include "utils.h"
#include "metadata.h"
#include "queue.h"

// --- Forward Declarations for Thread Functions ---
static void* reader_thread_func(void* arg);
static void* processor_thread_func(void* arg);
static void* writer_thread_func(void* arg);

// --- Forward Declarations for Static Helpers ---
static void convert_input_to_complex_mt(AppConfig *config, AppResources *resources, WorkItem *item);
static bool resample_block_mt(AppConfig *config, AppResources *resources, WorkItem *item, unsigned int *num_output_frames);
static void convert_complex_to_output_mt(AppConfig *config, AppResources *resources, WorkItem *item, unsigned int num_frames);
static void handle_thread_error(const char* thread_name, const char* error_msg, AppResources *resources);

// Struct to pass arguments to threads
typedef struct {
    AppConfig* config;
    AppResources* resources;
} ThreadArgs;


/**
 * @brief Signals that a fatal error has occurred in a thread, and triggers a shutdown.
 * @param thread_name The name of the thread where the error occurred.
 * @param error_msg The error message to display.
 * @param resources Pointer to the shared application resources.
 */
static void handle_thread_error(const char* thread_name, const char* error_msg, AppResources *resources) {
    fprintf(stderr, "\nFATAL ERROR in %s thread: %s\n", thread_name, error_msg);

    pthread_mutex_lock(&resources->progress_mutex);
    bool already_error = resources->error_occurred;
    resources->error_occurred = true;
    pthread_mutex_unlock(&resources->progress_mutex);

    // Only signal shutdown on the first error to avoid redundant signaling
    if (!already_error) {
        fprintf(stderr, "Signaling shutdown to other threads...\n");
        if (resources->input_q) queue_signal_shutdown(resources->input_q);
        if (resources->output_q) queue_signal_shutdown(resources->output_q);
        if (resources->free_pool_q) queue_signal_shutdown(resources->free_pool_q);
    }
}

/**
 * @brief Platform-independent helper to get the base filename from a path for metadata parsing.
 * @param config Pointer to the application configuration containing resolved paths.
 * @param buffer A buffer to store the resulting basename.
 * @param buffer_size The size of the buffer.
 * @return A pointer to the provided buffer on success, or NULL on failure.
 * @note On POSIX, this uses a temporary copy to avoid modifying the original path with basename().
 */
static const char* _get_basename_for_parsing(const AppConfig *config, char* buffer, size_t buffer_size) {
#ifdef _WIN32
    if (config->effective_input_filename_w) {
        const wchar_t* base_w = PathFindFileNameW(config->effective_input_filename_w);
        if (WideCharToMultiByte(CP_UTF8, 0, base_w, -1, buffer, buffer_size, NULL, NULL) > 0) {
            return buffer;
        }
    }
#else
    if (config->effective_input_filename) {
        char* temp_copy = strdup(config->effective_input_filename);
        if (temp_copy) {
            char* base = basename(temp_copy);
            strncpy(buffer, base, buffer_size - 1);
            buffer[buffer_size - 1] = '\0';
            free(temp_copy);
            return buffer;
        }
    }
#endif
    return NULL;
}


// --- Public Function Implementations ---

/**
 * @brief Initializes all application resources.
 */
bool initialize_resources(AppConfig *config, AppResources *resources) {
    if (!config || !resources) return false;

    init_sdr_metadata(&resources->sdr_info);
    bool success = false;
    float resample_ratio = 0.0f;

    if (!resolve_file_paths(config)) goto cleanup_init;
    if (!open_and_validate_input_file(config, resources)) goto cleanup_init;

    // --- Metadata Parsing ---
    resources->sdr_info_present = parse_sdr_metadata_chunks(resources->infile, &resources->sfinfo, &resources->sdr_info);
    char basename_buffer[MAX_PATH_LEN];
    const char* base_filename_to_parse = _get_basename_for_parsing(config, basename_buffer, sizeof(basename_buffer));
    if (base_filename_to_parse) {
        bool filename_parsed_something = parse_sdr_metadata_from_filename(base_filename_to_parse, &resources->sdr_info);
        resources->sdr_info_present = resources->sdr_info_present || filename_parsed_something;
    }
    // --- End Metadata Parsing ---

    if (!calculate_and_validate_resample_ratio(config, resources, &resample_ratio)) goto cleanup_init;
    if (!allocate_processing_buffers(config, resources, resample_ratio)) goto cleanup_init;

    // Initialize WorkItems by linking them to their segments in the memory pools
    size_t raw_size_per_item = BUFFER_SIZE_SAMPLES * 2 * resources->input_bytes_per_sample;
    size_t complex_elements_per_input_item = BUFFER_SIZE_SAMPLES;
    size_t complex_elements_per_output_item = resources->max_out_samples;
    bool shift_requested = config->freq_shift_requested || config->set_center_frequency_target_hz;
    size_t complex_elements_per_shifted_item = shift_requested ? (config->shift_after_resample ? complex_elements_per_output_item : complex_elements_per_input_item) : 0;
    size_t output_buffer_bytes_per_item = complex_elements_per_output_item * resources->output_bytes_per_sample_pair;

    for (size_t i = 0; i < NUM_BUFFERS; ++i) {
        WorkItem* item = &resources->work_item_pool[i];
        item->raw_input_buffer = (char*)resources->raw_input_pool + i * raw_size_per_item;
        item->complex_buffer_scaled = resources->complex_scaled_pool + i * complex_elements_per_input_item;
        item->complex_buffer_resampled = resources->complex_resampled_pool + i * complex_elements_per_output_item;
        item->output_buffer = (unsigned char*)resources->output_pool + i * output_buffer_bytes_per_item;
        if (shift_requested) {
             item->complex_buffer_shifted = resources->complex_shifted_pool + i * complex_elements_per_shifted_item;
        } else {
             item->complex_buffer_shifted = NULL;
        }
    }

    if (!create_dsp_components(config, resources, resample_ratio)) goto cleanup_init;

    // Create and populate the thread-safe queues
    resources->free_pool_q = queue_create(NUM_BUFFERS);
    resources->input_q = queue_create(NUM_BUFFERS);
    resources->output_q = queue_create(NUM_BUFFERS);
    if (!resources->free_pool_q || !resources->input_q || !resources->output_q) {
        fprintf(stderr, "Error: Failed to create one or more processing queues.\n");
        goto cleanup_init;
    }
    for (size_t i = 0; i < NUM_BUFFERS; ++i) {
        if (!queue_enqueue(resources->free_pool_q, &resources->work_item_pool[i])) {
             fprintf(stderr, "Error: Failed to initially populate free queue.\n");
             goto cleanup_init;
        }
    }

    // Initialize synchronization primitives
    if (pthread_mutex_init(&resources->progress_mutex, NULL) != 0) {
        perror("Failed to initialize progress mutex");
        goto cleanup_init;
    }
    resources->error_occurred = false;
    resources->total_frames_read = 0;
    resources->total_output_frames = 0;

    if (!config->output_to_stdout) {
        print_configuration_summary(config, resources, resample_ratio);
    }
    if (!check_nyquist_warning(config, resources)) goto cleanup_init;
    if (!prepare_output_stream(config, resources)) goto cleanup_init;

    success = true;

cleanup_init:
    if (!success) {
        cleanup_resources(config, resources);
    }
    return success;
}

/**
 * @brief Creates, runs, and joins the processing threads.
 */
bool run_processing_threads(AppConfig *config, AppResources *resources) {
    if (!config || !resources) return false;

    ThreadArgs thread_args = { .config = config, .resources = resources };

    if (pthread_create(&resources->reader_thread, NULL, reader_thread_func, &thread_args) != 0 ||
        pthread_create(&resources->processor_thread, NULL, processor_thread_func, &thread_args) != 0 ||
        pthread_create(&resources->writer_thread, NULL, writer_thread_func, &thread_args) != 0) {
        perror("Failed to create one or more threads");
        handle_thread_error("Main", "Thread creation failed", resources);
        // Attempt to join any threads that might have been created
        pthread_join(resources->reader_thread, NULL);
        pthread_join(resources->processor_thread, NULL);
        pthread_join(resources->writer_thread, NULL);
        return false;
    }

    // Wait for all threads to complete
    pthread_join(resources->reader_thread, NULL);
    pthread_join(resources->processor_thread, NULL);
    pthread_join(resources->writer_thread, NULL);

    return !resources->error_occurred;
}

/**
 * @brief Releases all allocated resources.
 */
void cleanup_resources(AppConfig *config, AppResources *resources) {
    if (!resources) return;

    // Determine final output size BEFORE closing the file handle/stream
    if (resources->outfile && resources->outfile != stdout) {
        fflush(resources->outfile);
        #ifdef _WIN32
            if (resources->h_outfile != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER file_size;
                if (GetFileSizeEx(resources->h_outfile, &file_size)) {
                    resources->final_output_size_bytes = file_size.QuadPart;
                }
            }
        #else
            if (config && config->effective_output_filename) {
                struct stat stat_buf;
                if (stat(config->effective_output_filename, &stat_buf) == 0) {
                    resources->final_output_size_bytes = stat_buf.st_size;
                }
            }
        #endif
    }

    // Queues
    if(resources->input_q) queue_destroy(resources->input_q);
    if(resources->output_q) queue_destroy(resources->output_q);
    if(resources->free_pool_q) queue_destroy(resources->free_pool_q);

    pthread_mutex_destroy(&resources->progress_mutex);

    // Free buffer pools
    free(resources->work_item_pool);
    free(resources->raw_input_pool);
    free(resources->complex_scaled_pool);
    free(resources->complex_shifted_pool);
    free(resources->complex_resampled_pool);
    free(resources->output_pool);

    // Destroy DSP components
    if(resources->shifter_nco) nco_crcf_destroy(resources->shifter_nco);
    if(resources->resampler) msresamp_crcf_destroy(resources->resampler);

    // Close files
    if (resources->infile) sf_close(resources->infile);
    if (resources->outfile && resources->outfile != stdout) {
        fclose(resources->outfile);
        #ifdef _WIN32
        // fclose on a stream from _fdopen also closes the underlying handle.
        // Invalidate our copy to prevent a double-close attempt.
        resources->h_outfile = INVALID_HANDLE_VALUE;
        #endif
    }

    // This is a fallback for the case where CreateFileW succeeded but _fdopen failed.
    #ifdef _WIN32
    if (resources->h_outfile != INVALID_HANDLE_VALUE) {
        CloseHandle(resources->h_outfile);
    }
    #endif

    // Free resolved paths allocated on Windows
    if (config) {
        #ifdef _WIN32
            free_absolute_path_windows(&config->effective_input_filename_w, &config->effective_input_filename_utf8);
            free_absolute_path_windows(&config->effective_output_filename_w, &config->effective_output_filename_utf8);
        #endif
    }
}


// --- Processing Step Helpers (Multi-threaded versions) ---

static void convert_input_to_complex_mt(AppConfig *config, AppResources *resources, WorkItem *item) {
    for (sf_count_t i = 0; i < item->frames_read; ++i) {
         float i_float = 0.0f, q_float = 0.0f;
         size_t base_idx = 2 * (size_t)i;

         if (resources->input_bit_depth == 16) {
              int16_t i_s16 = ((int16_t*)item->raw_input_buffer)[base_idx];
              int16_t q_s16 = ((int16_t*)item->raw_input_buffer)[base_idx + 1];
              i_float = (float)i_s16;
              q_float = (float)q_s16;
         } else { // 8-bit
              if (resources->input_format_subtype == SF_FORMAT_PCM_S8) {
                   int8_t i_s8 = ((int8_t*)item->raw_input_buffer)[base_idx];
                   int8_t q_s8 = ((int8_t*)item->raw_input_buffer)[base_idx + 1];
                   i_float = (float)i_s8 * SCALE_8_TO_16;
                   q_float = (float)q_s8 * SCALE_8_TO_16;
              } else { // SF_FORMAT_PCM_U8
                   uint8_t i_u8 = ((uint8_t*)item->raw_input_buffer)[base_idx];
                   uint8_t q_u8 = ((uint8_t*)item->raw_input_buffer)[base_idx + 1];
                   i_float = ((float)i_u8 - 127.5f) * SCALE_8_TO_16;
                   q_float = ((float)q_u8 - 127.5f) * SCALE_8_TO_16;
              }
         }
         item->complex_buffer_scaled[i] = (i_float * config->scale_value) + I * (q_float * config->scale_value);
    }
}

static bool resample_block_mt(AppConfig *config, AppResources *resources, WorkItem *item, unsigned int *num_output_frames) {
    if (item->frames_read <= 0) {
        *num_output_frames = 0;
        return true;
    }
    unsigned int nframes_uint = (unsigned int)item->frames_read;

    liquid_float_complex *resampler_input = (resources->shifter_nco != NULL && !config->shift_after_resample)
                                            ? item->complex_buffer_shifted
                                            : item->complex_buffer_scaled;

    msresamp_crcf_execute(resources->resampler, resampler_input, nframes_uint, item->complex_buffer_resampled, num_output_frames);

    if (*num_output_frames > resources->max_out_samples) {
         fprintf(stderr, "\nCRITICAL ERROR: Resampler buffer overflow detected!\n");
         return false;
    }
    return true;
}

static void convert_complex_to_output_mt(AppConfig *config, AppResources *resources, WorkItem *item, unsigned int num_frames) {
    if (num_frames == 0) return;

    liquid_float_complex *final_complex_data = (config->shift_after_resample && resources->shifter_nco != NULL)
                                               ? item->complex_buffer_shifted
                                               : item->complex_buffer_resampled;

    if (config->mode == MODE_CU8) {
        uint8_t *out_ptr_u8 = (uint8_t*)item->output_buffer;
        for (unsigned int i = 0; i < num_frames; ++i) {
            *out_ptr_u8++ = float_to_uchar(crealf(final_complex_data[i]));
            *out_ptr_u8++ = float_to_uchar(cimagf(final_complex_data[i]));
        }
    } else { // MODE_CS16_FM or MODE_CS16_AM
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


// --- Thread Functions ---

/**
 * @brief Reader thread: Reads raw data from file and puts it into a queue for the processor.
 */
static void* reader_thread_func(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    AppResources *resources = args->resources;

    unsigned long long seq_num = 0;
    size_t bytes_per_input_frame = 2 * resources->input_bytes_per_sample;
    size_t bytes_to_read_per_chunk = (size_t)BUFFER_SIZE_SAMPLES * bytes_per_input_frame;

    while (true) {
        if (resources->error_occurred) break;

        WorkItem *current_item = (WorkItem*)queue_dequeue(resources->free_pool_q);
        if (!current_item) break; // Shutdown signaled

        sf_count_t bytes_read = sf_read_raw(resources->infile, current_item->raw_input_buffer, bytes_to_read_per_chunk);
        if (bytes_read < 0) {
            char error_buf[256];
            snprintf(error_buf, sizeof(error_buf), "libsndfile read error: %s", sf_strerror(resources->infile));
            handle_thread_error("Reader", error_buf, resources);
            queue_enqueue(resources->free_pool_q, current_item);
            break;
        }

        current_item->frames_read = bytes_read / bytes_per_input_frame;
        current_item->sequence_number = seq_num++;
        current_item->is_last_chunk = (bytes_read == 0);

        if (!current_item->is_last_chunk) {
             pthread_mutex_lock(&resources->progress_mutex);
             resources->total_frames_read += current_item->frames_read;
             pthread_mutex_unlock(&resources->progress_mutex);
        }

        if (!queue_enqueue(resources->input_q, current_item)) {
             break; // Shutdown signaled
        }

        if (current_item->is_last_chunk) {
            break; // Normal EOF
        }
    }
    return NULL;
}

/**
 * @brief Processor thread: Processes data from the input queue and puts it into an output queue.
 */
static void* processor_thread_func(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    AppResources *resources = args->resources;
    AppConfig *config = args->config;

    while (true) {
        WorkItem *item = (WorkItem*)queue_dequeue(resources->input_q);
        if (!item) break; // Shutdown signaled

        if (item->is_last_chunk) {
            queue_enqueue(resources->output_q, item); // Propagate shutdown signal
            break;
        }

        convert_input_to_complex_mt(config, resources, item);

        if (!config->shift_after_resample && resources->shifter_nco != NULL) {
            unsigned int nframes_uint = (unsigned int)item->frames_read;
            if (resources->actual_nco_shift_hz >= 0) {
                nco_crcf_mix_block_up(resources->shifter_nco, item->complex_buffer_scaled, item->complex_buffer_shifted, nframes_uint);
            } else {
                nco_crcf_mix_block_down(resources->shifter_nco, item->complex_buffer_scaled, item->complex_buffer_shifted, nframes_uint);
            }
        }

        unsigned int output_frames_this_chunk = 0;
        if (!resample_block_mt(config, resources, item, &output_frames_this_chunk)) {
            handle_thread_error("Processor", "Resampling failed", resources);
            queue_enqueue(resources->free_pool_q, item);
            break;
        }
        item->frames_to_write = output_frames_this_chunk;

        if (config->shift_after_resample && resources->shifter_nco != NULL && item->frames_to_write > 0) {
            if (resources->actual_nco_shift_hz >= 0) {
                nco_crcf_mix_block_up(resources->shifter_nco, item->complex_buffer_resampled, item->complex_buffer_shifted, item->frames_to_write);
            } else {
                nco_crcf_mix_block_down(resources->shifter_nco, item->complex_buffer_resampled, item->complex_buffer_shifted, item->frames_to_write);
            }
        }

        convert_complex_to_output_mt(config, resources, item, item->frames_to_write);

        if (!queue_enqueue(resources->output_q, item)) {
             break; // Shutdown signaled
        }
    }
    return NULL;
}

/**
 * @brief Writer thread: Writes processed data from the output queue to the destination file/stdout.
 */
static void* writer_thread_func(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    AppResources *resources = args->resources;
    AppConfig *config = args->config;
    FILE* out_stream = resources->outfile;
    int loop_count = 0;

    while (true) {
        WorkItem *item = (WorkItem*)queue_dequeue(resources->output_q);
        if (!item) break; // Shutdown signaled

        if (item->is_last_chunk) {
            queue_enqueue(resources->free_pool_q, item); // Return marker item to pool
            break;
        }

        size_t output_bytes_this_chunk = item->frames_to_write * resources->output_bytes_per_sample_pair;
        if (output_bytes_this_chunk > 0) {
            errno = 0;
            size_t written_bytes = fwrite(item->output_buffer, 1, output_bytes_this_chunk, out_stream);
            int write_errno = errno;

            if (written_bytes != output_bytes_this_chunk) {
                // A "broken pipe" error is not a fatal application error when writing to stdout.
                // It simply means the downstream program (like nrsc5) has closed its input pipe.
                // We treat this as a signal to terminate gracefully and silently.
                #ifdef EPIPE
                if (config->output_to_stdout && write_errno == EPIPE) {
                    queue_enqueue(resources->free_pool_q, item);
                    break; // Exit silently on broken pipe
                }
                #endif
                char error_buf[256];
                snprintf(error_buf, sizeof(error_buf), "fwrite error: %s", strerror(write_errno));
                handle_thread_error("Writer", error_buf, resources);
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
                double percentage_complete = (resources->sfinfo.frames > 0)
                    ? ((double)current_total_read / (double)resources->sfinfo.frames) * 100.0
                    : 0.0;
                if (percentage_complete > 100.0) percentage_complete = 100.0;
                 fprintf(stderr, "\rProcessed %llu / %llu input frames (%.1f%%)...",
                        current_total_read,
                        (unsigned long long)resources->sfinfo.frames,
                        percentage_complete);
            }
        }

        if (!queue_enqueue(resources->free_pool_q, item)) {
             break; // Shutdown signaled
        }
    }

    if (!config->output_to_stdout) {
        fflush(stderr);
    }
    if (out_stream) {
        fflush(out_stream);
    }
    return NULL;
}
