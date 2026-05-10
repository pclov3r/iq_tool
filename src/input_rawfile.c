#include "input_rawfile.h"
#include "constants.h"
#include "log.h"
#include "signal_handler.h"
#include "utils.h"
#include "sample_format_table.h"
#include "app_context.h"
#include "platform.h"
#include "sample_convert.h"
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

#ifndef _WIN32
#include <strings.h>
#endif

#ifdef _WIN32
#define strcasecmp _stricmp
#endif


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
} RawfileInputContext;

static const struct argparse_option rawfile_input_cli_options[] = {
    OPT_GROUP("Raw File Input (rawfile)"),
    OPT_FLOAT(0, "raw-file-input-rate", &s_rawfile_config.raw_file_sample_rate_hz_arg, "(Required) The sample rate of the RAW input file.", NULL, 0, 0),
    OPT_STRING(0, "raw-file-input-sample-format", &s_rawfile_config.format_str, "(Required) The sample format of the RAW input file.", NULL, 0, 0),
};

const struct argparse_option* rawfile_input_get_cli_options(int* count) {
    *count = sizeof(rawfile_input_cli_options) / sizeof(rawfile_input_cli_options[0]);
    return rawfile_input_cli_options;
}

static bool rawfile_input_initialize(ModuleContext* ctx);
static void* rawfile_input_start_stream(ModuleContext* ctx, QueueSamples queue_samples, void* pipeline_ctx);
static size_t rawfile_input_read_chunk(ModuleContext* ctx, void* buffer, size_t bytes_to_read);
static void rawfile_input_stop_stream(ModuleContext* ctx);
static void rawfile_input_cleanup(ModuleContext* ctx);
static void rawfile_input_get_summary_info(const ModuleContext* ctx, InputSummaryInfo* info);
static bool rawfile_input_validate_options(AppConfig* config);
static bool rawfile_input_pre_stream_iq_correction(ModuleContext* ctx);

static InputModuleInterface s_rawfile_input_api = {
    .initialize = rawfile_input_initialize,
    .start_stream = rawfile_input_start_stream,
    .read_chunk = rawfile_input_read_chunk,
    .stop_stream = rawfile_input_stop_stream,
    .cleanup = rawfile_input_cleanup,
    .get_summary_info = rawfile_input_get_summary_info,
    .validate_options = rawfile_input_validate_options,
    .has_known_length = _input_source_has_known_length_true,
    .validate_generic_options = NULL,
    .pre_stream_iq_correction = rawfile_input_pre_stream_iq_correction,
};

InputModuleInterface* input_rawfile_get_module_api(void) {
    return &s_rawfile_input_api;
}

static bool rawfile_input_validate_options(AppConfig* config) {
    (void)config;
    if (s_rawfile_config.raw_file_sample_rate_hz_arg > 0.0f) {
        s_rawfile_config.sample_rate_hz = (double)s_rawfile_config.raw_file_sample_rate_hz_arg;
        s_rawfile_config.sample_rate_provided = true;
    }

    bool format_provided = s_rawfile_config.format_str != NULL;

    if (!s_rawfile_config.sample_rate_provided) {
        log_error("Missing required option --raw-file-input-rate <hz> for raw file input.");
        return false;
    }
    if (!format_provided) {
        log_error("Missing required option --raw-file-input-sample-format <format> for raw file input.");
        return false;
    }

    s_rawfile_config.format_provided = true;
    return true;
}

static bool rawfile_input_initialize(ModuleContext* ctx) {
    const AppConfig *config = ctx->config;
    AppContext* app = ctx->app;

    RawfileInputContext* private_data = (RawfileInputContext*)mem_arena_alloc(&app->pipeline.setup_arena, sizeof(RawfileInputContext), true);
    if (!private_data) {
        return false;
    }
    app->module.input_private_data = private_data;

    app->module.input_format = get_format_info_by_name(s_rawfile_config.format_str) ? get_format_info_by_name(s_rawfile_config.format_str)->format_enum : FORMAT_UNKNOWN;
    if (app->module.input_format == FORMAT_UNKNOWN) {
        log_error("Invalid RAW input format '%s'. See --help for valid formats.", s_rawfile_config.format_str);
        return false;
    }

    app->module.input_bytes_per_iq_sample = get_bytes_per_iq_sample(app->module.input_format);
    if (!config->dsp.dbm_offset_provided) {
        app->module.input_dbm_offset = get_format_info_by_enum(app->module.input_format) ? get_format_info_by_enum(app->module.input_format)->dbm_offset : 0.0f;
    }
    if (app->module.input_bytes_per_iq_sample == 0) {
        log_fatal("Internal error: could not determine sample size for format '%s'.", s_rawfile_config.format_str);
        return false;
    }

    SF_INFO sfinfo;
    memset(&sfinfo, 0, sizeof(SF_INFO));
    sfinfo.samplerate = (int)s_rawfile_config.sample_rate_hz;
    sfinfo.channels = 2;
    int format_code = SF_FORMAT_RAW;
    switch (app->module.input_format) {
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
            log_fatal("Internal error: unhandled format enum in rawfile_input_initialize.");
            return false;
    }
    sfinfo.format = format_code;

#ifdef _WIN32
    log_info("Opening RAW input file: %s", config->input.effective_path_utf8);
    private_data->infile = sf_wchar_open(config->input.effective_path_w, SFM_READ, &sfinfo);
#else
    log_info("Opening RAW input file: %s", config->input.effective_path);
    private_data->infile = sf_open(config->input.effective_path, SFM_READ, &sfinfo);
#endif

    if (!private_data->infile) {
        log_error("Error opening RAW input file '%s': %s", config->input.path_arg, sf_strerror(NULL));
        return false;
    }

    sf_command(private_data->infile, SFC_GET_CURRENT_SF_INFO, &sfinfo, sizeof(sfinfo));
    app->module.source_info.sample_rate = sfinfo.samplerate;
    app->module.source_info.frames = sfinfo.frames;

    app->pipeline_mode = PIPELINE_MODE_FILE_PROCESSING;

    return true;
}

static size_t rawfile_input_read_chunk(ModuleContext* ctx, void* buffer, size_t bytes_to_read) {
    RawfileInputContext* private_data = (RawfileInputContext*)ctx->app->module.input_private_data;
    int64_t bytes_read = sf_read_raw(private_data->infile, buffer, bytes_to_read);
    
    if (bytes_read < 0) {
        log_error("libsndfile read error: %s", sf_strerror(private_data->infile));
        handle_fatal_thread_error("Rawfile Reader: File read error.", ctx->app);
        return 0;
    }
    return (size_t)bytes_read;
}

static void* rawfile_input_start_stream(ModuleContext* ctx, QueueSamples queue_samples, void* pipeline_ctx) {
    (void)ctx; (void)queue_samples; (void)pipeline_ctx;
    return NULL; // Not used for synchronous file readers
}
static void rawfile_input_stop_stream(ModuleContext* ctx) {
    (void)ctx;
}

static void rawfile_input_cleanup(ModuleContext* ctx) {
    AppContext* app = ctx->app;
    if (app->module.input_private_data) {
        RawfileInputContext* private_data = (RawfileInputContext*)app->module.input_private_data;
        if (private_data->infile) {
            log_info("Closing RAW input file.");
            sf_close(private_data->infile);
            private_data->infile = NULL;
        }
        app->module.input_private_data = NULL;
    }
}

static void rawfile_input_get_summary_info(const ModuleContext* ctx, InputSummaryInfo* info) {
    const AppConfig *config = ctx->config;
    const AppContext* app = ctx->app;
    const char* display_path = config->input.path_arg;
#ifdef _WIN32
    if (config->input.effective_path_utf8[0] != '\0') {
        display_path = config->input.effective_path_utf8;
    }
#endif

    add_summary_item(info, "Input File", "%s", display_path);
    add_summary_item(info, "Input Type", "RAW FILE");
    add_summary_item(info, "Input Format", "%s", s_rawfile_config.format_str);
    add_summary_item(info, "Input Rate", "%.0f Hz", s_rawfile_config.sample_rate_hz);

    char size_buf[40];
    long long file_size_bytes = app->module.source_info.frames * app->module.input_bytes_per_iq_sample;
    add_summary_item(info, "Input File Size", "%s", utils_format_size(file_size_bytes, size_buf, sizeof(size_buf)));
}

static bool rawfile_input_pre_stream_iq_correction(ModuleContext* ctx) {
    AppConfig* config = (AppConfig*)ctx->config;
    RawfileInputContext* private_data = (RawfileInputContext*)ctx->app->module.input_private_data;

    // This routine is only necessary if I/Q correction is enabled.
    if (!config->dsp.iq_correction.enable) {
        return true;
    }

    // The module's only job is to call the calibration service with its private file handle.
    return iq_correction_run_initial_calibration(ctx, private_data->infile);
}
