#ifndef INPUT_SDRPLAY_H_
#define INPUT_SDRPLAY_H_

#include "input_source.h"
#include "sdrplay_api.h"
#include <stdint.h>
#include "argparse.h"

#if defined(_WIN32) && defined(WITH_SDRPLAY)

#include <windows.h> // Needed for HINSTANCE and FARPROC
#include <stdbool.h>

// A struct to hold all the function pointers we need from the DLL
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

// Global variable to hold our function pointers
extern SdrplayApiFunctionPointers sdrplay_api;

// Functions to load and unload the API
bool sdrplay_load_api(void);
void sdrplay_unload_api(void);

// --- MACRO REDIRECTION ---
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

#endif // defined(_WIN32) && defined(WITH_SDRPLAY)

InputSourceOps* get_sdrplay_input_ops(void);

const struct argparse_option* sdrplay_get_cli_options(int* count);

void sdrplay_set_default_config(AppConfig* config);

const char* get_sdrplay_device_name(uint8_t hwVer);

void sdrplay_stream_callback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params, unsigned int numSamples, unsigned int reset, void *cbContext);
void sdrplay_event_callback(sdrplay_api_EventT eventId, sdrplay_api_TunerSelectT tuner, sdrplay_api_EventParamsT *params, void *cbContext);

#endif // INPUT_SDRPLAY_H_
