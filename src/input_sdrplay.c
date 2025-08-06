// --- Includes for the main implementation ---
#include "input_sdrplay.h" // Its own header
#include "log.h"
#include "signal_handler.h" // For is_shutdown_requested and handle_fatal_thread_error
#include "config.h"
#include "types.h"
#include "spectrum_shift.h"
#include "utils.h"
#include "sample_convert.h" // For get_bytes_per_sample

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>
#include <stdarg.h>

// --- Platform-specific includes ---
#if defined(_WIN32)
#include "platform.h" // For platform_get_sdrplay_dll_path, print_win_error
#include <windows.h>
#include <io.h>
#define strcasecmp _stricmp
#else
#include <unistd.h>
#include <strings.h>
#include <time.h>
#endif

// =================================================================================
// START: Merged Dynamic Loading Code for Windows SDRplay API
// =================================================================================
#if defined(_WIN32) && defined(WITH_SDRPLAY)

// Global instance of the function pointers struct
SdrplayApiFunctionPointers sdrplay_api;

// Helper macro to simplify GetProcAddress calls
#define LOAD_SDRPLAY_FUNC(func_name) \
    sdrplay_api.func_name = (void*)GetProcAddress(sdrplay_api.dll_handle, "sdrplay_api_" #func_name); \
    if (!sdrplay_api.func_name) { \
        log_fatal("Failed to load SDRplay API function: %s", "sdrplay_api_" #func_name); \
        FreeLibrary(sdrplay_api.dll_handle); \
        sdrplay_api.dll_handle = NULL; \
        return false; \
    }

bool sdrplay_load_api(void) {
    if (sdrplay_api.dll_handle) {
        return true; // Already loaded
    }

    wchar_t* dll_path = platform_get_sdrplay_dll_path();
    if (!dll_path) {
        log_fatal("Could not determine SDRplay API DLL path.");
        return false;
    }

    log_info("Attempting to load SDRplay API from: %ls", dll_path);
    sdrplay_api.dll_handle = LoadLibraryW(dll_path);
    free(dll_path);

    if (!sdrplay_api.dll_handle) {
        print_win_error("LoadLibraryW for sdrplay_api.dll", GetLastError());
        return false;
    }

    log_info("SDRplay API DLL loaded successfully. Loading function pointers...");

    // Load all required function pointers from the DLL
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

    log_info("All SDRplay API function pointers loaded.");
    return true;
}

void sdrplay_unload_api(void) {
    if (sdrplay_api.dll_handle) {
        FreeLibrary(sdrplay_api.dll_handle);
        sdrplay_api.dll_handle = NULL;
        log_info("SDRplay API DLL unloaded.");
    }
}

#endif // defined(_WIN32) && defined(WITH_SDRPLAY)
// =================================================================================
// END: Merged Dynamic Loading Code
// =================================================================================


// Add an external declaration for the global console mutex defined in main.c
extern pthread_mutex_t g_console_mutex;

// Define a sequence to clear the current line on a terminal
#define LINE_CLEAR_SEQUENCE "\r                                                                                \r"

// --- Helper Functions ---

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

/**
 * @brief Gets the precise number of LNA states (gain levels) available for a given
 *        SDRplay device at a specific RF frequency and port selection.
 */
static int get_num_lna_states(uint8_t hwVer, double rfFreqHz, bool useHdrMode, bool isHizPortActive) {
    double rfFreqMHz = rfFreqHz / 1e6;

    switch (hwVer) {
        case SDRPLAY_RSP1_ID:
            return 4;
        case SDRPLAY_RSP1A_ID:
            if (rfFreqMHz <= 60.0)   return 7;
            if (rfFreqMHz <= 1000.0) return 10;
            return 9;
        case SDRPLAY_RSP1B_ID:
            if (rfFreqMHz <= 50.0)   return 7;
            if (rfFreqMHz <= 1000.0) return 10;
            return 9;
        case SDRPLAY_RSP2_ID:
            if (isHizPortActive && rfFreqMHz <= 60.0) {
                return 5;
            }
            if (rfFreqMHz <= 420.0)  return 9;
            return 6;
        case SDRPLAY_RSPduo_ID:
            if (isHizPortActive && rfFreqMHz <= 60.0) {
                return 5;
            }
            if (rfFreqMHz <= 60.0)   return 7;
            if (rfFreqMHz <= 1000.0) return 10;
            return 9;
        case SDRPLAY_RSPdx_ID:
        case SDRPLAY_RSPdxR2_ID:
            if (useHdrMode && rfFreqMHz <= 2.0) {
                return 21;
            }
            if (rfFreqMHz <= 12.0)   return 14;
            if (rfFreqMHz <= 50.0)   return 14;
            if (rfFreqMHz <= 60.0)   return 28;
            if (rfFreqMHz <= 250.0)  return 27;
            if (rfFreqMHz <= 420.0)  return 27;
            if (rfFreqMHz <= 1000.0) return 21;
            return 19;
        default:
            log_warn("get_num_lna_states: Unknown device hwVer %d. Returning a default safe value.", hwVer);
            return 10;
    }
}

// --- ADDED: Helper to map bandwidth Hz to API enum ---
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

// --- SDRplay API Callback Functions ---

void sdrplay_stream_callback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params, unsigned int numSamples, unsigned int reset, void *cbContext) {
    (void)params; // Unused parameter
    AppResources *resources = (AppResources*)cbContext;

    if (is_shutdown_requested() || resources->error_occurred) {
        return;
    }

    if (reset) {
        pthread_mutex_lock(&g_console_mutex);
        #ifdef _WIN32
        if (_isatty(_fileno(stderr))) {
            fprintf(stderr, LINE_CLEAR_SEQUENCE);
        }
        #else
        if (isatty(fileno(stderr))) {
            fprintf(stderr, LINE_CLEAR_SEQUENCE);
        }
        #endif
        log_info("SDRplay stream reset detected. Resetting DSP state.");
        pthread_mutex_unlock(&g_console_mutex);

        pthread_mutex_lock(&resources->dsp_mutex);
        if (resources->resampler) msresamp_crcf_reset(resources->resampler);
        shift_reset_nco(resources);
        pthread_mutex_unlock(&resources->dsp_mutex);
    }

    WorkItem *item = (WorkItem*)queue_dequeue(resources->free_pool_q);
    if (!item) {
        return;
    }

    // The number of bytes to copy is the number of samples (frames) * size of a cs16 pair
    size_t bytes_to_copy = numSamples * resources->input_bytes_per_sample_pair;
    if (bytes_to_copy > (BUFFER_SIZE_SAMPLES * resources->input_bytes_per_sample_pair)) {
        log_warn("SDRplay callback provided more samples than buffer can hold. Truncating.");
        numSamples = BUFFER_SIZE_SAMPLES;
    }

    int16_t *raw_buffer = (int16_t*)item->raw_input_buffer;
    for (unsigned int i = 0; i < numSamples; i++) {
        raw_buffer[i * 2]     = xi[i];
        raw_buffer[i * 2 + 1] = xq[i];
    }
    item->frames_read = numSamples;
    item->is_last_chunk = false;

    if (numSamples > 0) {
        pthread_mutex_lock(&resources->progress_mutex);
        resources->total_frames_read += numSamples;
        pthread_mutex_unlock(&resources->progress_mutex);
    }

    if (!queue_enqueue(resources->input_q, item)) {
        queue_enqueue(resources->free_pool_q, item);
    }
}

void sdrplay_event_callback(sdrplay_api_EventT eventId, sdrplay_api_TunerSelectT tuner, sdrplay_api_EventParamsT *params, void *cbContext) {
    AppResources *resources = (AppResources*)cbContext;

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

            sdrplay_api_Update(resources->sdr_device->dev, tuner, sdrplay_api_Update_Ctrl_OverloadMsgAck, sdrplay_api_Update_Ext1_None);
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

// --- InputSourceOps Implementations for SDRplay ---

static void sdrplay_get_summary_info(const InputSourceContext* ctx, InputSummaryInfo* info) {
    const AppConfig *config = ctx->config;
    const AppResources *resources = ctx->resources;

    if (!resources->sdr_device) return;

    char source_name_buf[128];
    snprintf(source_name_buf, sizeof(source_name_buf), "%s (S/N: %s)",
             get_sdrplay_device_name(resources->sdr_device->hwVer),
             resources->sdr_device->SerNo);
    add_summary_item(info, "Input Source", "%s", source_name_buf);

    add_summary_item(info, "Input Format", "16-bit Signed Complex (cs16)");
    add_summary_item(info, "Input Rate", "%.3f Msps", (double)resources->source_info.samplerate / 1e6);
    double active_bw = config->sdrplay.bandwidth_provided ? config->sdrplay.bandwidth_hz : SDRPLAY_DEFAULT_BANDWIDTH_HZ;
    add_summary_item(info, "Bandwidth", "%.3f MHz", active_bw / 1e6);
    add_summary_item(info, "RF Frequency", "%.6f MHz", config->sdr.rf_freq_hz / 1e6);

    if (config->sdrplay.gain_level_provided) {
        add_summary_item(info, "Gain", "%d (Manual)", config->sdrplay.gain_level);
    } else {
        add_summary_item(info, "Gain", "Automatic (AGC)");
    }

    if (config->sdrplay.antenna_port_name) {
        add_summary_item(info, "Antenna Port", "%s", config->sdrplay.antenna_port_name);
    }

    if (config->sdrplay.use_hdr_mode) {
        const char* bw_str = "1.7"; // Default
        if (config->sdrplay.hdr_bw_mode_provided) {
            switch(config->sdrplay.hdr_bw_mode) {
                case sdrplay_api_RspDx_HDRMODE_BW_0_200: bw_str = "0.2"; break;
                case sdrplay_api_RspDx_HDRMODE_BW_0_500: bw_str = "0.5"; break;
                case sdrplay_api_RspDx_HDRMODE_BW_1_200: bw_str = "1.2"; break;
                case sdrplay_api_RspDx_HDRMODE_BW_1_700: bw_str = "1.7"; break;
            }
        }
        add_summary_item(info, "HDR Mode", "Enabled (BW: %s MHz)", bw_str);
    }

    add_summary_item(info, "Bias-T", "%s", config->sdr.bias_t_enable ? "Enabled" : "Disabled");
}

static bool sdrplay_validate_options(const AppConfig* config) {
    if (!config->sdr.rf_freq_provided) {
        log_fatal("Option --rf-freq <hz> is required when using an SDR input.");
        return false;
    }
    if (config->set_center_frequency_target_hz || config->freq_shift_requested) {
        log_fatal("Frequency shifting options (--target-freq, --shift-freq) are only valid for 'wav' input.");
        return false;
    }
    if (config->sdrplay.hdr_bw_mode_provided && !config->sdrplay.use_hdr_mode) {
        log_fatal("Option --sdrplay-hdr-bw requires --sdrplay-hdr-mode to be specified.");
        return false;
    }

    double sample_rate = config->sdrplay.sample_rate_provided ? config->sdrplay.sample_rate_hz : SDRPLAY_DEFAULT_SAMPLE_RATE_HZ;
    double bandwidth = config->sdrplay.bandwidth_provided ? config->sdrplay.bandwidth_hz : SDRPLAY_DEFAULT_BANDWIDTH_HZ;

    if (sample_rate < 2e6 || sample_rate > 10e6) {
        log_fatal("Invalid SDRplay sample rate %.0f Hz. Must be between 2,000,000 and 10,000,000.", sample_rate);
        return false;
    }

    if (map_bw_hz_to_enum(bandwidth) == sdrplay_api_BW_Undefined) {
        log_fatal("Invalid SDRplay bandwidth %.0f Hz. See --help for valid values.", bandwidth);
        return false;
    }

    if (bandwidth > sample_rate) {
        log_fatal("Bandwidth (%.0f Hz) cannot be greater than the sample rate (%.0f Hz).", bandwidth, sample_rate);
        return false;
    }

    return true;
}

static bool sdrplay_is_sdr_hardware(void) {
    return true;
}


static bool sdrplay_initialize(InputSourceContext* ctx) {
    const AppConfig *config = ctx->config;
    AppResources *resources = ctx->resources;
    sdrplay_api_ErrT err;

#if defined(_WIN32)
    if (!sdrplay_load_api()) { return false; }
#endif
    err = sdrplay_api_Open();
    if (err != sdrplay_api_Success) {
        log_fatal("Failed to open SDRplay API: %s", sdrplay_api_GetErrorString(err));
#if defined(_WIN32)
        sdrplay_unload_api();
#endif
        return false;
    }
    resources->sdr_api_is_open = true;

    sdrplay_api_DeviceT devs[SDRPLAY_MAX_DEVICES];
    unsigned int numDevs = 0;
    err = sdrplay_api_GetDevices(devs, &numDevs, SDRPLAY_MAX_DEVICES);
    if (err != sdrplay_api_Success) {
        log_fatal("Failed to list SDRplay devices: %s", sdrplay_api_GetErrorString(err));
        return false;
    }
    if (numDevs == 0) {
        log_fatal("No SDRplay devices found.");
        return false;
    }
    if ((unsigned int)config->sdrplay.device_index >= numDevs) {
        log_fatal("Device index %d is out of range. Found %u devices (0 to %u).",
                config->sdrplay.device_index, numDevs, numDevs - 1);
        return false;
    }

    resources->sdr_device = (sdrplay_api_DeviceT *)malloc(sizeof(sdrplay_api_DeviceT));
    if (!resources->sdr_device) {
        log_fatal("Failed to allocate memory for device structure: %s", strerror(errno));
        return false;
    }
    memcpy(resources->sdr_device, &devs[config->sdrplay.device_index], sizeof(sdrplay_api_DeviceT));

    err = sdrplay_api_SelectDevice(resources->sdr_device);
    if (err != sdrplay_api_Success) {
        log_fatal("Failed to select SDRplay device %d: %s", config->sdrplay.device_index, sdrplay_api_GetErrorString(err));
        free(resources->sdr_device);
        resources->sdr_device = NULL;
        return false;
    }

    log_info("Using SDRplay device: %s (S/N: %s)",
            get_sdrplay_device_name(resources->sdr_device->hwVer),
            resources->sdr_device->SerNo);

    err = sdrplay_api_GetDeviceParams(resources->sdr_device->dev, &resources->sdr_device_params);
    if (err != sdrplay_api_Success) {
        log_fatal("Failed to get device parameters: %s", sdrplay_api_GetErrorString(err));
        sdrplay_api_ReleaseDevice(resources->sdr_device);
        free(resources->sdr_device);
        resources->sdr_device = NULL;
        return false;
    }

    sdrplay_api_RxChannelParamsT *chParams = resources->sdr_device_params->rxChannelA;
    sdrplay_api_DevParamsT *devParams = resources->sdr_device_params->devParams;

    double sample_rate_to_set = config->sdrplay.sample_rate_provided ? config->sdrplay.sample_rate_hz : SDRPLAY_DEFAULT_SAMPLE_RATE_HZ;
    double bandwidth_to_set = config->sdrplay.bandwidth_provided ? config->sdrplay.bandwidth_hz : SDRPLAY_DEFAULT_BANDWIDTH_HZ;

    sdrplay_api_Bw_MHzT bw_enum = map_bw_hz_to_enum(bandwidth_to_set);

    devParams->fsFreq.fsHz = sample_rate_to_set;
    chParams->tunerParams.bwType = bw_enum;
    chParams->tunerParams.ifType = sdrplay_api_IF_Zero; // Always use Zero-IF mode

    chParams->tunerParams.rfFreq.rfHz = config->sdr.rf_freq_hz;

    if (config->sdrplay.use_hdr_mode) {
        if (resources->sdr_device->hwVer != SDRPLAY_RSPdx_ID && resources->sdr_device->hwVer != SDRPLAY_RSPdxR2_ID) {
            log_fatal("--sdrplay-hdr-mode is only supported on RSPdx and RSPdx-R2 devices.");
            sdrplay_api_ReleaseDevice(resources->sdr_device); free(resources->sdr_device); resources->sdr_device = NULL; return false;
        }
        devParams->rspDxParams.hdrEnable = 1;
        chParams->rspDxTunerParams.hdrBw = config->sdrplay.hdr_bw_mode_provided
                                           ? config->sdrplay.hdr_bw_mode
                                           : sdrplay_api_RspDx_HDRMODE_BW_1_700;
    }

    if (resources->sdr_device->hwVer == SDRPLAY_RSPduo_ID) {
        resources->sdr_device->rspDuoMode = sdrplay_api_RspDuoMode_Single_Tuner;
        resources->sdr_device->tuner = sdrplay_api_Tuner_A;
    }

    bool antenna_request_handled = false;
    bool biast_request_handled = false;
    bool hiz_port_selected = false;

    if (config->sdrplay.antenna_port_name || config->sdr.bias_t_enable) {
        switch (resources->sdr_device->hwVer) {
            case SDRPLAY_RSP2_ID:
                if (config->sdr.bias_t_enable) {
                    chParams->rsp2TunerParams.biasTEnable = 1;
                    biast_request_handled = true;
                }
                if (config->sdrplay.antenna_port_name) {
                    if (strcasecmp(config->sdrplay.antenna_port_name, "A") == 0) {
                        chParams->rsp2TunerParams.antennaSel = sdrplay_api_Rsp2_ANTENNA_A;
                    } else if (strcasecmp(config->sdrplay.antenna_port_name, "B") == 0) {
                        chParams->rsp2TunerParams.antennaSel = sdrplay_api_Rsp2_ANTENNA_B;
                    } else if (strcasecmp(config->sdrplay.antenna_port_name, "HIZ") == 0) {
                        chParams->rsp2TunerParams.amPortSel = sdrplay_api_Rsp2_AMPORT_2;
                        hiz_port_selected = true;
                    } else {
                        log_fatal("Invalid antenna port '%s' for RSP2. Use A, B, or HIZ.", config->sdrplay.antenna_port_name);
                        sdrplay_api_ReleaseDevice(resources->sdr_device); free(resources->sdr_device); resources->sdr_device = NULL; return false;
                    }
                    antenna_request_handled = true;
                }
                break;

            case SDRPLAY_RSPduo_ID:
                if (config->sdr.bias_t_enable) {
                    chParams->rspDuoTunerParams.biasTEnable = 1;
                    biast_request_handled = true;
                }
                if (config->sdrplay.antenna_port_name) {
                     if (strcasecmp(config->sdrplay.antenna_port_name, "A") == 0) {
                        // Default, no change needed
                    } else if (strcasecmp(config->sdrplay.antenna_port_name, "HIZ") == 0) {
                        chParams->rspDuoTunerParams.tuner1AmPortSel = sdrplay_api_RspDuo_AMPORT_2;
                        hiz_port_selected = true;
                    } else {
                        log_fatal("Invalid antenna port '%s' for RSPduo. Use A or HIZ.", config->sdrplay.antenna_port_name);
                        sdrplay_api_ReleaseDevice(resources->sdr_device); free(resources->sdr_device); resources->sdr_device = NULL; return false;
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
                if (config->sdrplay.antenna_port_name) {
                    if (strcasecmp(config->sdrplay.antenna_port_name, "A") == 0) {
                        devParams->rspDxParams.antennaSel = sdrplay_api_RspDx_ANTENNA_A;
                        antenna_request_handled = true;
                    } else if (strcasecmp(config->sdrplay.antenna_port_name, "B") == 0) {
                        devParams->rspDxParams.antennaSel = sdrplay_api_RspDx_ANTENNA_B;
                        antenna_request_handled = true;
                    } else if (strcasecmp(config->sdrplay.antenna_port_name, "C") == 0) {
                        devParams->rspDxParams.antennaSel = sdrplay_api_RspDx_ANTENNA_C;
                        antenna_request_handled = true;
                    } else {
                        log_fatal("Invalid antenna port '%s' for RSPdx/RSPdx-R2. Use A, B, or C.", config->sdrplay.antenna_port_name);
                        sdrplay_api_ReleaseDevice(resources->sdr_device); free(resources->sdr_device); resources->sdr_device = NULL; return false;
                    }
                }
                break;

            case SDRPLAY_RSP1A_ID:
            case SDRPLAY_RSP1B_ID:
                 if (config->sdr.bias_t_enable) {
                    chParams->rsp1aTunerParams.biasTEnable = 1;
                    biast_request_handled = true;
                }
                break;
        }
    }

    if (config->sdrplay.antenna_port_name && !antenna_request_handled) { log_warn("Antenna selection not applicable for the detected device."); }
    if (config->sdr.bias_t_enable && !biast_request_handled) { log_warn("Bias-T is not supported on the detected device."); }

    if (config->sdrplay.gain_level_provided) {
        int num_lna_states = get_num_lna_states(resources->sdr_device->hwVer, config->sdr.rf_freq_hz, config->sdrplay.use_hdr_mode, hiz_port_selected);
        if (config->sdrplay.gain_level < 0 || config->sdrplay.gain_level >= num_lna_states) {
            log_fatal("Invalid gain level '%d'. Valid range for this device/frequency/port is 0 to %d.",
                    config->sdrplay.gain_level, num_lna_states - 1);
            sdrplay_api_ReleaseDevice(resources->sdr_device); free(resources->sdr_device); resources->sdr_device = NULL; return false;
        }
        chParams->ctrlParams.agc.enable = sdrplay_api_AGC_DISABLE;
        chParams->tunerParams.gain.LNAstate = num_lna_states - 1 - config->sdrplay.gain_level;
    }

    resources->input_format = CS16;
    resources->input_bytes_per_sample_pair = get_bytes_per_sample(resources->input_format);
    resources->source_info.samplerate = (int)sample_rate_to_set;
    resources->source_info.frames = -1;

    return true;
}

static void* sdrplay_start_stream(InputSourceContext* ctx) {
    AppResources *resources = ctx->resources;
    sdrplay_api_CallbackFnsT cbFns;
    cbFns.StreamACbFn = sdrplay_stream_callback;
    cbFns.StreamBCbFn = NULL;
    cbFns.EventCbFn = sdrplay_event_callback;

    log_info("Starting SDRplay stream...");
    sdrplay_api_ErrT err = sdrplay_api_Init(resources->sdr_device->dev, &cbFns, resources);

    if (err != sdrplay_api_Success && err != sdrplay_api_StopPending) {
        sdrplay_api_ErrorInfoT *errorInfo = sdrplay_api_GetLastError(resources->sdr_device);
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
    return NULL;
}

static void sdrplay_stop_stream(InputSourceContext* ctx) {
    AppResources *resources = ctx->resources;
    if (resources->sdr_device) {
        log_info("Stopping SDRplay stream...");
        sdrplay_api_ErrT err = sdrplay_api_Uninit(resources->sdr_device->dev);
        if (err != sdrplay_api_Success && err != sdrplay_api_StopPending) {
            log_error("Failed to uninitialize SDRplay device: %s", sdrplay_api_GetErrorString(err));
        }
    }
}

static void sdrplay_cleanup(InputSourceContext* ctx) {
    AppResources *resources = ctx->resources;
    if (resources->sdr_device) {
        log_info("Releasing SDRplay device handle...");
        sdrplay_api_ReleaseDevice(resources->sdr_device);
        #ifndef _WIN32
        log_info("Waiting for SDRplay daemon to release device...");
        sleep(1);
        #endif
        free(resources->sdr_device);
        resources->sdr_device = NULL;
    }
    if (resources->sdr_api_is_open) {
        log_info("Closing SDRplay API...");
        sdrplay_api_Close();
#if defined(_WIN32)
        sdrplay_unload_api();
#endif
        resources->sdr_api_is_open = false;
    }
}

static InputSourceOps sdrplay_ops = {
    .initialize = sdrplay_initialize,
    .start_stream = sdrplay_start_stream,
    .stop_stream = sdrplay_stop_stream,
    .cleanup = sdrplay_cleanup,
    .get_summary_info = sdrplay_get_summary_info,
    .validate_options = sdrplay_validate_options,
    .is_sdr_hardware = sdrplay_is_sdr_hardware
};

InputSourceOps* get_sdrplay_input_ops(void) {
    return &sdrplay_ops;
}
