/**
 * @file input_rawfile.c
 */

#include "input_rawfile.h"
#include "constants.h"
#include "log.h"
#include "signal_handler.h"
#include "utilities.h"
#include "sample_format_table.h"
#include "app_context.h"
#include "platform.h"
#include "input_common.h"
#include "mem_arena.h"
#include "queue.h"
#include "ring_buffer.h"
#include "argparse.h"
#include "iq_correction.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <sndfile.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

static struct {
    double sample_rate_hz;
    float raw_file_sample_rate_hz_arg;
    bool sample_rate_provided;
    char *format_str;
    bool format_provided;
    bool repeat_enabled; // Loop state
} s_rawfile_config = {
    .sample_rate_hz = 0.0,
    .raw_file_sample_rate_hz_arg = 0.0f,
    .sample_rate_provided = false,
    .format_str = NULL,
    .format_provided = false,
    .repeat_enabled = false
};

// This is the private data structure for the Raw File input module.
typedef struct {
    SNDFILE *infile;
    SF_INFO sfinfo;
    bool repeat_enabled; // Loop state
} RawfileInputContext;

static const struct argparse_option rawfile_input_cli_options[] = {
    OPT_GROUP("Raw File Input (rawfile)"),
    OPT_FLOAT(0, "rawfile-input-sample-rate", &s_rawfile_config.raw_file_sample_rate_hz_arg, "(Required) The sample rate of the RAW input file.", NULL, 0, 0),
    OPT_STRING(0, "rawfile-input-sample-format", &s_rawfile_config.format_str, "(Required) The sample format of the RAW input file.", NULL, 0, 0),
    OPT_BOOLEAN(0, "rawfile-repeat", &s_rawfile_config.repeat_enabled, "Loop the RAW input file.", NULL, 0, 0),
};

const struct argparse_option* rawfile_input_get_cli_options(int* count) {
    *count = sizeof(rawfile_input_cli_options) / sizeof(rawfile_input_cli_options[0]);
    return rawfile_input_cli_options;
}

static void rawfile_input_get_summary_info(const ModuleContext* context, InputSummaryInfo* info);
static bool rawfile_input_validate_options(AppContext* app);
static bool rawfile_input_pre_stream_iq_correction(ModuleContext* context);

static bool rawfile_input_validate_options(AppContext* app) {
    AppConfig* config = app ? (AppConfig*)app->config : NULL;
    if (s_rawfile_config.raw_file_sample_rate_hz_arg > 0.0f) {
        s_rawfile_config.sample_rate_hz = (double)s_rawfile_config.raw_file_sample_rate_hz_arg;
        s_rawfile_config.sample_rate_provided = true;
    }

    bool format_provided = s_rawfile_config.format_str != NULL;

    if (!s_rawfile_config.sample_rate_provided) {
        log_error("Missing required option --rawfile-input-rate <hz> for raw file input.");
        return false;
    }
    if (!format_provided) {
        log_error("Missing required option --rawfile-input-sample-format <format> for raw file input.");
        return false;
    }

    s_rawfile_config.format_provided = true;

    // Fail early logic
#ifdef _WIN32
    if (!config || config->input.effective_path_utf8[0] == '\0') {
#else
    if (!config || !config->input.effective_path || config->input.effective_path[0] == '\0') {
#endif
        log_error("RAW file input requires an input file path.");
        return false;
    }

    RawfileInputContext* private_data = (RawfileInputContext*)mem_arena_alloc(&app->pipeline.setup_arena, sizeof(RawfileInputContext), true);
    if (!private_data) return false;
    app->module.input_private_data = private_data;

    SampleFormat format_enum = get_format_info_by_name(s_rawfile_config.format_str) ? get_format_info_by_name(s_rawfile_config.format_str)->format_enum : FORMAT_UNKNOWN;
    if (format_enum == FORMAT_UNKNOWN) {
        log_error("Invalid RAW input format '%s'. See --help for valid formats.", s_rawfile_config.format_str);
        return false;
    }

    SF_INFO sfinfo;
    memset(&sfinfo, 0, sizeof(SF_INFO));
    sfinfo.samplerate = (int)s_rawfile_config.sample_rate_hz;
    sfinfo.channels = 2;
    int format_code = SF_FORMAT_RAW;
    switch (format_enum) {
        case SC16Q11:
        case CS16: format_code |= SF_FORMAT_PCM_16; break;
        case CU16: format_code |= SF_FORMAT_PCM_16; break;
        case CS8:  format_code |= SF_FORMAT_PCM_S8; break;
        case CU8:  format_code |= SF_FORMAT_PCM_U8; break;
        case CS24: format_code |= SF_FORMAT_PCM_24; break;
        case CS32: format_code |= SF_FORMAT_PCM_32; break;
        case CU32: format_code |= SF_FORMAT_PCM_32; break;
        case CF32: format_code |= SF_FORMAT_FLOAT;  break;
        default:
            log_fatal("Internal error: unhandled format enum in rawfile_input_validate_options.");
            return false;
    }
    sfinfo.format = format_code;

#ifdef _WIN32
    private_data->infile = sf_wchar_open(config->input.effective_path_w, SFM_READ, &sfinfo);
#else
    private_data->infile = sf_open(config->input.effective_path, SFM_READ, &sfinfo);
#endif

    if (!private_data->infile) {
        log_error("Error opening RAW input file '%s': %s", config->input.path_arg, sf_strerror(NULL));
        return false;
    }

    // Save info for Phase 2
    private_data->sfinfo = sfinfo;

    return true;
}

static bool rawfile_input_initialize(ModuleContext* context) {
    const AppConfig *config = context->config;
    AppContext* app = context->app;

    RawfileInputContext* private_data = (RawfileInputContext*)app->module.input_private_data;
    if (!private_data || !private_data->infile) return false;

    private_data->repeat_enabled = s_rawfile_config.repeat_enabled;

    app->module.input_format = get_format_info_by_name(s_rawfile_config.format_str) ? get_format_info_by_name(s_rawfile_config.format_str)->format_enum : FORMAT_UNKNOWN;

    app->module.input_bytes_per_iq_sample = get_bytes_per_iq_sample(app->module.input_format);
    if (app->module.input_bytes_per_iq_sample == 0) {
        log_fatal("Internal error: could not determine sample size for format '%s'.", s_rawfile_config.format_str);
        return false;
    }

    sf_command(private_data->infile, SFC_GET_CURRENT_SF_INFO, &private_data->sfinfo, sizeof(private_data->sfinfo));
    app->module.source_info.sample_rate = private_data->sfinfo.samplerate;
    app->module.source_info.frames = private_data->sfinfo.frames;

#ifdef _WIN32
    log_info("Opening RAW input file: %s", config->input.effective_path_utf8);
#else
    log_info("Opening RAW input file: %s", config->input.effective_path);
#endif

    return true;
}

static size_t rawfile_input_read_chunk(ModuleContext* context, void* buffer, size_t bytes_to_read) {
    AppContext* app = context->app;
    RawfileInputContext* p = (RawfileInputContext*)app->module.input_private_data;
    if (!p || !p->infile || bytes_to_read == 0) return 0;

    size_t bytes_read_total = 0;
    size_t bytes_left = bytes_to_read;

    while (bytes_left > 0) {
        // Read directly into the offset buffer
        sf_count_t read_this_pass = sf_read_raw(p->infile, (char*)buffer + bytes_read_total, bytes_left);

        if (read_this_pass < 0) {
            log_error("libsndfile read error: %s", sf_strerror(p->infile));
            handle_fatal_thread_error("Rawfile Reader: File read error.", app);
            return 0;
        }

        if (read_this_pass > 0) {
            bytes_read_total += (size_t)read_this_pass;
            bytes_left -= (size_t)read_this_pass;
        } else {
            // EOF reached
            if (p->repeat_enabled) {
                // Instantly and safely rewind the read pointer back to sample 0
                sf_seek(p->infile, 0, SEEK_SET);
                log_info("Looping RAW input back to start.");
            } else {
                break; // True EOF
            }
        }
    }

    return bytes_read_total;
}

static void* rawfile_input_push_samples_to_queue(ModuleContext* context, QueueSamples queue_samples, void* pipeline_context) {
    (void)context; (void)queue_samples; (void)pipeline_context;
    return NULL; // Not used for synchronous file readers
}
static void rawfile_input_stop_sample_queue_push(ModuleContext* context) {
    (void)context;
}

static void rawfile_input_cleanup(ModuleContext* context) {
    AppContext* app = context->app;
    if (app->module.input_private_data) {
        RawfileInputContext* private_data = (RawfileInputContext*)app->module.input_private_data;
        if (private_data->infile) {
            sf_close(private_data->infile);
            private_data->infile = NULL;
        }
        app->module.input_private_data = NULL;
    }
}

static void rawfile_input_get_summary_info(const ModuleContext* context, InputSummaryInfo* info) {
    const AppConfig *config = context->config;
    const AppContext* app = context->app;
    const char* display_path = config->input.path_arg;
#ifdef _WIN32
    if (config->input.effective_path_utf8[0] != '\0') {
        display_path = config->input.effective_path_utf8;
    }
#endif

    add_summary_item(info, "Input File", "%s", display_path);
    add_summary_item(info, "Input Type", "RAW FILE");
    add_summary_item(info, "Input Format", "%s", s_rawfile_config.format_str);
    add_summary_item(info, "Input Sample Rate", "%.15g Hz", s_rawfile_config.sample_rate_hz);

    char size_buf[40];
    long long file_size_bytes = app->module.source_info.frames * app->module.input_bytes_per_iq_sample;
    add_summary_item(info, "Input File Size", "%s", utility_format_size(file_size_bytes, size_buf, sizeof(size_buf)));
}

static size_t raw_iq_cal_read_cb(void* user_data, void* buffer, size_t bytes) {
    SNDFILE* infile = (SNDFILE*)user_data;
    return (size_t)sf_read_raw(infile, buffer, bytes);
}

static bool rawfile_input_pre_stream_iq_correction(ModuleContext* context) {
    AppConfig* config = (AppConfig*)context->config;
    RawfileInputContext* private_data = (RawfileInputContext*)context->app->module.input_private_data;

    // This routine is only necessary if I/Q correction is enabled.
    if (!config->dsp.iq_correction.enable) {
        return true;
    }

    // The module's only job is to call the calibration service with its private file handle.
    size_t raw_buffer_size = 4096 * context->app->module.input_bytes_per_iq_sample; // IQ_CORRECTION_FFT_SIZE
    void* raw_buffer = mem_arena_alloc(&context->app->pipeline.setup_arena, raw_buffer_size, false);
    if (!raw_buffer) return false;

    // We do an initial read inside the calibration function now
    bool result = iq_correction_run_initial_calibration(context, raw_buffer, raw_buffer_size, raw_iq_cal_read_cb, private_data->infile);

    if (sf_seek(private_data->infile, 0, SEEK_SET) < 0) {
        log_fatal("Failed to rewind file after calibration.");
        return false;
    }
    return result;
}

// --- The InputModuleInterface V-Table ---
static InputModuleInterface s_rawfile_input_api = {
    .initialize = rawfile_input_initialize,
    .push_samples_to_queue = rawfile_input_push_samples_to_queue,
    .read_chunk = rawfile_input_read_chunk,
    .stop_sample_queue_push = rawfile_input_stop_sample_queue_push,
    .cleanup = rawfile_input_cleanup,
    .get_summary_info = rawfile_input_get_summary_info,
    .validate_options = rawfile_input_validate_options,
    .validate_generic_options = NULL,
    .pre_stream_iq_correction = rawfile_input_pre_stream_iq_correction,
};

InputModuleInterface* input_rawfile_get_module_api(void) {
    return &s_rawfile_input_api;
}
