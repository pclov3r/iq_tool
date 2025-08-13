// src/input_rawfile.c

#include "input_rawfile.h"
#include "log.h"
#include "signal_handler.h"
#include "utils.h"
#include "config.h"
#include "platform.h"
#include "sample_convert.h" // For get_bytes_per_sample
#include "input_common.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sndfile.h>
#include <pthread.h>
#include <sys/stat.h>
#include <stdarg.h>
#include "argparse.h"

#ifndef _WIN32
#include <strings.h>
#endif

#ifdef _WIN32
#define strcasecmp _stricmp
#endif

// --- Add an external declaration for the global config ---
extern AppConfig g_config;

// --- Define the CLI options for this module ---
static const struct argparse_option rawfile_cli_options[] = {
    OPT_GROUP("Raw File Input Options"),
    OPT_FLOAT(0, "raw-file-input-rate", &g_config.raw_file.raw_file_sample_rate_hz_arg, "(Required) The sample rate of the raw input file.", NULL, 0, 0),
    OPT_STRING(0, "raw-file-input-sample-format", &g_config.raw_file.format_str, "(Required) The sample format of the raw input file.", NULL, 0, 0),
};

// --- Implement the interface function to provide the options ---
const struct argparse_option* rawfile_get_cli_options(int* count) {
    *count = sizeof(rawfile_cli_options) / sizeof(rawfile_cli_options[0]);
    return rawfile_cli_options;
}


// --- Forward Declarations for Static Functions ---
static bool rawfile_initialize(InputSourceContext* ctx);
static void* rawfile_start_stream(InputSourceContext* ctx);
static void rawfile_stop_stream(InputSourceContext* ctx);
static void rawfile_cleanup(InputSourceContext* ctx);
static void rawfile_get_summary_info(const InputSourceContext* ctx, InputSummaryInfo* info);
static bool rawfile_validate_options(AppConfig* config);
static format_t get_format_from_string(const char *name);


// --- The single instance of InputSourceOps for raw files ---
static InputSourceOps raw_file_ops = {
    .initialize = rawfile_initialize,
    .start_stream = rawfile_start_stream,
    .stop_stream = rawfile_stop_stream,
    .cleanup = rawfile_cleanup,
    .get_summary_info = rawfile_get_summary_info,
    .validate_options = rawfile_validate_options,
    .has_known_length = _input_source_has_known_length_true
};

InputSourceOps* get_raw_file_input_ops(void) {
    return &raw_file_ops;
}

// --- Module-specific validation function ---
static bool rawfile_validate_options(AppConfig* config) {
    // This function is only called if "raw-file" is the selected input.
    
    if (config->raw_file.raw_file_sample_rate_hz_arg > 0.0f) {
        config->raw_file.sample_rate_hz = (double)config->raw_file.raw_file_sample_rate_hz_arg;
        config->raw_file.sample_rate_provided = true;
    }

    bool format_provided = config->raw_file.format_str != NULL;

    if (!config->raw_file.sample_rate_provided) {
        log_fatal("Missing required option --raw-file-input-rate <hz> for raw file input.");
        return false;
    }
    if (!format_provided) {
        log_fatal("Missing required option --raw-file-input-sample-format <format> for raw file input.");
        return false;
    }

    config->raw_file.format_provided = true;

    return true;
}


// --- Implementations of the InputSourceOps functions ---

static bool rawfile_initialize(InputSourceContext* ctx) {
    const AppConfig *config = ctx->config;
    AppResources *resources = ctx->resources;

    resources->input_format = get_format_from_string(config->raw_file.format_str);
    if (resources->input_format == FORMAT_UNKNOWN) {
        log_fatal("Invalid raw input format '%s'. See --help for valid formats.", config->raw_file.format_str);
        return false;
    }

    resources->input_bytes_per_sample_pair = get_bytes_per_sample(resources->input_format);
    if (resources->input_bytes_per_sample_pair == 0) {
        log_fatal("Internal error: could not determine sample size for format '%s'.", config->raw_file.format_str);
        return false;
    }

    SF_INFO sfinfo;
    memset(&sfinfo, 0, sizeof(SF_INFO));
    sfinfo.samplerate = (int)config->raw_file.sample_rate_hz;
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
    resources->infile = sf_wchar_open(config->effective_input_filename_w, SFM_READ, &sfinfo);
#else
    resources->infile = sf_open(config->effective_input_filename, SFM_READ, &sfinfo);
#endif

    if (!resources->infile) {
        log_fatal("Error opening raw input file '%s': %s", config->input_filename_arg, sf_strerror(NULL));
        return false;
    }

    sf_command(resources->infile, SFC_GET_CURRENT_SF_INFO, &sfinfo, sizeof(sfinfo));
    resources->source_info.samplerate = sfinfo.samplerate;
    resources->source_info.frames = sfinfo.frames;

    log_info("Opened raw file with format %s, rate %.0f Hz, and %lld frames.",
             config->raw_file.format_str, (double)resources->source_info.samplerate, (long long)resources->source_info.frames);

    return true;
}

static void* rawfile_start_stream(InputSourceContext* ctx) {
    AppResources *resources = ctx->resources;
    const AppConfig *config = ctx->config;
    size_t bytes_to_read_per_chunk = (size_t)BUFFER_SIZE_SAMPLES * resources->input_bytes_per_sample_pair;

    if (config->no_convert && resources->input_format != config->output_format) {
        char error_buf[256];
        snprintf(error_buf, sizeof(error_buf),
                 "Option --no-convert requires input and output formats to be identical. Input format is '%s', output format is '%s'.",
                 config->raw_file.format_str, config->sample_type_name);
        handle_fatal_thread_error(error_buf, resources);
        return NULL;
    }

    while (!is_shutdown_requested() && !resources->error_occurred) {
        SampleChunk *current_item = (SampleChunk*)queue_dequeue(resources->free_sample_chunk_queue);
        if (!current_item) {
            break;
        }

        current_item->stream_discontinuity_event = false;

        void* target_buffer = config->no_convert ? current_item->final_output_data : current_item->raw_input_data;
        int64_t bytes_read = sf_read_raw(resources->infile, target_buffer, bytes_to_read_per_chunk);

        if (bytes_read < 0) {
            log_fatal("libsndfile read error: %s", sf_strerror(resources->infile));
            pthread_mutex_lock(&resources->progress_mutex);
            resources->error_occurred = true;
            pthread_mutex_unlock(&resources->progress_mutex);
            request_shutdown();
            queue_enqueue(resources->free_sample_chunk_queue, current_item);
            break;
        }

        current_item->frames_read = bytes_read / resources->input_bytes_per_sample_pair;
        current_item->is_last_chunk = (current_item->frames_read == 0);

        if (!current_item->is_last_chunk) {
            pthread_mutex_lock(&resources->progress_mutex);
            resources->total_frames_read += current_item->frames_read;
            pthread_mutex_unlock(&resources->progress_mutex);
        }

        if (config->no_convert) {
            // <<< CORRECTED: Changed 'item' to 'current_item' >>>
            current_item->frames_to_write = current_item->frames_read;
            if (!queue_enqueue(resources->final_output_queue, current_item)) {
                queue_enqueue(resources->free_sample_chunk_queue, current_item);
                break;
            }
        } else {
            if (!queue_enqueue(resources->raw_to_pre_process_queue, current_item)) {
                queue_enqueue(resources->free_sample_chunk_queue, current_item);
                break;
            }
        }

        if (current_item->is_last_chunk) {
            break;
        }
    }
    return NULL;
}

static void rawfile_stop_stream(InputSourceContext* ctx) {
    (void)ctx;
}

static void rawfile_cleanup(InputSourceContext* ctx) {
    AppResources *resources = ctx->resources;
    if (resources->infile) {
        log_info("Closing raw input file.");
        sf_close(resources->infile);
        resources->infile = NULL;
    }
}

static void rawfile_get_summary_info(const InputSourceContext* ctx, InputSummaryInfo* info) {
    const AppConfig *config = ctx->config;
    const AppResources *resources = ctx->resources;
    const char* display_path = config->input_filename_arg;
#ifdef _WIN32
    if (config->effective_input_filename_utf8) {
        display_path = config->effective_input_filename_utf8;
    }
#endif

    add_summary_item(info, "Input File", "%s", display_path);
    add_summary_item(info, "Input Type", "RAW FILE");
    add_summary_item(info, "Input Format", "%s", config->raw_file.format_str);
    add_summary_item(info, "Input Rate", "%.0f Hz", config->raw_file.sample_rate_hz);

    char size_buf[40];
    long long file_size_bytes = resources->source_info.frames * resources->input_bytes_per_sample_pair;
    add_summary_item(info, "Input File Size", "%s", format_file_size(file_size_bytes, size_buf, sizeof(size_buf)));
}

static format_t get_format_from_string(const char *name) {
    if (strcasecmp(name, "s8") == 0) return S8;
    if (strcasecmp(name, "u8") == 0) return U8;
    if (strcasecmp(name, "s16") == 0) return S16;
    if (strcasecmp(name, "u16") == 0) return U16;
    if (strcasecmp(name, "s32") == 0) return S32;
    if (strcasecmp(name, "u32") == 0) return U32;
    if (strcasecmp(name, "f32") == 0) return F32;
    if (strcasecmp(name, "cs8") == 0) return CS8;
    if (strcasecmp(name, "cu8") == 0) return CU8;
    if (strcasecmp(name, "cs16") == 0) return CS16;
    if (strcasecmp(name, "cu16") == 0) return CU16;
    if (strcasecmp(name, "cs32") == 0) return CS32;
    if (strcasecmp(name, "cu32") == 0) return CU32;
    if (strcasecmp(name, "cf32") == 0) return CF32;
    if (strcasecmp(name, "sc16q11") == 0) return SC16Q11;
    return FORMAT_UNKNOWN;
}
