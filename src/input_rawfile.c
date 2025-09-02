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
#include "file_write_buffer.h"
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
    OPT_FLOAT(0, "raw-file-input-rate", &s_rawfile_config.raw_file_sample_rate_hz_arg, "(Required) The sample rate of the raw input file.", NULL, 0, 0),
    OPT_STRING(0, "raw-file-input-sample-format", &s_rawfile_config.format_str, "(Required) The sample format of the raw input file.", NULL, 0, 0),
};

const struct argparse_option* rawfile_get_cli_options(int* count) {
    *count = sizeof(rawfile_cli_options) / sizeof(rawfile_cli_options[0]);
    return rawfile_cli_options;
}

static bool rawfile_initialize(InputSourceContext* ctx);
static void* rawfile_start_stream(InputSourceContext* ctx);
static void rawfile_stop_stream(InputSourceContext* ctx);
static void rawfile_cleanup(InputSourceContext* ctx);
static void rawfile_get_summary_info(const InputSourceContext* ctx, InputSummaryInfo* info);
static bool rawfile_validate_options(AppConfig* config);
static bool rawfile_pre_stream_iq_correction(InputSourceContext* ctx);

static InputSourceOps raw_file_ops = {
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

InputSourceOps* get_raw_file_input_ops(void) {
    return &raw_file_ops;
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

static bool rawfile_initialize(InputSourceContext* ctx) {
    const AppConfig *config = ctx->config;
    AppResources *resources = ctx->resources;

    RawfilePrivateData* private_data = (RawfilePrivateData*)mem_arena_alloc(&resources->setup_arena, sizeof(RawfilePrivateData), true);
    if (!private_data) {
        return false;
    }
    resources->input_module_private_data = private_data;

    resources->input_format = utils_get_format_from_string(s_rawfile_config.format_str);
    if (resources->input_format == FORMAT_UNKNOWN) {
        log_fatal("Invalid raw input format '%s'. See --help for valid formats.", s_rawfile_config.format_str);
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
    private_data->infile = sf_wchar_open(config->effective_input_filename_w, SFM_READ, &sfinfo);
#else
    private_data->infile = sf_open(config->effective_input_filename, SFM_READ, &sfinfo);
#endif

    if (!private_data->infile) {
        log_fatal("Error opening raw input file '%s': %s", config->input_filename_arg, sf_strerror(NULL));
        return false;
    }

    sf_command(private_data->infile, SFC_GET_CURRENT_SF_INFO, &sfinfo, sizeof(sfinfo));
    resources->source_info.samplerate = sfinfo.samplerate;
    resources->source_info.frames = sfinfo.frames;

    log_info("Opened raw file with format %s, rate %.0f Hz, and %lld frames.",
             s_rawfile_config.format_str, (double)resources->source_info.samplerate, (long long)resources->source_info.frames);

    return true;
}

static void* rawfile_start_stream(InputSourceContext* ctx) {
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

    while (!is_shutdown_requested() && !resources->error_occurred) {
        SampleChunk *current_item = (SampleChunk*)queue_dequeue(resources->free_sample_chunk_queue);
        if (!current_item) {
            break; // Shutdown or error signaled
        }

        current_item->stream_discontinuity_event = false;

        void* target_buffer = config->raw_passthrough ? current_item->final_output_data : current_item->raw_input_data;
        size_t bytes_to_read = config->raw_passthrough ? current_item->final_output_capacity_bytes : current_item->raw_input_capacity_bytes;
        
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
            // End of file. Send a final chunk with the is_last_chunk flag set.
            current_item->is_last_chunk = true;
            current_item->frames_read = 0;
            current_item->packet_sample_format = resources->input_format;
            if (config->raw_passthrough) {
                // In passthrough, we also need to signal the writer thread directly.
                file_write_buffer_signal_end_of_stream(resources->file_write_buffer);
                queue_enqueue(resources->free_sample_chunk_queue, current_item);
            } else {
                queue_enqueue(resources->raw_to_pre_process_queue, current_item);
            }
            break; 
        }

        current_item->frames_read = bytes_read / resources->input_bytes_per_sample_pair;
        current_item->packet_sample_format = resources->input_format;
        current_item->is_last_chunk = false;
        
        pthread_mutex_lock(&resources->progress_mutex);
        resources->total_frames_read += current_item->frames_read;
        pthread_mutex_unlock(&resources->progress_mutex);

        if (config->raw_passthrough) {
            size_t bytes_written = file_write_buffer_write(resources->file_write_buffer, target_buffer, bytes_read);
            if (bytes_written < (size_t)bytes_read) {
                log_warn("I/O buffer overrun! Dropped %zu bytes.", (size_t)bytes_read - bytes_written);
            }
            queue_enqueue(resources->free_sample_chunk_queue, current_item);
        } else {
            if (!queue_enqueue(resources->raw_to_pre_process_queue, current_item)) {
                queue_enqueue(resources->free_sample_chunk_queue, current_item);
                break;
            }
        }
    }

    return NULL;
}

static void rawfile_stop_stream(InputSourceContext* ctx) {
    (void)ctx;
}

static void rawfile_cleanup(InputSourceContext* ctx) {
    AppResources *resources = ctx->resources;
    if (resources->input_module_private_data) {
        RawfilePrivateData* private_data = (RawfilePrivateData*)resources->input_module_private_data;
        if (private_data->infile) {
            log_info("Closing raw input file.");
            sf_close(private_data->infile);
            private_data->infile = NULL;
        }
        resources->input_module_private_data = NULL;
    }
}

static void rawfile_get_summary_info(const InputSourceContext* ctx, InputSummaryInfo* info) {
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

static bool rawfile_pre_stream_iq_correction(InputSourceContext* ctx) {
    AppConfig* config = (AppConfig*)ctx->config;
    RawfilePrivateData* private_data = (RawfilePrivateData*)ctx->resources->input_module_private_data;

    // This routine is only necessary if I/Q correction is enabled.
    if (!config->iq_correction.enable) {
        return true;
    }
    
    // The module's only job is to call the calibration service with its private file handle.
    return iq_correct_run_initial_calibration(ctx, private_data->infile);
}
