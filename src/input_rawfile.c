// input_rawfile.c

#include "input_rawfile.h"
#include "log.h"
#include "signal_handler.h"
#include "utils.h"
#include "config.h"
#include "platform.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sndfile.h>
#include <pthread.h>
#include <sys/stat.h>
#include <stdarg.h>

// --- ADDED: Include strings.h for strcasecmp on POSIX ---
#ifndef _WIN32
#include <strings.h>
#endif

#ifdef _WIN32
#define strcasecmp _stricmp
#endif

// --- Forward Declarations for Static Functions ---
static bool rawfile_initialize(InputSourceContext* ctx);
static void* rawfile_start_stream(InputSourceContext* ctx);
static void rawfile_stop_stream(InputSourceContext* ctx);
static void rawfile_cleanup(InputSourceContext* ctx);
static void rawfile_get_summary_info(const InputSourceContext* ctx, InputSummaryInfo* info);
static bool rawfile_validate_options(const AppConfig* config);
static bool rawfile_is_sdr_hardware(void);

// --- Helper to add summary items ---
static void add_summary_item(InputSummaryInfo* info, const char* label, const char* value_fmt, ...) {
    if (info->count >= MAX_SUMMARY_ITEMS) return;
    SummaryItem* item = &info->items[info->count];
    strncpy(item->label, label, sizeof(item->label) - 1);
    item->label[sizeof(item->label) - 1] = '\0';
    va_list args;
    va_start(args, value_fmt);
    vsnprintf(item->value, sizeof(item->value), value_fmt, args);
    va_end(args);
    item->value[sizeof(item->value) - 1] = '\0';
    info->count++;
}

// --- The single instance of InputSourceOps for raw files ---
static InputSourceOps raw_file_ops = {
    .initialize = rawfile_initialize,
    .start_stream = rawfile_start_stream,
    .stop_stream = rawfile_stop_stream,
    .cleanup = rawfile_cleanup,
    .get_summary_info = rawfile_get_summary_info,
    .validate_options = rawfile_validate_options,
    .is_sdr_hardware = rawfile_is_sdr_hardware
};

InputSourceOps* get_raw_file_input_ops(void) {
    return &raw_file_ops;
}

// --- Implementations of the InputSourceOps functions ---

static bool rawfile_initialize(InputSourceContext* ctx) {
    const AppConfig *config = ctx->config;
    AppResources *resources = ctx->resources;

    // 1. Create and manually populate the SF_INFO struct from user arguments
    SF_INFO sfinfo;
    memset(&sfinfo, 0, sizeof(SF_INFO));
    sfinfo.samplerate = (int)config->raw_file.sample_rate_hz;
    sfinfo.channels = 2; // Hard-coded for I/Q data

    // 2. Determine the format code and set internal format properties
    int format_code = SF_FORMAT_RAW; // Major format is RAW
    if (strcasecmp(config->raw_file.format_str, "cs16") == 0) {
        format_code |= SF_FORMAT_PCM_16;
        resources->input_pcm_format = PCM_FORMAT_S16;
        resources->input_bit_depth = 16;
        resources->input_bytes_per_sample = sizeof(int16_t);
    } else if (strcasecmp(config->raw_file.format_str, "cu16") == 0) {
        format_code |= SF_FORMAT_PCM_U8; // Per docs, PCM_U8 is the flag for unsigned
        resources->input_pcm_format = PCM_FORMAT_U16;
        resources->input_bit_depth = 16;
        resources->input_bytes_per_sample = sizeof(uint16_t);
    } else if (strcasecmp(config->raw_file.format_str, "cs8") == 0) {
        format_code |= SF_FORMAT_PCM_S8;
        resources->input_pcm_format = PCM_FORMAT_S8;
        resources->input_bit_depth = 8;
        resources->input_bytes_per_sample = sizeof(int8_t);
    } else if (strcasecmp(config->raw_file.format_str, "cu8") == 0) {
        format_code |= SF_FORMAT_PCM_U8;
        resources->input_pcm_format = PCM_FORMAT_U8;
        resources->input_bit_depth = 8;
        resources->input_bytes_per_sample = sizeof(uint8_t);
    } else {
        log_fatal("Invalid raw input format '%s'. Valid formats are cs16, cu16, cs8, cu8.", config->raw_file.format_str);
        return false;
    }
    sfinfo.format = format_code;

    // 3. Open the file using the STANDARD sf_open() function, which will detect the pre-filled SF_INFO
#ifdef _WIN32
    resources->infile = sf_wchar_open(config->effective_input_filename_w, SFM_READ, &sfinfo);
#else
    resources->infile = sf_open(config->effective_input_filename, SFM_READ, &sfinfo);
#endif

    if (!resources->infile) {
        log_fatal("Error opening raw input file '%s': %s", config->input_filename_arg, sf_strerror(NULL));
        return false;
    }

    // 4. Copy the info into our generic application structs
    // We need to re-query sfinfo because libsndfile calculates the frame count for us.
    sf_command(resources->infile, SFC_GET_CURRENT_SF_INFO, &sfinfo, sizeof(sfinfo));
    resources->source_info.samplerate = sfinfo.samplerate;
    resources->source_info.frames = sfinfo.frames;

    log_info("Opened raw file with format %s, rate %.1f Hz, and %lld frames.",
             config->raw_file.format_str,
             (double)resources->source_info.samplerate,
             (long long)resources->source_info.frames);

    return true;
}

static void* rawfile_start_stream(InputSourceContext* ctx) {
    AppResources *resources = ctx->resources;
    size_t bytes_per_input_frame = 2 * resources->input_bytes_per_sample;
    size_t bytes_to_read_per_chunk = (size_t)BUFFER_SIZE_SAMPLES * bytes_per_input_frame;

    while (!is_shutdown_requested() && !resources->error_occurred) {
        WorkItem *current_item = (WorkItem*)queue_dequeue(resources->free_pool_q);
        if (!current_item) {
            break; // Shutdown signaled
        }

        int64_t bytes_read = sf_read_raw(resources->infile, current_item->raw_input_buffer, bytes_to_read_per_chunk);
        if (bytes_read < 0) {
            log_fatal("libsndfile read error: %s", sf_strerror(resources->infile));
            pthread_mutex_lock(&resources->progress_mutex);
            resources->error_occurred = true;
            pthread_mutex_unlock(&resources->progress_mutex);
            queue_signal_shutdown(resources->input_q);
            queue_signal_shutdown(resources->output_q);
            queue_enqueue(resources->free_pool_q, current_item);
            break;
        }

        current_item->frames_read = bytes_read / bytes_per_input_frame;
        current_item->is_last_chunk = (current_item->frames_read == 0);

        if (!current_item->is_last_chunk) {
             pthread_mutex_lock(&resources->progress_mutex);
             resources->total_frames_read += current_item->frames_read;
             pthread_mutex_unlock(&resources->progress_mutex);
        }

        if (!queue_enqueue(resources->input_q, current_item)) {
             queue_enqueue(resources->free_pool_q, current_item);
             break; // Shutdown signaled
        }

        if (current_item->is_last_chunk) {
            break; // End of file
        }
    }
    return NULL;
}

static void rawfile_stop_stream(InputSourceContext* ctx) {
    (void)ctx; // No-op for file-based input
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
    add_summary_item(info, "Input Rate", "%.1f Hz", config->raw_file.sample_rate_hz);

    char size_buf[40];
    long long file_size_bytes = resources->source_info.frames * resources->input_bytes_per_sample * 2;
    add_summary_item(info, "Input File Size", "%s", format_file_size(file_size_bytes, size_buf, sizeof(size_buf)));
}

static bool rawfile_validate_options(const AppConfig* config) {
    (void)config;
    return true;
}

static bool rawfile_is_sdr_hardware(void) {
    return false;
}
