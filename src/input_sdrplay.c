#include "input_sdrplay.h"
#include "module.h"
#include "constants.h"
#include "log.h"
#include "signal_handler.h"
#include "app_context.h"
#include "freq_shift.h"
#include "utils.h"
#include "sample_convert.h"
#include "input_common.h"
#include "mem_arena.h"
#include "queue.h"
#include "ring_buffer.h"
#include "sdr_packet_serializer.h"
#include "argparse.h"
#include "wait_event.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>
#include <stdarg.h>

// Module-specific includes
#include "sdrplay_api.h"

// --- Configuration Constants ---
#define SDRPLAY_TARGET_VERSION 3.15f

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
    sdrplay_api_ErrT (*ApiVersion)(float *apiVer); // Added Version Check
    sdrplay_api_ErrT (*GetDevices)(sdrplay_api_DeviceT*, unsigned int*, unsigned int);
    sdrplay_api_ErrT (*SelectDevice)(sdrplay_api_DeviceT*);
    sdrplay_api_ErrT (*ReleaseDevice)(sdrplay_api_DeviceT*);
    sdrplay_api_ErrT (*GetDeviceParams)(HANDLE, sdrplay_api_DeviceParamsT**);
    const char*      (*GetErrorString)(sdrplay_api_ErrT);
    sdrplay_api_ErrorInfoT* (*GetLastError)(sdrplay_api_DeviceT*);
    sdrplay_api_ErrT (*Update)(HANDLE, sdrplay_api_TunerSelectT, sdrplay_api_ReasonForUpdateT, sdrplay_api_ReasonForUpdateExtension1T);
    sdrplay_api_ErrT (*Init)(HANDLE, sdrplay_api_CallbackFnsT*, void*);
    sdrplay_api_ErrT (*Uninit)(HANDLE);
    sdrplay_api_ErrT (*LockDeviceApi)(void);
    sdrplay_api_ErrT (*UnlockDeviceApi)(void);
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
    LOAD_SDRPLAY_FUNC(ApiVersion);
    LOAD_SDRPLAY_FUNC(GetDevices);
    LOAD_SDRPLAY_FUNC(SelectDevice);
    LOAD_SDRPLAY_FUNC(ReleaseDevice);
    LOAD_SDRPLAY_FUNC(GetDeviceParams);
    LOAD_SDRPLAY_FUNC(GetErrorString);
    LOAD_SDRPLAY_FUNC(GetLastError);
    LOAD_SDRPLAY_FUNC(Update);
    LOAD_SDRPLAY_FUNC(Init);
    LOAD_SDRPLAY_FUNC(Uninit);
    LOAD_SDRPLAY_FUNC(LockDeviceApi);
    LOAD_SDRPLAY_FUNC(UnlockDeviceApi);
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
#define sdrplay_api_ApiVersion    sdrplay_api.ApiVersion
#define sdrplay_api_GetDevices    sdrplay_api.GetDevices
#define sdrplay_api_SelectDevice  sdrplay_api.SelectDevice
#define sdrplay_api_ReleaseDevice sdrplay_api.ReleaseDevice
#define sdrplay_api_GetDeviceParams sdrplay_api.GetDeviceParams
#define sdrplay_api_GetErrorString sdrplay_api.GetErrorString
#define sdrplay_api_GetLastError  sdrplay_api.GetLastError
#define sdrplay_api_Update        sdrplay_api.Update
#define sdrplay_api_Init          sdrplay_api.Init
#define sdrplay_api_Uninit        sdrplay_api.Uninit
#define sdrplay_api_LockDeviceApi sdrplay_api.LockDeviceApi
#define sdrplay_api_UnlockDeviceApi sdrplay_api.UnlockDeviceApi

#endif

extern pthread_mutex_t g_console_mutex;
#define LINE_CLEAR_SEQUENCE "\r \r"


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
    // New Flags for Notch Filters
    bool notch_fm;
    bool notch_dab;
    bool notch_am;
} s_sdrplay_config;

// --- Private Module State ---
typedef struct {
    sdrplay_api_DeviceT *sdr_device;
    sdrplay_api_DeviceParamsT *sdr_device_params;
    bool sdr_api_is_open;
    bool device_selected; // Tracks if SelectDevice was successful
    int16_t *interleave_buffer;
    bool is_streaming;
    pthread_mutex_t driver_mutex;
} SdrplayContext;


void sdrplay_set_default_config(AppConfig* config) {
    config->sdr_general.sample_rate_hz = SDRPLAY_DEFAULT_SAMPLE_RATE_HZ;
    s_sdrplay_config.bandwidth_hz = SDRPLAY_DEFAULT_BANDWIDTH_HZ;
    s_sdrplay_config.sdrplay_bandwidth_hz_arg = 0.0f;
    s_sdrplay_config.sdrplay_if_gain_db_arg = 0;
    s_sdrplay_config.sdrplay_hdr_bw_hz_arg = 0.0f;
    s_sdrplay_config.notch_fm = false;
    s_sdrplay_config.notch_dab = false;
    s_sdrplay_config.notch_am = false;
}

static const struct argparse_option sdrplay_input_cli_options[] = {
    OPT_GROUP("SDRplay Input (sdrplay)"),
    OPT_FLOAT(0, "sdrplay-bandwidth", &s_sdrplay_config.sdrplay_bandwidth_hz_arg, "Set analog bandwidth in Hz. (Optional, Default: 1.536e6)", NULL, 0, 0),
    OPT_INTEGER(0, "sdrplay-device-idx", &s_sdrplay_config.device_index, "Select specific SDRplay device by index (0-indexed). (Default: 0)", NULL, 0, 0),
    OPT_INTEGER(0, "sdrplay-lna-state", &s_sdrplay_config.lna_state, "Set LNA state (0=min gain). Disables AGC.", NULL, 0, 0),
    OPT_INTEGER(0, "sdrplay-if-gain", &s_sdrplay_config.sdrplay_if_gain_db_arg, "Set IF gain in dB (fine gain, e.g., -20, -35, -59). (Default: -50 if --sdrplay-lna-state is specified.) Disables AGC.", NULL, 0, 0),
    OPT_STRING(0, "sdrplay-antenna", &s_sdrplay_config.antenna_port_name, "Select antenna port (device-specific).", NULL, 0, 0),
    OPT_BOOLEAN(0, "sdrplay-hdr-mode", &s_sdrplay_config.use_hdr_mode, "(Optional) Enable HDR mode on RSPdx/RSPdxR2.", NULL, 0, 0),
    OPT_FLOAT(0, "sdrplay-hdr-bw", &s_sdrplay_config.sdrplay_hdr_bw_hz_arg, "Set bandwidth for HDR mode. Requires --sdrplay-hdr-mode.", NULL, 0, 0),
    // Notch Filter Options
    OPT_BOOLEAN(0, "sdrplay-notch-fm", &s_sdrplay_config.notch_fm, "Enable FM Broadcast Notch Filter.", NULL, 0, 0),
    OPT_BOOLEAN(0, "sdrplay-notch-dab",&s_sdrplay_config.notch_dab, "Enable DAB Broadcast Notch Filter.", NULL, 0, 0),
    OPT_BOOLEAN(0, "sdrplay-notch-am", &s_sdrplay_config.notch_am, "Enable MW/AM Notch Filter (RSPduo Tuner A only).", NULL, 0, 0),
};

const struct argparse_option* sdrplay_input_get_cli_options(int* count) {
    *count = sizeof(sdrplay_input_cli_options) / sizeof(sdrplay_input_cli_options[0]);
    return sdrplay_input_cli_options;
}

static bool sdrplay_input_initialize(ModuleContext* ctx);
static void* sdrplay_input_start_stream(ModuleContext* ctx);
static void sdrplay_input_stop_stream(ModuleContext* ctx);
static void sdrplay_input_cleanup(ModuleContext* ctx);
static void sdrplay_input_get_summary_info(const ModuleContext* ctx, InputSummaryInfo* info);
static bool sdrplay_input_validate_options(AppConfig* config);
static bool sdrplay_input_validate_generic_options(const AppConfig* config);
static sdrplay_api_Bw_MHzT map_bw_hz_to_enum(double bw_hz);
static void sdrplay_input_buffered_stream_callback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params, unsigned int numSamples, unsigned int reset, void *cbContext);
static void sdrplay_input_event_callback(sdrplay_api_EventT eventId, sdrplay_api_TunerSelectT tuner, sdrplay_api_EventParamsT *params, void *cbContext);


static InputModuleInterface s_sdrplay_input_api = {
    .initialize = sdrplay_input_initialize,
    .start_stream = sdrplay_input_start_stream,
    .stop_stream = sdrplay_input_stop_stream,
    .cleanup = sdrplay_input_cleanup,
    .get_summary_info = sdrplay_input_get_summary_info,
    .validate_options = sdrplay_input_validate_options,
    .validate_generic_options = sdrplay_input_validate_generic_options,
    .has_known_length = _input_source_has_known_length_false,
    .pre_stream_iq_correction = NULL
};

InputModuleInterface* input_sdrplay_get_module_api(void) {
    return &s_sdrplay_input_api;
}

static bool sdrplay_input_validate_generic_options(const AppConfig* config) {
    if (!config->sdr_general.rf_freq_provided) {
        log_error("SDRplay input requires the --sdr-rf-freq option.");
        return false;
    }
    return true;
}

static bool sdrplay_input_validate_options(AppConfig* config) {
    if (s_sdrplay_config.lna_state != 0) {
        s_sdrplay_config.lna_state_provided = true;
    } else {
        s_sdrplay_config.lna_state = 0; // Ensure default is 0 if not provided
    }

    if (s_sdrplay_config.sdrplay_if_gain_db_arg != 0) {
        if (s_sdrplay_config.sdrplay_if_gain_db_arg > 0 || s_sdrplay_config.sdrplay_if_gain_db_arg < -59) {
            log_error("Invalid value for --sdrplay-if-gain. Must be between -59 and 0.");
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
            log_error("Invalid HDR bandwidth '%.0f'. Valid values are 200e3, 500e3, 1.2e6, 1.7e6.", bw_hz);
            return false;
        }
        s_sdrplay_config.hdr_bw_mode_provided = true;
    }

    if (s_sdrplay_config.hdr_bw_mode_provided && !s_sdrplay_config.use_hdr_mode) {
        log_error("Option --sdrplay-hdr-bw requires --sdrplay-hdr-mode to be specified.");
        return false;
    }

    if (config->sdr_general.sample_rate_provided) {
        if (config->sdr_general.sample_rate_hz < 2e6 || config->sdr_general.sample_rate_hz > 10e6) {
            log_error("Invalid SDRplay sample rate %.0f Hz. Must be between 2,000,000 and 10,000,000.", config->sdr_general.sample_rate_hz);
            return false;
        }
    }

    if (map_bw_hz_to_enum(s_sdrplay_config.bandwidth_hz) == sdrplay_api_BW_Undefined) {
        log_error("Invalid SDRplay bandwidth %.0f Hz. See --help for valid values.", s_sdrplay_config.bandwidth_hz);
        return false;
    }
    if (s_sdrplay_config.bandwidth_hz > config->sdr_general.sample_rate_hz) {
        log_error("Bandwidth (%.0f Hz) cannot be greater than the sample rate (%.0f Hz).", s_sdrplay_config.bandwidth_hz, config->sdr_general.sample_rate_hz);
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


// sdrplay_realtime_stream_callback removed by refactor


static void sdrplay_input_buffered_stream_callback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params, unsigned int numSamples, unsigned int reset, void *cbContext) {
    (void)params;
    AppContext* app = (AppContext*)cbContext;
    SdrplayContext* private_data = (SdrplayContext*)app->module.input_private_data;

    // --- HEARTBEAT ---
    sdr_input_update_heartbeat(app);

    if (is_shutdown_requested() || app->stats.error_occurred) {
        return;
    }

    if (reset) {
        log_info("SDRplay stream reset detected, sending event.");
        sdr_packet_serializer_write_reset_event(app->pipeline.sdr_input_buffer);
    }

    if (numSamples > 0) {
        if (numSamples > MAX_SDRPLAY_CONVERSION_SAMPLES) {
            log_warn("SDRplay callback chunk too large (%u samples). Dropping.", numSamples);
            return;
        }

        int16_t* interleaved_data = private_data->interleave_buffer;

        // 2. Interleave using helper
        sample_convert_interleave_s16(xi, xq, interleaved_data, numSamples);

        // 3. Write single Interleaved block to RingBuffer
        if (!sdr_packet_serializer_write_block(
                app->pipeline.sdr_input_buffer,
                numSamples,
                interleaved_data,
                CS16))
        {
            log_warn("SDR input buffer overrun! Dropped data.");
        }
    }
}

static void sdrplay_input_event_callback(sdrplay_api_EventT eventId, sdrplay_api_TunerSelectT tuner, sdrplay_api_EventParamsT *params, void *cbContext) {
    AppContext* app = (AppContext*)cbContext;
    SdrplayContext* private_data = (SdrplayContext*)app->module.input_private_data;

    if (is_shutdown_requested() || app->stats.error_occurred) {
        return;
    }

    switch (eventId) {
        case sdrplay_api_DeviceRemoved:
            handle_fatal_thread_error("SDRplay device has been removed.", app);
            break;
        case sdrplay_api_DeviceFailure:
            handle_fatal_thread_error("A generic SDRplay device failure has occurred.", app);
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

            // --- Overload ACK Logic ---
            // The SDRplay API requires an ACK for BOTH overload states
            sdrplay_api_Update(
                private_data->sdr_device->dev,
                tuner,
                sdrplay_api_Update_Ctrl_OverloadMsgAck,
                sdrplay_api_Update_Ext1_None
            );

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

static void sdrplay_input_get_summary_info(const ModuleContext* ctx, InputSummaryInfo* info) {
    const AppConfig *config = ctx->config;
    AppContext* app = ctx->app;
    SdrplayContext* private_data = (SdrplayContext*)app->module.input_private_data;
    if (!private_data || !private_data->sdr_device) return;

    char source_name_buf[128];
    snprintf(source_name_buf, sizeof(source_name_buf), "%s (S/N: %s)",
             get_sdrplay_device_name(private_data->sdr_device->hwVer), private_data->sdr_device->SerNo);
    add_summary_item(info, "Input Source", "%s", source_name_buf);
    add_summary_item(info, "Input Format", "16-bit Signed Complex (cs16)");
    add_summary_item(info, "Input Rate", "%d Hz", app->module.source_info.sample_rate);

    add_summary_item(info, "Bandwidth", "%.0f Hz", s_sdrplay_config.bandwidth_hz);
    add_summary_item(info, "RF Frequency", "%.0f Hz", config->sdr_general.rf_freq_hz);

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
 
    // Notch Filter Summary
    if (s_sdrplay_config.notch_fm)  add_summary_item(info, "FM Notch", "Enabled");
    if (s_sdrplay_config.notch_dab) add_summary_item(info, "DAB Notch", "Enabled");
    if (s_sdrplay_config.notch_am)  add_summary_item(info, "AM Notch", "Enabled");

    add_summary_item(info, "Bias-T", "%s", config->sdr_general.bias_t_enable ? "Enabled" : "Disabled");
}

static bool sdrplay_input_initialize(ModuleContext* ctx) {
    const AppConfig *config = ctx->config;
    AppContext* app = ctx->app;
    sdrplay_api_ErrT err;
    bool success = false;

    SdrplayContext* private_data = (SdrplayContext*)mem_arena_alloc(&app->pipeline.setup_arena, sizeof(SdrplayContext), true);
    if (!private_data) return false;

    // Allocate persistent scratch buffer for interleaving (64KB)
    size_t conversion_buf_size = MAX_SDRPLAY_CONVERSION_SAMPLES * 2 * sizeof(int16_t);

    if (pthread_mutex_init(&private_data->driver_mutex, NULL) != 0) {
        log_error("Failed to init driver mutex.");
        return false;
    }
    private_data->interleave_buffer = (int16_t*)mem_arena_alloc(&app->pipeline.setup_arena, conversion_buf_size, false);
    if (!private_data->interleave_buffer) return false;

    private_data->sdr_device = NULL;
    private_data->sdr_api_is_open = false;
    private_data->device_selected = false;
    private_data->is_streaming = false;
    app->module.input_private_data = private_data;

#if defined(_WIN32)
    if (!sdrplay_load_api()) goto cleanup;
#endif

    err = sdrplay_api_Open();
    if (err != sdrplay_api_Success) {
        log_error("Failed to open SDRplay API: %s", sdrplay_api_GetErrorString(err));
        goto cleanup;
    }
    private_data->sdr_api_is_open = true;

    // --- Version Check ---
    float current_version = 0.0f;
    if (sdrplay_api_ApiVersion(&current_version) != sdrplay_api_Success) {
        log_error("Could not determine SDRplay API version.");
        goto cleanup;
    }

    if (current_version < SDRPLAY_TARGET_VERSION) {
        log_error("SDRplay API version %.2f installed.", current_version);
        log_error("Please upgrade to %.2f or newer.", SDRPLAY_TARGET_VERSION);
        goto cleanup;
    }

    // --- LOCK API ---
    sdrplay_api_LockDeviceApi();

    sdrplay_api_DeviceT devs[SDRPLAY_MAX_DEVICES];
    unsigned int numDevs = 0;
    err = sdrplay_api_GetDevices(devs, &numDevs, SDRPLAY_MAX_DEVICES);
    if (err != sdrplay_api_Success) {
        log_error("Failed to list SDRplay devices: %s", sdrplay_api_GetErrorString(err));
        sdrplay_api_UnlockDeviceApi(); // Early unlock
        goto cleanup;
    }
    if (numDevs == 0) {
        log_error("No SDRplay devices found.");
        sdrplay_api_UnlockDeviceApi(); // Early unlock
        goto cleanup;
    }
    if ((unsigned int)s_sdrplay_config.device_index >= numDevs) {
        log_error("Device index %d is out of range. Found %u devices (0 to %u).",
                  s_sdrplay_config.device_index, numDevs, numDevs - 1);
        sdrplay_api_UnlockDeviceApi(); // Early unlock
        goto cleanup;
    }

    private_data->sdr_device = (sdrplay_api_DeviceT *)mem_arena_alloc(&app->pipeline.setup_arena, sizeof(sdrplay_api_DeviceT), true);
    if (!private_data->sdr_device) {
        sdrplay_api_UnlockDeviceApi(); // Early unlock
        goto cleanup;
    }
    memcpy(private_data->sdr_device, &devs[s_sdrplay_config.device_index], sizeof(sdrplay_api_DeviceT));

    err = sdrplay_api_SelectDevice(private_data->sdr_device);

    // --- UNLOCK API ---
    sdrplay_api_UnlockDeviceApi();

    if (err != sdrplay_api_Success) {
        log_fatal("Failed to select SDRplay device %d: %s", s_sdrplay_config.device_index, sdrplay_api_GetErrorString(err));
        private_data->sdr_device = NULL;
        goto cleanup;
    }
    private_data->device_selected = true; // Mark as successfully selected

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

    // --- BULK MODE ENFORCEMENT ---
    if (devParams) {
        devParams->mode = sdrplay_api_BULK;
    }

    // --- DSP HARDWARE ENFORCEMENT ---
    // Always enable hardware DC/IQ correction as it is tuned for the tuner physics.
    chParams->ctrlParams.dcOffset.DCenable = 1;
    chParams->ctrlParams.dcOffset.IQenable = 1;

    devParams->fsFreq.fsHz = config->sdr_general.sample_rate_hz;
    chParams->tunerParams.bwType = bw_enum;
    chParams->tunerParams.ifType = sdrplay_api_IF_Zero;
    chParams->tunerParams.rfFreq.rfHz = config->sdr_general.rf_freq_hz;
    log_debug("SDRplay: API accepting sample rate %.0f Hz.", devParams->fsFreq.fsHz);
    log_debug("SDRplay: API accepting bandwidth %.0f Hz.", s_sdrplay_config.bandwidth_hz);

    if (s_sdrplay_config.use_hdr_mode) {
        if (private_data->sdr_device->hwVer != SDRPLAY_RSPdx_ID && private_data->sdr_device->hwVer != SDRPLAY_RSPdxR2_ID) {
            log_error("--sdrplay-hdr-mode is only supported on RSPdx and RSPdx-R2 devices.");
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

    // --- NOTCH FILTER LOGIC ---
    // Apply notch filter settings based on hardware version
    unsigned char hwVer = private_data->sdr_device->hwVer;
    bool notches_applied = false;

    if (hwVer == SDRPLAY_RSP1A_ID || hwVer == SDRPLAY_RSP1B_ID) {
        if (s_sdrplay_config.notch_fm)  devParams->rsp1aParams.rfNotchEnable = 1;
        if (s_sdrplay_config.notch_dab) devParams->rsp1aParams.rfDabNotchEnable = 1;
        notches_applied = true;
    }
    else if (hwVer == SDRPLAY_RSPdx_ID || hwVer == SDRPLAY_RSPdxR2_ID) {
        if (s_sdrplay_config.notch_fm)  devParams->rspDxParams.rfNotchEnable = 1;
        if (s_sdrplay_config.notch_dab) devParams->rspDxParams.rfDabNotchEnable = 1;
        notches_applied = true;
    }
    else if (hwVer == SDRPLAY_RSPduo_ID) {
        if (s_sdrplay_config.notch_fm)  chParams->rspDuoTunerParams.rfNotchEnable = 1;
        if (s_sdrplay_config.notch_dab) chParams->rspDuoTunerParams.rfDabNotchEnable = 1;

        // AM Notch is specific to RSPduo Tuner A (High-Z port capability)
        if (s_sdrplay_config.notch_am) {
            if (private_data->sdr_device->tuner == sdrplay_api_Tuner_A) {
                chParams->rspDuoTunerParams.tuner1AmNotchEnable = 1;
            } else {
                log_warn("SDRplay: AM Notch ignored (Only available on Tuner A).");
            }
        }
        notches_applied = true;
    }
    else if (hwVer == SDRPLAY_RSP2_ID) {
        // RSP2: Only has one "RF Notch" (Broadband)
        if (s_sdrplay_config.notch_fm) {
            chParams->rsp2TunerParams.rfNotchEnable = 1;
            notches_applied = true;
        }
        if (s_sdrplay_config.notch_dab) log_warn("SDRplay: DAB notch not supported on RSP2.");
    }

    if (notches_applied) {
        if (s_sdrplay_config.notch_fm)  log_info("SDRplay: FM Notch Enabled.");
        if (s_sdrplay_config.notch_dab) log_info("SDRplay: DAB Notch Enabled.");
        if (s_sdrplay_config.notch_am)  log_info("SDRplay: AM Notch Enabled.");
    }

    if (s_sdrplay_config.antenna_port_name || config->sdr_general.bias_t_enable) {
        switch (private_data->sdr_device->hwVer) {
            case SDRPLAY_RSP1A_ID:
            case SDRPLAY_RSP1B_ID:
                if (config->sdr_general.bias_t_enable) {
                    chParams->rsp1aTunerParams.biasTEnable = 1;
                    biast_request_handled = true;
                }
                break;
            case SDRPLAY_RSP2_ID:
                if (config->sdr_general.bias_t_enable) {
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
                        log_error("Invalid antenna port '%s' for RSP2. Use A, B, or HIZ.", s_sdrplay_config.antenna_port_name);
                        goto cleanup;
                    }
                    antenna_request_handled = true;
                }
                break;
            case SDRPLAY_RSPduo_ID:
                if (config->sdr_general.bias_t_enable) {
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
                        log_error("Invalid antenna port '%s' for RSPduo. Use A or HIZ.", s_sdrplay_config.antenna_port_name);
                        goto cleanup;
                    }
                    antenna_request_handled = true;
                }
                break;
            case SDRPLAY_RSPdx_ID:
            case SDRPLAY_RSPdxR2_ID:
                if (config->sdr_general.bias_t_enable) {
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
                        log_error("Invalid antenna port '%s' for RSPdx/RSPdx-R2. Use A, B, or C.", s_sdrplay_config.antenna_port_name);
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
    if (config->sdr_general.bias_t_enable && !biast_request_handled) {
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
        int num_lna_states = get_num_lna_states(private_data->sdr_device->hwVer, config->sdr_general.rf_freq_hz, s_sdrplay_config.use_hdr_mode, hiz_port_selected);
        if (s_sdrplay_config.lna_state < 0 || s_sdrplay_config.lna_state >= num_lna_states) {
            log_error("Invalid LNA state '%d'. Valid range for this device/frequency is 0 (min gain) to %d (max gain).",
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

    app->module.input_format = CS16;
    app->module.input_bytes_per_sample_pair = sample_convert_bytes_per_sample(app->module.input_format);
    app->module.source_info.sample_rate = (int)config->sdr_general.sample_rate_hz;
    app->module.source_info.frames = -1;

    if (config->dsp.raw_passthrough && app->module.input_format != config->output.format) {
        log_error("Option --raw-passthrough requires input and output formats to be identical. SDRplay input is 'cs16', but output was set to '%s'.", config->output.format_name);
        goto cleanup;
    }

    app->pipeline_mode = PIPELINE_MODE_BUFFERED_INPUT;
    success = true;

cleanup:
    if (!success) {
        if (private_data && private_data->sdr_device && private_data->device_selected) {
            sdrplay_api_ReleaseDevice(private_data->sdr_device);
        }
        if (private_data && private_data->sdr_api_is_open) {
            sdrplay_api_Close();
            private_data->sdr_api_is_open = false; // Prevent double close in sdrplay_input_cleanup
        }
#if defined(_WIN32)
        sdrplay_unload_api();
#endif
    }
    return success;
}

static void* sdrplay_input_start_stream(ModuleContext* ctx) {
    AppContext* app = ctx->app;
    SdrplayContext* private_data = (SdrplayContext*)app->module.input_private_data;
    sdrplay_api_CallbackFnsT cbFns;
    cbFns.StreamBCbFn = NULL;
    cbFns.EventCbFn = sdrplay_input_event_callback;

    // Logic unified to Buffered Mode
    log_info("Starting sdrplay stream...");
    cbFns.StreamACbFn = sdrplay_input_buffered_stream_callback;

    sdrplay_api_ErrT err = sdrplay_api_Init(private_data->sdr_device->dev, &cbFns, app);
    if (err == sdrplay_api_Success) private_data->is_streaming = true;

    // After a successful Init, explicitly apply the Bias-T setting if requested.
    if (err == sdrplay_api_Success && app->config->sdr_general.bias_t_enable) {
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
        handle_fatal_thread_error(error_buf, app);
    } else {
        // Wait for the shutdown signal (Event-driven)
        if (app->pipeline.shutdown_event) {
            wait_event_wait(app->pipeline.shutdown_event);
        }
    }

    if (!is_shutdown_requested()) {
        sdrplay_input_stop_stream(ctx);
    }

    return NULL;
}

static void sdrplay_input_stop_stream(ModuleContext* ctx) {
    AppContext* app = ctx->app;
    SdrplayContext* private_data = (SdrplayContext*)app->module.input_private_data;
    if (private_data) {
    pthread_mutex_lock(&private_data->driver_mutex);
    if (private_data && private_data->sdr_device && private_data->is_streaming) {
        log_info("Stopping SDRplay stream...");
        private_data->is_streaming = false;
        sdrplay_api_ErrT err = sdrplay_api_Uninit(private_data->sdr_device->dev);
        // Ignore NotInitialised error, as it may happen during a race condition on shutdown
        if (err != sdrplay_api_Success && err != sdrplay_api_StopPending && err != sdrplay_api_NotInitialised) {
            log_error("Failed to uninitialize SDRplay device: %s", sdrplay_api_GetErrorString(err));
        }
    }
    pthread_mutex_unlock(&private_data->driver_mutex);
}
}

static void sdrplay_input_cleanup(ModuleContext* ctx) {
    AppContext* app = ctx->app;
    if (app->module.input_private_data) {
        SdrplayContext* private_data = (SdrplayContext*)app->module.input_private_data;
        pthread_mutex_lock(&private_data->driver_mutex);
        if (private_data->sdr_device && private_data->device_selected) {
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
        pthread_mutex_unlock(&private_data->driver_mutex);
        pthread_mutex_destroy(&private_data->driver_mutex);
        app->module.input_private_data = NULL;
    }
#if defined(_WIN32)
    sdrplay_unload_api();
#endif
}
