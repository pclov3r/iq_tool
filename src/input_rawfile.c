#include "input_rawfile.h"
#include "constants.h"
#include "log.h"
#include "signal_handler.h"
#include "utils.h"
#include "app_context.h"
#include "platform.h"
#include "sample_convert.h"
#include "input_common.h"
#include "memory_arena.h"
#include "queue.h"
#include "ring_buffer.h"
#include "argparse.h"
#include "iq_correct.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <stdarg.h>

#include <sndfile.h>

#ifndef _WIN32
#include <strings.h>
#include <unistd.h> // For usleep
#else
#include <windows.h> // For Sleep
#endif

#ifdef _WIN32
#define strcasecmp _stricmp
#endif

extern AppConfig g_config;

static struct {
    double sample_rate_hz;
    float raw_file_sample_rate_hz_arg;
    bool sample_rate_provided;
    char *format_str;
    bool format_provided;
} s_rawfile_config;

// This is the private data structure for the Raw File input module.
typedef struct {
    SNDFILE *infile;
} RawfilePrivateData;

static const struct argparse_option rawfile_cli_options[] = {
    OPT_GROUP("Raw File Input Options"),
    OPT_FLOAT(0, "raw-file-input-rate", &s_rawfile_config.raw_file_sample_rate_hz_arg, "(Required) The sample rate of the RAW input file.", NULL, 0, 0),
    OPT_STRING(0, "raw-file-input-sample-format", &s_rawfile_config.format_str, "(Required) The sample format of the RAW input file.", NULL, 0, 0),
};

const struct argparse_option* rawfile_get_cli_options(int* count) {
    *count = sizeof(rawfile_cli_options) / sizeof(rawfile_cli_options[0]);
    return rawfile_cli_options;
}

static bool rawfile_initialize(ModuleContext* ctx);
static void* rawfile_start_stream(ModuleContext* ctx);
static void rawfile_stop_stream(ModuleContext* ctx);
static void rawfile_cleanup(ModuleContext* ctx);
static void rawfile_get_summary_info(const ModuleContext* ctx, InputSummaryInfo* info);
static bool rawfile_validate_options(AppConfig* config);
static bool rawfile_pre_stream_iq_correction(ModuleContext* ctx);

static ModuleApi raw_file_module_api = {
    .initialize = rawfile_initialize,
    .start_stream = rawfile_start_stream,
    .stop_stream = rawfile_stop_stream,
    .cleanup = rawfile_cleanup,
    .get_summary_info = rawfile_get_summary_info,
    .validate_options = rawfile_validate_options,
    .has_known_length = _input_source_has_known_length_true,
    .validate_generic_options = NULL,
    .pre_stream_iq_correction = rawfile_pre_stream_iq_correction,
};

ModuleApi* get_raw_file_input_module_api(void) {
    return &raw_file_module_api;
}

static bool rawfile_validate_options(AppConfig* config) {
    (void)config;
    if (s_rawfile_config.raw_file_sample_rate_hz_arg > 0.0f) {
        s_rawfile_config.sample_rate_hz = (double)s_rawfile_config.raw_file_sample_rate_hz_arg;
        s_rawfile_config.sample_rate_provided = true;
    }

    bool format_provided = s_rawfile_config.format_str != NULL;

    if (!s_rawfile_config.sample_rate_provided) {
        log_fatal("Missing required option --raw-file-input-rate <hz> for raw file input.");
        return false;
    }
    if (!format_provided) {
        log_fatal("Missing required option --raw-file-input-sample-format <format> for raw file input.");
        return false;
    }

    s_rawfile_config.format_provided = true;
    return true;
}

static bool rawfile_initialize(ModuleContext* ctx) {
    const AppConfig *config = ctx->config;
    AppResources *resources = ctx->resources;

    RawfilePrivateData* private_data = (RawfilePrivateData*)mem_arena_alloc(&resources->setup_arena, sizeof(RawfilePrivateData), true);
    if (!private_data) {
        return false;
    }
    resources->input_module_private_data = private_data;

    resources->input_format = utils_get_format_from_string(s_rawfile_config.format_str);
    if (resources->input_format == FORMAT_UNKNOWN) {
        log_fatal("Invalid RAW input format '%s'. See --help for valid formats.", s_rawfile_config.format_str);
        return false;
    }

    resources->input_bytes_per_sample_pair = get_bytes_per_sample(resources->input_format);
    if (resources->input_bytes_per_sample_pair == 0) {
        log_fatal("Internal error: could not determine sample size for format '%s'.", s_rawfile_config.format_str);
        return false;
    }

    SF_INFO sfinfo;
    memset(&sfinfo, 0, sizeof(SF_INFO));
    sfinfo.samplerate = (int)s_rawfile_config.sample_rate_hz;
    sfinfo.channels = 2;
    int format_code = SF_FORMAT_RAW;
    switch (resources->input_format) {
        case SC16Q11:
        case CS16: format_code |= SF_FORMAT_PCM_16; break;
        case CU16: format_code |= SF_FORMAT_PCM_16; break;
        case CS8:  format_code |= SF_FORMAT_PCM_S8; break;
        case CU8:  format_code |= SF_FORMAT_PCM_U8; break;
        case CS32: format_code |= SF_FORMAT_PCM_32; break;
        case CU32: format_code |= SF_FORMAT_PCM_32; break;
        case CF32: format_code |= SF_FORMAT_FLOAT;  break;
        default:
            log_fatal("Internal error: unhandled format enum in rawfile_initialize.");
            return false;
    }
    sfinfo.format = format_code;

#ifdef _WIN32
    log_info("Opening RAW input file: %s", config->effective_input_filename_utf8);
    private_data->infile = sf_wchar_open(config->effective_input_filename_w, SFM_READ, &sfinfo);
#else
    log_info("Opening RAW input file: %s", config->effective_input_filename);
    private_data->infile = sf_open(config->effective_input_filename, SFM_READ, &sfinfo);
#endif

    if (!private_data->infile) {
        log_fatal("Error opening RAW input file '%s': %s", config->input_filename_arg, sf_strerror(NULL));
        return false;
    }

    sf_command(private_data->infile, SFC_GET_CURRENT_SF_INFO, &sfinfo, sizeof(sfinfo));
    resources->source_info.samplerate = sfinfo.samplerate;
    resources->source_info.frames = sfinfo.frames;

    return true;
}

static void* rawfile_start_stream(ModuleContext* ctx) {
    AppResources *resources = ctx->resources;
    const AppConfig *config = ctx->config;
    RawfilePrivateData* private_data = (RawfilePrivateData*)resources->input_module_private_data;

    if (config->raw_passthrough && resources->input_format != config->output_format) {
        char error_buf[256];
        snprintf(error_buf, sizeof(error_buf),
                 "Option --raw-passthrough requires input and output formats to be identical. Input format is '%s', output format is '%s'.",
                 s_rawfile_config.format_str, config->sample_type_name);
        handle_fatal_thread_error(error_buf, resources);
        return NULL;
    }

    // Pre-calculate the back-pressure threshold in bytes for efficiency.
    const size_t writer_buffer_capacity = ring_buffer_get_capacity(resources->writer_input_buffer);
    const size_t writer_buffer_threshold = (size_t)(writer_buffer_capacity * IO_WRITER_BUFFER_HIGH_WATER_MARK);

    while (!is_shutdown_requested() && !resources->error_occurred) {
        if (ring_buffer_get_size(resources->writer_input_buffer) > writer_buffer_threshold) {
            // The writer is falling behind. Pause briefly to let it catch up.
            #ifdef _WIN32
                Sleep(10); // 10 ms
            #else
                usleep(10000); // 10 ms
            #endif
            continue; // Re-evaluate the buffer state in the next loop iteration.
        }

        SampleChunk *current_item = (SampleChunk*)queue_dequeue(resources->free_sample_chunk_queue);
        if (!current_item) {
            break; // Shutdown or error signaled
        }

        current_item->stream_discontinuity_event = false;

        // In passthrough mode, the reader thread's job is to read directly into the
        // *final* output buffer of the chunk. The processing threads will then just
        // act as a simple relay to pass the chunk to the writer.
        void* target_buffer;
        size_t bytes_to_read;
        if (config->raw_passthrough) {
            target_buffer = current_item->final_output_data;
            bytes_to_read = current_item->final_output_capacity_bytes;
        } else {
            target_buffer = current_item->raw_input_data;
            bytes_to_read = current_item->raw_input_capacity_bytes;
        }

        int64_t bytes_read = sf_read_raw(private_data->infile, target_buffer, bytes_to_read);

        if (bytes_read < 0) {
            log_fatal("libsndfile read error: %s", sf_strerror(private_data->infile));
            pthread_mutex_lock(&resources->progress_mutex);
            resources->error_occurred = true;
            pthread_mutex_unlock(&resources->progress_mutex);
            request_shutdown();
            queue_enqueue(resources->free_sample_chunk_queue, current_item);
            break;
        }

        if (bytes_read == 0) {
            // End of file. ALWAYS send a final "last chunk" marker down the pipeline.
            current_item->is_last_chunk = true;
            current_item->frames_read = 0;
            current_item->packet_sample_format = resources->input_format;

            // The passthrough logic will happen in the processing threads now.
            // We just need to start the shutdown sequence for all threads.
            queue_enqueue(resources->reader_output_queue, current_item);

            break;
        }

        current_item->frames_read = bytes_read / resources->input_bytes_per_sample_pair;
        current_item->packet_sample_format = resources->input_format;
        current_item->is_last_chunk = false;

        pthread_mutex_lock(&resources->progress_mutex);
        resources->total_frames_read += current_item->frames_read;
        pthread_mutex_unlock(&resources->progress_mutex);

        // In both modes, the chunk now goes to the pre-processor.
        // In passthrough mode, the pre-processor will just forward it.
        if (!queue_enqueue(resources->reader_output_queue, current_item)) {
            queue_enqueue(resources->free_sample_chunk_queue, current_item);
            break;
        }
    }

    return NULL;
}

static void rawfile_stop_stream(ModuleContext* ctx) {
    (void)ctx;
}

static void rawfile_cleanup(ModuleContext* ctx) {
    AppResources *resources = ctx->resources;
    if (resources->input_module_private_data) {
        RawfilePrivateData* private_data = (RawfilePrivateData*)resources->input_module_private_data;
        if (private_data->infile) {
            log_info("Closing RAW input file.");
            sf_close(private_data->infile);
            private_data->infile = NULL;
        }
        resources->input_module_private_data = NULL;
    }
}

static void rawfile_get_summary_info(const ModuleContext* ctx, InputSummaryInfo* info) {
    const AppConfig *config = ctx->config;
    const AppResources *resources = ctx->resources;
    const char* display_path = config->input_filename_arg;
#ifdef _WIN32
    if (config->effective_input_filename_utf8[0] != '\0') {
        display_path = config->effective_input_filename_utf8;
    }
#endif

    add_summary_item(info, "Input File", "%s", display_path);
    add_summary_item(info, "Input Type", "RAW FILE");
    add_summary_item(info, "Input Format", "%s", s_rawfile_config.format_str);
    add_summary_item(info, "Input Rate", "%.0f Hz", s_rawfile_config.sample_rate_hz);

    char size_buf[40];
    long long file_size_bytes = resources->source_info.frames * resources->input_bytes_per_sample_pair;
    add_summary_item(info, "Input File Size", "%s", format_file_size(file_size_bytes, size_buf, sizeof(size_buf)));
}

static bool rawfile_pre_stream_iq_correction(ModuleContext* ctx) {
    AppConfig* config = (AppConfig*)ctx->config;
    RawfilePrivateData* private_data = (RawfilePrivateData*)ctx->resources->input_module_private_data;

    // This routine is only necessary if I/Q correction is enabled.
    if (!config->iq_correction.enable) {
        return true;
    }

    // The module's only job is to call the calibration service with its private file handle.
    return iq_correct_run_initial_calibration(ctx, private_data->infile);
}
