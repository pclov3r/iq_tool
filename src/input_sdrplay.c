#include "input_sdrplay.h"
#include "module.h"
#include "constants.h"
#include "log.h"
#include "signal_handler.h"
#include "app_context.h"
#include "frequency_shift.h"
#include "utils.h"
#include "sample_convert.h"
#include "input_common.h"
#include "memory_arena.h"
#include "queue.h"
#include "ring_buffer.h"
#include "sdr_packet_serializer.h"
#include "argparse.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>
#include <stdarg.h>

// Module-specific includes
#include "sdrplay_api.h"

#if defined(_WIN32)
#include "platform.h"
#include <windows.h>
#include <io.h>
#include <shlwapi.h> // For PathAppendW
#define strcasecmp _stricmp
#else
#include <unistd.h>
#include <strings.h>
#include <time.h>
#endif

#if defined(_WIN32) && defined(WITH_SDRPLAY)
// --- Private Windows Dynamic API Loading ---
typedef struct {
    HINSTANCE dll_handle;
    sdrplay_api_ErrT (*Open)(void);
    sdrplay_api_ErrT (*Close)(void);
    sdrplay_api_ErrT (*GetDevices)(sdrplay_api_DeviceT*, unsigned int*, unsigned int);
    sdrplay_api_ErrT (*SelectDevice)(sdrplay_api_DeviceT*);
    sdrplay_api_ErrT (*ReleaseDevice)(sdrplay_api_DeviceT*);
    sdrplay_api_ErrT (*GetDeviceParams)(HANDLE, sdrplay_api_DeviceParamsT**);
    const char*      (*GetErrorString)(sdrplay_api_ErrT);
    sdrplay_api_ErrorInfoT* (*GetLastError)(sdrplay_api_DeviceT*);
    sdrplay_api_ErrT (*Update)(HANDLE, sdrplay_api_TunerSelectT, sdrplay_api_ReasonForUpdateT, sdrplay_api_ReasonForUpdateExtension1T);
    sdrplay_api_ErrT (*Init)(HANDLE, sdrplay_api_CallbackFnsT*, void*);
    sdrplay_api_ErrT (*Uninit)(HANDLE);
} SdrplayApiFunctionPointers;

static SdrplayApiFunctionPointers sdrplay_api;

static wchar_t* get_sdrplay_dll_path(void) {
    HKEY hKey;
    LONG reg_status;
    wchar_t api_path_buf[MAX_PATH_BUFFER] = {0};
    DWORD buffer_size = sizeof(api_path_buf);
    bool path_found = false;

    reg_status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\SDRplay\\Service\\API", 0, KEY_READ | KEY_WOW64_64KEY, &hKey);
    if (reg_status == ERROR_SUCCESS) {
        if (RegQueryValueExW(hKey, L"Install_Dir", NULL, NULL, (LPBYTE)api_path_buf, &buffer_size) == ERROR_SUCCESS) {
            path_found = true;
        }
        RegCloseKey(hKey);
    }

    if (!path_found) {
        reg_status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\SDRplay\\Service\\API", 0, KEY_READ, &hKey);
        if (reg_status == ERROR_SUCCESS) {
            buffer_size = sizeof(api_path_buf);
            if (RegQueryValueExW(hKey, L"Install_Dir", NULL, NULL, (LPBYTE)api_path_buf, &buffer_size) == ERROR_SUCCESS) {
                path_found = true;
            }
            RegCloseKey(hKey);
        }
    }

    if (!path_found) {
        log_error("Could not find SDRplay API installation path in the registry.");
        log_error("Please ensure the SDRplay API service is installed correctly.");
        return NULL;
    }

#ifdef _WIN64
    PathAppendW(api_path_buf, L"x64");
#else
    PathAppendW(api_path_buf, L"x86");
#endif
    PathAppendW(api_path_buf, L"sdrplay_api.dll");

    return _wcsdup(api_path_buf);
}


#define LOAD_SDRPLAY_FUNC(func_name) \
    do { \
        FARPROC proc = GetProcAddress(sdrplay_api.dll_handle, "sdrplay_api_" #func_name); \
        if (!proc) { \
            log_fatal("Failed to load SDRplay API function: %s", "sdrplay_api_" #func_name); \
            FreeLibrary(sdrplay_api.dll_handle); \
            sdrplay_api.dll_handle = NULL; \
            return false; \
        } \
        memcpy(&sdrplay_api.func_name, &proc, sizeof(sdrplay_api.func_name)); \
    } while (0)

static bool sdrplay_load_api(void) {
    if (sdrplay_api.dll_handle) { return true; }
    wchar_t* dll_path = get_sdrplay_dll_path();
    if (!dll_path) {
        log_fatal("Could not determine SDRplay API DLL path.");
        return false;
    }
    log_debug("Attempting to load SDRplay API from: %ls", dll_path);
    sdrplay_api.dll_handle = LoadLibraryW(dll_path);
    free(dll_path);
    if (!sdrplay_api.dll_handle) {
        print_win_error("LoadLibraryW for sdrplay_api.dll", GetLastError());
        return false;
    }
    log_debug("SDRplay API DLL loaded successfully. Loading function pointers...");
    LOAD_SDRPLAY_FUNC(Open);
    LOAD_SDRPLAY_FUNC(Close);
    LOAD_SDRPLAY_FUNC(GetDevices);
    LOAD_SDRPLAY_FUNC(SelectDevice);
    LOAD_SDRPLAY_FUNC(ReleaseDevice);
    LOAD_SDRPLAY_FUNC(GetDeviceParams);
    LOAD_SDRPLAY_FUNC(GetErrorString);
    LOAD_SDRPLAY_FUNC(GetLastError);
    LOAD_SDRPLAY_FUNC(Update);
    LOAD_SDRPLAY_FUNC(Init);
    LOAD_SDRPLAY_FUNC(Uninit);
    log_debug("All SDRplay API function pointers loaded.");
    return true;
}

static void sdrplay_unload_api(void) {
    if (sdrplay_api.dll_handle) {
        FreeLibrary(sdrplay_api.dll_handle);
        sdrplay_api.dll_handle = NULL;
        log_debug("SDRplay API DLL unloaded.");
    }
}

#define sdrplay_api_Open          sdrplay_api.Open
#define sdrplay_api_Close         sdrplay_api.Close
#define sdrplay_api_GetDevices    sdrplay_api.GetDevices
#define sdrplay_api_SelectDevice  sdrplay_api.SelectDevice
#define sdrplay_api_ReleaseDevice sdrplay_api.ReleaseDevice
#define sdrplay_api_GetDeviceParams sdrplay_api.GetDeviceParams
#define sdrplay_api_GetErrorString sdrplay_api.GetErrorString
#define sdrplay_api_GetLastError  sdrplay_api.GetLastError
#define sdrplay_api_Update        sdrplay_api.Update
#define sdrplay_api_Init          sdrplay_api.Init
#define sdrplay_api_Uninit        sdrplay_api.Uninit

#endif

extern pthread_mutex_t g_console_mutex;
#define LINE_CLEAR_SEQUENCE "\r \r"

extern AppConfig g_config;

// --- Private Module Configuration ---
static struct {
    int device_index;
    int lna_state;
    bool lna_state_provided;
    int if_gain_db;
    bool if_gain_db_provided;
    int sdrplay_if_gain_db_arg;
    sdrplay_api_RspDx_HdrModeBwT hdr_bw_mode;
    bool hdr_bw_mode_provided;
    float sdrplay_hdr_bw_hz_arg;
    bool use_hdr_mode;
    char *antenna_port_name;
    double bandwidth_hz;
    float sdrplay_bandwidth_hz_arg;
    bool bandwidth_provided;
} s_sdrplay_config;

// --- Private Module State ---
typedef struct {
    sdrplay_api_DeviceT *sdr_device;
    sdrplay_api_DeviceParamsT *sdr_device_params;
    bool sdr_api_is_open;
} SdrplayPrivateData;


void sdrplay_set_default_config(AppConfig* config) {
    config->sdr.sample_rate_hz = SDRPLAY_DEFAULT_SAMPLE_RATE_HZ;
    s_sdrplay_config.bandwidth_hz = SDRPLAY_DEFAULT_BANDWIDTH_HZ;
    s_sdrplay_config.sdrplay_bandwidth_hz_arg = 0.0f;
    s_sdrplay_config.sdrplay_if_gain_db_arg = 0;
    s_sdrplay_config.sdrplay_hdr_bw_hz_arg = 0.0f;
}

static const struct argparse_option sdrplay_cli_options[] = {
    OPT_GROUP("SDRplay-Specific Options"),
    OPT_FLOAT(0, "sdrplay-bandwidth", &s_sdrplay_config.sdrplay_bandwidth_hz_arg, "Set analog bandwidth in Hz. (Optional, Default: 1.536e6)", NULL, 0, 0),
    OPT_INTEGER(0, "sdrplay-device-idx", &s_sdrplay_config.device_index, "Select specific SDRplay device by index (0-indexed). (Default: 0)", NULL, 0, 0),
    OPT_INTEGER(0, "sdrplay-lna-state", &s_sdrplay_config.lna_state, "Set LNA state (0=min gain). Disables AGC.", NULL, 0, 0),
    OPT_INTEGER(0, "sdrplay-if-gain", &s_sdrplay_config.sdrplay_if_gain_db_arg, "Set IF gain in dB (fine gain, e.g., -20, -35, -59). (Default: -50 if --sdrplay-lna-state is specified.) Disables AGC.", NULL, 0, 0),
    OPT_STRING(0, "sdrplay-antenna", &s_sdrplay_config.antenna_port_name, "Select antenna port (device-specific).", NULL, 0, 0),
    OPT_BOOLEAN(0, "sdrplay-hdr-mode", &s_sdrplay_config.use_hdr_mode, "(Optional) Enable HDR mode on RSPdx/RSPdxR2.", NULL, 0, 0),
    OPT_FLOAT(0, "sdrplay-hdr-bw", &s_sdrplay_config.sdrplay_hdr_bw_hz_arg, "Set bandwidth for HDR mode. Requires --sdrplay-hdr-mode.", NULL, 0, 0),
};

const struct argparse_option* sdrplay_get_cli_options(int* count) {
    *count = sizeof(sdrplay_cli_options) / sizeof(sdrplay_cli_options[0]);
    return sdrplay_cli_options;
}

static bool sdrplay_initialize(ModuleContext* ctx);
static void* sdrplay_start_stream(ModuleContext* ctx);
static void sdrplay_stop_stream(ModuleContext* ctx);
static void sdrplay_cleanup(ModuleContext* ctx);
static void sdrplay_get_summary_info(const ModuleContext* ctx, InputSummaryInfo* info);
static bool sdrplay_validate_options(AppConfig* config);
static bool sdrplay_validate_generic_options(const AppConfig* config);
static sdrplay_api_Bw_MHzT map_bw_hz_to_enum(double bw_hz);
static void sdrplay_realtime_stream_callback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params, unsigned int numSamples, unsigned int reset, void *cbContext);
static void sdrplay_buffered_stream_callback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params, unsigned int numSamples, unsigned int reset, void *cbContext);
static void sdrplay_event_callback(sdrplay_api_EventT eventId, sdrplay_api_TunerSelectT tuner, sdrplay_api_EventParamsT *params, void *cbContext);


static ModuleApi sdrplay_module_api = {
    .initialize = sdrplay_initialize,
    .start_stream = sdrplay_start_stream,
    .stop_stream = sdrplay_stop_stream,
    .cleanup = sdrplay_cleanup,
    .get_summary_info = sdrplay_get_summary_info,
    .validate_options = sdrplay_validate_options,
    .validate_generic_options = sdrplay_validate_generic_options,
    .has_known_length = _input_source_has_known_length_false,
    .pre_stream_iq_correction = NULL
};

ModuleApi* get_sdrplay_input_module_api(void) {
    return &sdrplay_module_api;
}

static bool sdrplay_validate_generic_options(const AppConfig* config) {
    if (!config->sdr.rf_freq_provided) {
        log_fatal("SDRplay input requires the --sdr-rf-freq option.");
        return false;
    }
    return true;
}

static bool sdrplay_validate_options(AppConfig* config) {
    if (s_sdrplay_config.lna_state != 0) {
        s_sdrplay_config.lna_state_provided = true;
    } else {
        s_sdrplay_config.lna_state = 0; // Ensure default is 0 if not provided
    }
    
    if (s_sdrplay_config.sdrplay_if_gain_db_arg != 0) {
        if (s_sdrplay_config.sdrplay_if_gain_db_arg > 0 || s_sdrplay_config.sdrplay_if_gain_db_arg < -59) {
            log_fatal("Invalid value for --sdrplay-if-gain. Must be between -59 and 0.");
            return false;
        }
        s_sdrplay_config.if_gain_db = s_sdrplay_config.sdrplay_if_gain_db_arg;
        s_sdrplay_config.if_gain_db_provided = true;
    } else {
        s_sdrplay_config.if_gain_db = SDRPLAY_DEFAULT_IF_GAIN_DB;
    }

    if (s_sdrplay_config.sdrplay_bandwidth_hz_arg != 0.0f) {
        s_sdrplay_config.bandwidth_hz = (double)s_sdrplay_config.sdrplay_bandwidth_hz_arg;
        s_sdrplay_config.bandwidth_provided = true;
    }

    if (s_sdrplay_config.sdrplay_hdr_bw_hz_arg != 0.0) {
        double bw_hz = s_sdrplay_config.sdrplay_hdr_bw_hz_arg;
        if      (fabs(bw_hz - 200000.0) < 1.0) s_sdrplay_config.hdr_bw_mode = sdrplay_api_RspDx_HDRMODE_BW_0_200;
        else if (fabs(bw_hz - 500000.0) < 1.0) s_sdrplay_config.hdr_bw_mode = sdrplay_api_RspDx_HDRMODE_BW_0_500;
        else if (fabs(bw_hz - 1200000.0) < 1.0) s_sdrplay_config.hdr_bw_mode = sdrplay_api_RspDx_HDRMODE_BW_1_200;
        else if (fabs(bw_hz - 1700000.0) < 1.0) s_sdrplay_config.hdr_bw_mode = sdrplay_api_RspDx_HDRMODE_BW_1_700;
        else {
            log_fatal("Invalid HDR bandwidth '%.0f'. Valid values are 200e3, 500e3, 1.2e6, 1.7e6.", bw_hz);
            return false;
        }
        s_sdrplay_config.hdr_bw_mode_provided = true;
    }

    if (s_sdrplay_config.hdr_bw_mode_provided && !s_sdrplay_config.use_hdr_mode) {
        log_fatal("Option --sdrplay-hdr-bw requires --sdrplay-hdr-mode to be specified.");
        return false;
    }

    if (config->sdr.sample_rate_provided) {
        if (config->sdr.sample_rate_hz < 2e6 || config->sdr.sample_rate_hz > 10e6) {
            log_fatal("Invalid SDRplay sample rate %.0f Hz. Must be between 2,000,000 and 10,000,000.", config->sdr.sample_rate_hz);
            return false;
        }
    }
    
    if (map_bw_hz_to_enum(s_sdrplay_config.bandwidth_hz) == sdrplay_api_BW_Undefined) {
        log_fatal("Invalid SDRplay bandwidth %.0f Hz. See --help for valid values.", s_sdrplay_config.bandwidth_hz);
        return false;
    }
    if (s_sdrplay_config.bandwidth_hz > config->sdr.sample_rate_hz) {
        log_fatal("Bandwidth (%.0f Hz) cannot be greater than the sample rate (%.0f Hz).", s_sdrplay_config.bandwidth_hz, config->sdr.sample_rate_hz);
        return false;
    }

    return true;
}

const char* get_sdrplay_device_name(uint8_t hwVer) {
    switch (hwVer) {
        case SDRPLAY_RSP1_ID:    return "SDRplay RSP1";
        case SDRPLAY_RSP1A_ID:   return "SDRplay RSP1A";
        case SDRPLAY_RSP1B_ID:   return "SDRplay RSP1B";
        case SDRPLAY_RSP2_ID:    return "SDRplay RSP2";
        case SDRPLAY_RSPduo_ID:  return "SDRplay RSPduo";
        case SDRPLAY_RSPdx_ID:   return "SDRplay RSPdx";
        case SDRPLAY_RSPdxR2_ID: return "SDRplay RSPdx-R2";
        default:                 return "Unknown SDRplay Device";
    }
}

static int get_num_lna_states(uint8_t hwVer, double rfFreqHz, bool useHdrMode, bool isHizPortActive) {
    double rfFreqMHz = rfFreqHz / 1e6;
    switch (hwVer) {
        case SDRPLAY_RSP1_ID: return 4;
        case SDRPLAY_RSP1A_ID:
        case SDRPLAY_RSP1B_ID:
            if (rfFreqMHz <= 60.0) return 7;
            if (rfFreqMHz <= 1000.0) return 10;
            return 9;
        case SDRPLAY_RSP2_ID:
            if (isHizPortActive && rfFreqMHz <= 60.0) return 5;
            if (rfFreqMHz <= 420.0) return 9;
            return 6;
        case SDRPLAY_RSPduo_ID:
            if (isHizPortActive && rfFreqMHz <= 60.0) return 5;
            if (rfFreqMHz <= 60.0) return 7;
            if (rfFreqMHz <= 1000.0) return 10;
            return 9;
        case SDRPLAY_RSPdx_ID:
        case SDRPLAY_RSPdxR2_ID:
            if (useHdrMode && rfFreqMHz <= 2.0) return 21;
            if (rfFreqMHz <= 12.0) return 14;
            if (rfFreqMHz <= 50.0) return 14;
            if (rfFreqMHz <= 60.0) return 28;
            if (rfFreqMHz <= 250.0) return 27;
            if (rfFreqMHz <= 420.0) return 27;
            if (rfFreqMHz <= 1000.0) return 21;
            return 19;
        default:
            log_warn("get_num_lna_states: Unknown device hwVer %d. Returning a default safe value.", hwVer);
            return 10;
    }
}

static sdrplay_api_Bw_MHzT map_bw_hz_to_enum(double bw_hz) {
    if (fabs(bw_hz - 200000.0) < 1.0)   return sdrplay_api_BW_0_200;
    if (fabs(bw_hz - 300000.0) < 1.0)   return sdrplay_api_BW_0_300;
    if (fabs(bw_hz - 600000.0) < 1.0)   return sdrplay_api_BW_0_600;
    if (fabs(bw_hz - 1536000.0) < 1.0)  return sdrplay_api_BW_1_536;
    if (fabs(bw_hz - 5000000.0) < 1.0)  return sdrplay_api_BW_5_000;
    if (fabs(bw_hz - 6000000.0) < 1.0)  return sdrplay_api_BW_6_000;
    if (fabs(bw_hz - 7000000.0) < 1.0)  return sdrplay_api_BW_7_000;
    if (fabs(bw_hz - 8000000.0) < 1.0)  return sdrplay_api_BW_8_000;
    return sdrplay_api_BW_Undefined;
}

static void sdrplay_realtime_stream_callback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params, unsigned int numSamples, unsigned int reset, void *cbContext) {
    (void)params;
    AppResources *resources = (AppResources*)cbContext;
    const AppConfig *config = resources->config;

    // --- HEARTBEAT ---
    sdr_input_update_heartbeat(resources);

    if (is_shutdown_requested() || resources->error_occurred) return;

    if (reset) {
        log_info("SDRplay stream reset detected. Sending reset command to pipeline.");
        SampleChunk* reset_item = (SampleChunk*)queue_dequeue(resources->free_sample_chunk_queue);
        if (reset_item) {
            reset_item->stream_discontinuity_event = true;
            reset_item->is_last_chunk = false;
            reset_item->frames_read = 0;
            if (!queue_enqueue(resources->reader_output_queue, reset_item)) {
                queue_enqueue(resources->free_sample_chunk_queue, reset_item);
            }
        }
    }

    if (numSamples == 0) return;

    if (config->raw_passthrough) {
        int16_t temp_buffer[8192];
        unsigned int samples_processed = 0;
        while (samples_processed < numSamples) {
            unsigned int samples_this_chunk = numSamples - samples_processed;
            if (samples_this_chunk > (sizeof(temp_buffer) / sizeof(int16_t) / 2)) {
                samples_this_chunk = sizeof(temp_buffer) / sizeof(int16_t) / 2;
            }
            for (unsigned int i = 0; i < samples_this_chunk; i++) {
                temp_buffer[i * 2]     = xi[samples_processed + i];
                temp_buffer[i * 2 + 1] = xq[samples_processed + i];
            }
            size_t bytes_to_write = samples_this_chunk * resources->input_bytes_per_sample_pair;
            size_t written = resources->writer_ctx.api.write(&resources->writer_ctx, temp_buffer, bytes_to_write);
            if (written < bytes_to_write) {
                log_debug("Real-time passthrough: stdout write error, consumer likely closed pipe.");
                request_shutdown();
                return;
            }
            samples_processed += samples_this_chunk;
        }
    } else {
        SampleChunk *item = (SampleChunk*)queue_dequeue(resources->free_sample_chunk_queue);
        if (!item) {
            log_warn("Real-time pipeline stalled. Dropping %u samples.", numSamples);
            return;
        }
        item->stream_discontinuity_event = false;
        size_t samples_to_copy = numSamples;
        if (samples_to_copy > PIPELINE_CHUNK_BASE_SAMPLES) {
            log_warn("SDRplay callback provided more samples than buffer can hold. Truncating.");
            samples_to_copy = PIPELINE_CHUNK_BASE_SAMPLES;
        }
        int16_t *raw_buffer = (int16_t*)item->raw_input_data;
        for (unsigned int i = 0; i < samples_to_copy; i++) {
            raw_buffer[i * 2] = xi[i];
            raw_buffer[i * 2 + 1] = xq[i];
        }
        item->frames_read = samples_to_copy;
        item->is_last_chunk = false;
	    item->packet_sample_format = resources->input_format;

        if (samples_to_copy > 0) {
            pthread_mutex_lock(&resources->progress_mutex);
            resources->total_frames_read += samples_to_copy;
            pthread_mutex_unlock(&resources->progress_mutex);
        }
        if (!queue_enqueue(resources->reader_output_queue, item)) {
            queue_enqueue(resources->free_sample_chunk_queue, item);
        }
    }
}

static void sdrplay_buffered_stream_callback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params, unsigned int numSamples, unsigned int reset, void *cbContext) {
    (void)params;
    AppResources *resources = (AppResources*)cbContext;

    // --- HEARTBEAT ---
    sdr_input_update_heartbeat(resources);

    if (is_shutdown_requested() || resources->error_occurred) {
        return;
    }

    if (reset) {
        log_info("SDRplay stream reset detected (buffered mode), sending event.");
        sdr_packet_serializer_write_reset_event(resources->sdr_input_buffer);
    }

    if (numSamples > 0) {
        if (!sdr_packet_serializer_write_deinterleaved_chunk(resources->sdr_input_buffer, numSamples, xi, xq, CS16)) {
            log_warn("SDR input buffer overrun! Dropped data.");
        }
    }
}

static void sdrplay_event_callback(sdrplay_api_EventT eventId, sdrplay_api_TunerSelectT tuner, sdrplay_api_EventParamsT *params, void *cbContext) {
    AppResources *resources = (AppResources*)cbContext;
    SdrplayPrivateData* private_data = (SdrplayPrivateData*)resources->input_module_private_data;

    if (is_shutdown_requested() || resources->error_occurred) {
        return;
    }

    switch (eventId) {
        case sdrplay_api_DeviceRemoved:
            handle_fatal_thread_error("SDRplay device has been removed.", resources);
            break;
        case sdrplay_api_DeviceFailure:
            handle_fatal_thread_error("A generic SDRplay device failure has occurred.", resources);
            break;
        case sdrplay_api_PowerOverloadChange: {
            sdrplay_api_PowerOverloadCbEventIdT overload_state = params->powerOverloadParams.powerOverloadChangeType;
            pthread_mutex_lock(&g_console_mutex);
#ifdef _WIN32
            const int stderr_is_tty = _isatty(_fileno(stderr));
#else
            const int stderr_is_tty = isatty(fileno(stderr));
#endif
            if (overload_state == sdrplay_api_Overload_Detected) {
                if (stderr_is_tty) fprintf(stderr, LINE_CLEAR_SEQUENCE);
                log_warn("Overload Detected! Reduce gain or RF input level.");
            } else {
                if (stderr_is_tty) fprintf(stderr, LINE_CLEAR_SEQUENCE);
                log_info("Overload condition corrected.");
            }
            pthread_mutex_unlock(&g_console_mutex);
            sdrplay_api_Update(private_data->sdr_device->dev, tuner, sdrplay_api_Update_Ctrl_OverloadMsgAck, sdrplay_api_Update_Ext1_None);
            break;
        }
        case sdrplay_api_GainChange:
        case sdrplay_api_RspDuoModeChange:
            break;
        default:
            log_info("Received unknown SDRplay event (ID: %d)", eventId);
            break;
    }
}

static void sdrplay_get_summary_info(const ModuleContext* ctx, InputSummaryInfo* info) {
    const AppConfig *config = ctx->config;
    AppResources *resources = ctx->resources;
    SdrplayPrivateData* private_data = (SdrplayPrivateData*)resources->input_module_private_data;
    if (!private_data || !private_data->sdr_device) return;

    char source_name_buf[128];
    snprintf(source_name_buf, sizeof(source_name_buf), "%s (S/N: %s)",
             get_sdrplay_device_name(private_data->sdr_device->hwVer), private_data->sdr_device->SerNo);
    add_summary_item(info, "Input Source", "%s", source_name_buf);
    add_summary_item(info, "Input Format", "16-bit Signed Complex (cs16)");
    add_summary_item(info, "Input Rate", "%d Hz", resources->source_info.samplerate);

    add_summary_item(info, "Bandwidth", "%.0f Hz", s_sdrplay_config.bandwidth_hz);
    add_summary_item(info, "RF Frequency", "%.0f Hz", config->sdr.rf_freq_hz);

    if (s_sdrplay_config.lna_state_provided || s_sdrplay_config.if_gain_db_provided) {
        // Manual gain mode is active. Show the status of both components.
        // The values in s_sdrplay_config will be either user-provided or the manual-mode defaults.
        add_summary_item(info, "LNA State", "%d", s_sdrplay_config.lna_state);
        add_summary_item(info, "IF Gain", "%d dB", s_sdrplay_config.if_gain_db);
    } else {
        // AGC is active.
        add_summary_item(info, "Gain", "Automatic (AGC)");
    }

    if (s_sdrplay_config.antenna_port_name) add_summary_item(info, "Antenna Port", "%s", s_sdrplay_config.antenna_port_name);
    
    if (s_sdrplay_config.use_hdr_mode) {
        const char* bw_str = "1700000";
        if (s_sdrplay_config.hdr_bw_mode_provided) {
            switch(s_sdrplay_config.hdr_bw_mode) {
                case sdrplay_api_RspDx_HDRMODE_BW_0_200: bw_str = "200000"; break;
                case sdrplay_api_RspDx_HDRMODE_BW_0_500: bw_str = "500000"; break;
                case sdrplay_api_RspDx_HDRMODE_BW_1_200: bw_str = "1200000"; break;
                case sdrplay_api_RspDx_HDRMODE_BW_1_700: bw_str = "1700000"; break;
            }
        }
        add_summary_item(info, "HDR Mode", "Enabled (BW: %s Hz)", bw_str);
    }
    add_summary_item(info, "Bias-T", "%s", config->sdr.bias_t_enable ? "Enabled" : "Disabled");
}

static bool sdrplay_initialize(ModuleContext* ctx) {
    const AppConfig *config = ctx->config;
    AppResources *resources = ctx->resources;
    sdrplay_api_ErrT err;
    bool success = false;

    SdrplayPrivateData* private_data = (SdrplayPrivateData*)mem_arena_alloc(&resources->setup_arena, sizeof(SdrplayPrivateData), true);
    if (!private_data) return false;
    
    private_data->sdr_device = NULL;
    private_data->sdr_api_is_open = false;
    resources->input_module_private_data = private_data;

#if defined(_WIN32)
    if (!sdrplay_load_api()) goto cleanup;
#endif

    err = sdrplay_api_Open();
    if (err != sdrplay_api_Success) {
        log_fatal("Failed to open SDRplay API: %s", sdrplay_api_GetErrorString(err));
        goto cleanup;
    }
    private_data->sdr_api_is_open = true;

    sdrplay_api_DeviceT devs[SDRPLAY_MAX_DEVICES];
    unsigned int numDevs = 0;
    err = sdrplay_api_GetDevices(devs, &numDevs, SDRPLAY_MAX_DEVICES);
    if (err != sdrplay_api_Success) {
        log_fatal("Failed to list SDRplay devices: %s", sdrplay_api_GetErrorString(err));
        goto cleanup;
    }
    if (numDevs == 0) {
        log_fatal("No SDRplay devices found.");
        goto cleanup;
    }
    if ((unsigned int)s_sdrplay_config.device_index >= numDevs) {
        log_fatal("Device index %d is out of range. Found %u devices (0 to %u).",
                  s_sdrplay_config.device_index, numDevs, numDevs - 1);
        goto cleanup;
    }

    private_data->sdr_device = (sdrplay_api_DeviceT *)mem_arena_alloc(&resources->setup_arena, sizeof(sdrplay_api_DeviceT), true);
    if (!private_data->sdr_device) goto cleanup;
    memcpy(private_data->sdr_device, &devs[s_sdrplay_config.device_index], sizeof(sdrplay_api_DeviceT));

    err = sdrplay_api_SelectDevice(private_data->sdr_device);
    if (err != sdrplay_api_Success) {
        log_fatal("Failed to select SDRplay device %d: %s", s_sdrplay_config.device_index, sdrplay_api_GetErrorString(err));
        private_data->sdr_device = NULL;
        goto cleanup;
    }
    log_info("Using SDRplay device: %s (S/N: %s)",
             get_sdrplay_device_name(private_data->sdr_device->hwVer), private_data->sdr_device->SerNo);

    err = sdrplay_api_GetDeviceParams(private_data->sdr_device->dev, &private_data->sdr_device_params);
    if (err != sdrplay_api_Success) {
        log_fatal("Failed to get device parameters: %s", sdrplay_api_GetErrorString(err));
        goto cleanup;
    }

    sdrplay_api_RxChannelParamsT *chParams = private_data->sdr_device_params->rxChannelA;
    sdrplay_api_DevParamsT *devParams = private_data->sdr_device_params->devParams;
    sdrplay_api_Bw_MHzT bw_enum = map_bw_hz_to_enum(s_sdrplay_config.bandwidth_hz);

    devParams->fsFreq.fsHz = config->sdr.sample_rate_hz;
    chParams->tunerParams.bwType = bw_enum;
    chParams->tunerParams.ifType = sdrplay_api_IF_Zero;
    chParams->tunerParams.rfFreq.rfHz = config->sdr.rf_freq_hz;
    log_debug("SDRplay: API accepting sample rate %.0f Hz.", devParams->fsFreq.fsHz);
    log_debug("SDRplay: API accepting bandwidth %.0f Hz.", s_sdrplay_config.bandwidth_hz);

    if (s_sdrplay_config.use_hdr_mode) {
        if (private_data->sdr_device->hwVer != SDRPLAY_RSPdx_ID && private_data->sdr_device->hwVer != SDRPLAY_RSPdxR2_ID) {
            log_fatal("--sdrplay-hdr-mode is only supported on RSPdx and RSPdx-R2 devices.");
            goto cleanup;
        }
        devParams->rspDxParams.hdrEnable = 1;
        chParams->rspDxTunerParams.hdrBw = s_sdrplay_config.hdr_bw_mode_provided ? s_sdrplay_config.hdr_bw_mode : sdrplay_api_RspDx_HDRMODE_BW_1_700;
    }

    if (private_data->sdr_device->hwVer == SDRPLAY_RSPduo_ID) {
        private_data->sdr_device->rspDuoMode = sdrplay_api_RspDuoMode_Single_Tuner;
        private_data->sdr_device->tuner = sdrplay_api_Tuner_A;
    }

    bool antenna_request_handled = false;
    bool biast_request_handled = false;
    bool hiz_port_selected = false;

    if (s_sdrplay_config.antenna_port_name || config->sdr.bias_t_enable) {
        switch (private_data->sdr_device->hwVer) {
            case SDRPLAY_RSP1A_ID:
            case SDRPLAY_RSP1B_ID:
                if (config->sdr.bias_t_enable) {
                    chParams->rsp1aTunerParams.biasTEnable = 1;
                    biast_request_handled = true;
                }
                break;
            case SDRPLAY_RSP2_ID:
                if (config->sdr.bias_t_enable) {
                    chParams->rsp2TunerParams.biasTEnable = 1;
                    biast_request_handled = true;
                }
                if (s_sdrplay_config.antenna_port_name) {
                    if (strcasecmp(s_sdrplay_config.antenna_port_name, "A") == 0) {
                        chParams->rsp2TunerParams.antennaSel = sdrplay_api_Rsp2_ANTENNA_A;
                    } else if (strcasecmp(s_sdrplay_config.antenna_port_name, "B") == 0) {
                        chParams->rsp2TunerParams.antennaSel = sdrplay_api_Rsp2_ANTENNA_B;
                    } else if (strcasecmp(s_sdrplay_config.antenna_port_name, "HIZ") == 0) {
                        chParams->rsp2TunerParams.amPortSel = sdrplay_api_Rsp2_AMPORT_2;
                        hiz_port_selected = true;
                    } else {
                        log_fatal("Invalid antenna port '%s' for RSP2. Use A, B, or HIZ.", s_sdrplay_config.antenna_port_name);
                        goto cleanup;
                    }
                    antenna_request_handled = true;
                }
                break;
            case SDRPLAY_RSPduo_ID:
                if (config->sdr.bias_t_enable) {
                    chParams->rspDuoTunerParams.biasTEnable = 1;
                    biast_request_handled = true;
                }
                if (s_sdrplay_config.antenna_port_name) {
                    if (strcasecmp(s_sdrplay_config.antenna_port_name, "A") == 0) {
                        // Port A is default, no change needed.
                    } else if (strcasecmp(s_sdrplay_config.antenna_port_name, "HIZ") == 0) {
                        chParams->rspDuoTunerParams.tuner1AmPortSel = sdrplay_api_RspDuo_AMPORT_2;
                        hiz_port_selected = true;
                    } else {
                        log_fatal("Invalid antenna port '%s' for RSPduo. Use A or HIZ.", s_sdrplay_config.antenna_port_name);
                        goto cleanup;
                    }
                    antenna_request_handled = true;
                }
                break;
            case SDRPLAY_RSPdx_ID:
            case SDRPLAY_RSPdxR2_ID:
                if (config->sdr.bias_t_enable) {
                    devParams->rspDxParams.biasTEnable = 1;
                    biast_request_handled = true;
                }
                if (s_sdrplay_config.antenna_port_name) {
                    if (strcasecmp(s_sdrplay_config.antenna_port_name, "A") == 0) {
                        devParams->rspDxParams.antennaSel = sdrplay_api_RspDx_ANTENNA_A;
                    } else if (strcasecmp(s_sdrplay_config.antenna_port_name, "B") == 0) {
                        devParams->rspDxParams.antennaSel = sdrplay_api_RspDx_ANTENNA_B;
                    } else if (strcasecmp(s_sdrplay_config.antenna_port_name, "C") == 0) {
                        devParams->rspDxParams.antennaSel = sdrplay_api_RspDx_ANTENNA_C;
                    } else {
                        log_fatal("Invalid antenna port '%s' for RSPdx/RSPdx-R2. Use A, B, or C.", s_sdrplay_config.antenna_port_name);
                        goto cleanup;
                    }
                    antenna_request_handled = true;
                }
                break;
        }
    }

    if (s_sdrplay_config.antenna_port_name && !antenna_request_handled) {
        log_warn("Antenna selection not applicable for the detected device.");
    }
    if (config->sdr.bias_t_enable && !biast_request_handled) {
        log_warn("Bias-T is not supported on the detected device.");
    }

    if (s_sdrplay_config.lna_state_provided || s_sdrplay_config.if_gain_db_provided) {
        chParams->ctrlParams.agc.enable = sdrplay_api_AGC_DISABLE;
        log_info("SDRplay: AGC disabled due to manual gain setting.");
    }

    if (s_sdrplay_config.if_gain_db_provided) {
        chParams->tunerParams.gain.gRdB = -s_sdrplay_config.if_gain_db;
    }

    if (s_sdrplay_config.lna_state_provided) {
        int num_lna_states = get_num_lna_states(private_data->sdr_device->hwVer, config->sdr.rf_freq_hz, s_sdrplay_config.use_hdr_mode, hiz_port_selected);
        if (s_sdrplay_config.lna_state < 0 || s_sdrplay_config.lna_state >= num_lna_states) {
            log_fatal("Invalid LNA state '%d'. Valid range for this device/frequency is 0 (min gain) to %d (max gain).",
                      s_sdrplay_config.lna_state, num_lna_states - 1);
            goto cleanup;
        }
        // CORRECTED: Invert the user's gain level to map to the API's LNAstate.
        // The user provides a level where 0 = min gain and (num_lna_states - 1) = max gain.
        // The API expects an LNAstate where 0 = max gain (min reduction) and (num_lna_states - 1) = min gain (max reduction).
        // Therefore, we apply the formula: LNAstate = (Total States - 1) - User Level.
        int lna_state_for_api = (num_lna_states - 1) - s_sdrplay_config.lna_state;
        chParams->tunerParams.gain.LNAstate = lna_state_for_api;
    }

    resources->input_format = CS16;
    resources->input_bytes_per_sample_pair = get_bytes_per_sample(resources->input_format);
    resources->source_info.samplerate = (int)config->sdr.sample_rate_hz;
    resources->source_info.frames = -1;

    if (config->raw_passthrough && resources->input_format != config->output_format) {
        log_fatal("Option --raw-passthrough requires input and output formats to be identical. SDRplay input is 'cs16', but output was set to '%s'.", config->sample_type_name);
        goto cleanup;
    }

    success = true;

cleanup:
    if (!success) {
        if (private_data && private_data->sdr_device) {
            sdrplay_api_ReleaseDevice(private_data->sdr_device);
        }
        if (private_data && private_data->sdr_api_is_open) {
            sdrplay_api_Close();
        }
#if defined(_WIN32)
        sdrplay_unload_api();
#endif
    }
    return success;
}

static void* sdrplay_start_stream(ModuleContext* ctx) {
    AppResources *resources = ctx->resources;
    SdrplayPrivateData* private_data = (SdrplayPrivateData*)resources->input_module_private_data;
    sdrplay_api_CallbackFnsT cbFns;
    cbFns.StreamBCbFn = NULL;
    cbFns.EventCbFn = sdrplay_event_callback;

    if (resources->pipeline_mode == PIPELINE_MODE_BUFFERED_SDR) {
        log_info("Starting SDRplay stream (Buffered File Mode)...");
        cbFns.StreamACbFn = sdrplay_buffered_stream_callback;
    } else { // PIPELINE_MODE_REALTIME_SDR
        log_info("Starting SDRplay stream (Real-Time Stdout Mode)...");
        cbFns.StreamACbFn = sdrplay_realtime_stream_callback;
    }

    sdrplay_api_ErrT err = sdrplay_api_Init(private_data->sdr_device->dev, &cbFns, resources);

    // After a successful Init, explicitly apply the Bias-T setting if requested.
    if (err == sdrplay_api_Success && resources->config->sdr.bias_t_enable) {
        log_info("Enabling Bias-T");
        sdrplay_api_ReasonForUpdateT reasonForUpdate = sdrplay_api_Update_None;
        sdrplay_api_ReasonForUpdateExtension1T reasonForUpdateExt1 = sdrplay_api_Update_Ext1_None;
        bool biast_request_handled = false; // Re-check if the device supports it

        switch (private_data->sdr_device->hwVer) {
            case SDRPLAY_RSP1A_ID:
            case SDRPLAY_RSP1B_ID:
                reasonForUpdate = sdrplay_api_Update_Rsp1a_BiasTControl;
                biast_request_handled = true;
                break;
            case SDRPLAY_RSP2_ID:
                reasonForUpdate = sdrplay_api_Update_Rsp2_BiasTControl;
                biast_request_handled = true;
                break;
            case SDRPLAY_RSPduo_ID:
                reasonForUpdate = sdrplay_api_Update_RspDuo_BiasTControl;
                biast_request_handled = true;
                break;
            case SDRPLAY_RSPdx_ID:
            case SDRPLAY_RSPdxR2_ID:
                reasonForUpdateExt1 = sdrplay_api_Update_RspDx_BiasTControl;
                biast_request_handled = true;
                break;
        }

        if (biast_request_handled) {
            err = sdrplay_api_Update(private_data->sdr_device->dev, private_data->sdr_device->tuner, reasonForUpdate, reasonForUpdateExt1);
            if (err != sdrplay_api_Success) {
                log_error("Failed to enable Bias-T: %s", sdrplay_api_GetErrorString(err));
            }
        }
    }

    if (err != sdrplay_api_Success && err != sdrplay_api_StopPending) {
        sdrplay_api_ErrorInfoT *errorInfo = sdrplay_api_GetLastError(private_data->sdr_device);
        char error_buf[1536];
        snprintf(error_buf, sizeof(error_buf), "sdrplay_api_Init() failed: %s", sdrplay_api_GetErrorString(err));
        if (errorInfo && strlen(errorInfo->message) > 0) {
            snprintf(error_buf + strlen(error_buf), sizeof(error_buf) - strlen(error_buf), " - API Message: %s", errorInfo->message);
        }
        handle_fatal_thread_error(error_buf, resources);
    } else {
        while (!is_shutdown_requested() && !resources->error_occurred) {
#ifdef _WIN32
            Sleep(100);
#else
            struct timespec sleep_time = {0, 100000000L};
            nanosleep(&sleep_time, NULL);
#endif
        }
    }
    
    sdrplay_stop_stream(ctx);
    
    return NULL;
}

static void sdrplay_stop_stream(ModuleContext* ctx) {
    AppResources *resources = ctx->resources;
    SdrplayPrivateData* private_data = (SdrplayPrivateData*)resources->input_module_private_data;
    if (private_data && private_data->sdr_device) {
        log_info("Stopping SDRplay stream...");
        sdrplay_api_ErrT err = sdrplay_api_Uninit(private_data->sdr_device->dev);
        if (err != sdrplay_api_Success && err != sdrplay_api_StopPending) {
            log_error("Failed to uninitialize SDRplay device: %s", sdrplay_api_GetErrorString(err));
        }
    }
}

static void sdrplay_cleanup(ModuleContext* ctx) {
    AppResources *resources = ctx->resources;
    if (resources->input_module_private_data) {
        SdrplayPrivateData* private_data = (SdrplayPrivateData*)resources->input_module_private_data;
        if (private_data->sdr_device) {
            log_debug("Releasing SDRplay device handle...");
            sdrplay_api_ReleaseDevice(private_data->sdr_device);
#ifndef _WIN32
            log_debug("Waiting for SDRplay daemon to release device...");
            sleep(1);
#endif
            // No free() needed, memory is in the arena
            private_data->sdr_device = NULL;
        }
        if (private_data->sdr_api_is_open) {
            log_debug("Closing SDRplay API...");
            sdrplay_api_Close();
            private_data->sdr_api_is_open = false;
        }
        resources->input_module_private_data = NULL;
    }
#if defined(_WIN32)
    sdrplay_unload_api();
#endif
}
