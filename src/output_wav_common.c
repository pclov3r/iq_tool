/**
 * @file output_wav_common.c
 * @brief Implements the shared logic for WAV and RF64 output modules.
 */

#include "output_wav_common.h"
#include <sndfile.h>
#include "app_context.h"
#include "log.h"
#include "platform.h"
#include "ring_buffer.h"
#include "utilities.h"
#include "signal_handler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <stdint.h>
#ifdef _WIN32
#else
#include <sys/stat.h>
#endif

// --- Private Helper ---
// This helper remains private to the common implementation.

// --- Shared Implementation ---

bool wav_common_validate_options(AppConfig* config) {
    // This logic is identical for both WAV and RF64.
    if (config->output.sample_format != CS16 && config->output.sample_format != CU8) {
        log_error("Invalid sample format '%s' for WAV/RF64 container. Only 'cs16' and 'cu8' are supported.", config->output.sample_format_str);
        return false;
    }
    return true;
}

bool wav_common_initialize(ModuleContext* context, int sf_format_flag) {
    const AppConfig* config = context->config;
    AppContext* app = context->app;

    // Allocate the private data struct for this module instance.
    WavCommonContext* data = (WavCommonContext*)mem_arena_alloc(&app->pipeline.setup_arena, sizeof(WavCommonContext), true);
    if (!data) return false;
    app->module.output_private_data = data;

    // Use platform-specific UTF-8 path for messages.
    #ifdef _WIN32
    const char* out_path = config->output.effective_path_utf8;
    #else
    const char* out_path = config->output.effective_path;
    #endif

    if (!utility_verify_output_path(config, out_path)) {
        return false;
    }

    // Prepare the libsndfile info struct.
    SF_INFO sfinfo;
    memset(&sfinfo, 0, sizeof(SF_INFO));
    sfinfo.samplerate = (int)config->output_sample_rate.rate_hz;
    sfinfo.channels = 2;
    sfinfo.format = sf_format_flag; // Use the specific format flag passed by the wrapper.

    switch (config->output.sample_format) {
        case CS16: sfinfo.format |= SF_FORMAT_PCM_16; break;
        case CU8:  sfinfo.format |= SF_FORMAT_PCM_U8; break;
        default: return false; // Should be caught by validation.
    }

    // Verify that libsndfile supports this format combination.
    if (!sf_format_check(&sfinfo)) { log_error("libsndfile does not support the requested format (Rate: %d, Format: 0x%08X).", sfinfo.samplerate, sfinfo.format); return false; }

    // Open the file using the appropriate platform-specific function.
    #ifdef _WIN32
    data->handle = sf_wchar_open(config->output.effective_path_w, SFM_WRITE, &sfinfo);
    #else
    data->handle = sf_open(out_path, SFM_WRITE, &sfinfo);
    #endif

    if (!data->handle) { log_error("Error opening output WAV file %s: %s", out_path, sf_strerror(NULL)); return false; }

    // --- SDR-XML Metadata Injection ---
    time_t now = time(NULL);
    struct tm* tm_info = gmtime(&now);
    char time_utc[64] = {0};
    char time_created[64] = {0};
    char date_only[64] = {0};
    if (tm_info) {
        strftime(time_utc, sizeof(time_utc), "%d-%m-%Y %H:%M:%S", tm_info);
        strftime(time_created, sizeof(time_created), "%d-%b-%Y %H:%M", tm_info);
        strftime(date_only, sizeof(date_only), "%d-%b-%Y", tm_info);
    }

    size_t bps = (size_t)config->output_sample_rate.rate_hz * app->module.output_bytes_per_iq_sample * 2;
    int bits = (int)app->module.output_bytes_per_iq_sample * 8;
    const char* radio_model = config->input.type_name ? config->input.type_name : "iq_tool";

#ifndef GIT_HASH
#define GIT_HASH "unknown"
#endif

    // Construct the strictly formatted filename exclusively for the XML attributes to satisfy SDR Console's regex parser
    char sdr_console_xml_filename[256];
    snprintf(sdr_console_xml_filename, sizeof(sdr_console_xml_filename), "%s %02d%02d%02d.000 %.3fMHz.wav",
        date_only, tm_info ? tm_info->tm_hour : 0, tm_info ? tm_info->tm_min : 0, tm_info ? tm_info->tm_sec : 0,
        config->sdr_general.rf_freq_hz / 1000000.0);

    // Construct the true Title string that SDR Console uses natively for its UI
    char true_title[256];
    snprintf(true_title, sizeof(true_title), "%.3f MHz, BW %.0f kHz, %04d-%02d-%02d %02d:%02d",
        config->sdr_general.rf_freq_hz / 1000000.0,
        config->output_sample_rate.rate_hz / 1000.0,
        tm_info ? tm_info->tm_year + 1900 : 0,
        tm_info ? tm_info->tm_mon + 1 : 0,
        tm_info ? tm_info->tm_mday : 0,
        tm_info ? tm_info->tm_hour : 0,
        tm_info ? tm_info->tm_min : 0);

    char xml_buf[2048];
    int xml_len = snprintf(xml_buf, sizeof(xml_buf),
        "<?xml version=\"1.0\"?>"
        "<SDR-XML-Root Description=\"Saved recording data\" Created=\"%s\">"
        "<Definition CurrentTimeUTC=\"%s\" "
        "Filename=\"%s\" "
        "FirstFile=\"%s\" "
        "Folder=\"\" "
        "InternalTag=\"iq_tool_recording\" "
        "PreviousFile=\"\" "
        "RadioModel=\"%s\" "
        "RadioSerial=\"\" "
        "SoftwareName=\"iq_tool\" "
        "SoftwareVersion=\"%s\" "
        "UTC=\"%s\" "
        "XMLLevel=\"XMLLevel003\" "
        "CreatedBy=\"iq_tool\" "
        "TimeZoneStatus=\"0\" "
        "TimeZoneInfo=\"\" "
        "DualMode=\"0\" "
        "Sequence=\"0\" "
        "ADFrequency=\"0\" "
        "BitsPerSample=\"%d\" "
        "BytesPerSecond=\"%zu\" "
        "RadioCenterFreq=\"%.0f\" "
        "SampleRate=\"%.0f\" "
        "UTCSeconds=\"%lld\"/>"
        "</SDR-XML-Root>",
        time_created, time_utc, sdr_console_xml_filename, sdr_console_xml_filename, radio_model, GIT_HASH, time_utc,
        bits, bps, config->sdr_general.rf_freq_hz, config->output_sample_rate.rate_hz, (long long)now
    );

    if (xml_len > 0 && xml_len < (int)sizeof(xml_buf)) {
        size_t utf16_size = (size_t)(xml_len + 1) * 2;
        uint8_t* utf16_buf = (uint8_t*)mem_arena_alloc(&app->pipeline.setup_arena, utf16_size, true);
        if (utf16_buf) {
            for (int i = 0; i < xml_len; i++) {
                utf16_buf[i * 2] = (uint8_t)xml_buf[i];
                utf16_buf[i * 2 + 1] = 0x00;
            }
            utf16_buf[xml_len * 2] = 0x00;
            utf16_buf[xml_len * 2 + 1] = 0x00;

            SF_CHUNK_INFO chunk;
            memset(&chunk, 0, sizeof(chunk));
            strncpy(chunk.id, "auxi", sizeof(chunk.id));
            chunk.id_size = 4;
            chunk.datalen = utf16_size;
            chunk.data = utf16_buf;

            int set_res = sf_set_chunk(data->handle, &chunk);
            if (set_res != SF_ERR_NO_ERROR) {
                log_warn("Failed to write XML metadata chunk: %s", sf_error_number(set_res));
            }
        }
    }

    // Fallback: Also write standard WAV RIFF INFO tags!
    // SDR Console (and other audio players) often read Title and Artist from here instead of the custom XML.
    // Use true_title so it perfectly matches the native SDR Console UI format
    sf_set_string(data->handle, SF_STR_TITLE, true_title);
    sf_set_string(data->handle, SF_STR_SOFTWARE, "iq_tool");
    sf_set_string(data->handle, SF_STR_COMMENT, radio_model);
    sf_set_string(data->handle, SF_STR_ARTIST, radio_model);

    return true;
}

size_t wav_common_write_chunk(ModuleContext* context, const void* buffer, size_t bytes_to_write) {
    AppContext* app = context->app;
    WavCommonContext* data = (WavCommonContext*)app->module.output_private_data;
    if (!data || !data->handle || bytes_to_write == 0) return 0;
    sf_count_t written = sf_write_raw(data->handle, buffer, bytes_to_write);
    if (written > 0) data->total_bytes_written += written;
    return (size_t)written;
}

void wav_common_cleanup(ModuleContext* context) {
    AppContext* app = context->app;
    if (!app->module.output_private_data) return;
    WavCommonContext* data = (WavCommonContext*)app->module.output_private_data;
    if (data->handle) {
        sf_close(data->handle);
        data->handle = NULL;
    }
    app->stats.final_output_size_bytes = data->total_bytes_written;
}
