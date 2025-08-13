#ifndef INPUT_BLADERF_H_
#define INPUT_BLADERF_H_

#include "input_source.h"
#include <libbladeRF.h>
#include "argparse.h" // <<< ADDED

#if defined(_WIN32) && defined(WITH_BLADERF)
#include <windows.h>
#include <stdbool.h>

typedef struct {
    HINSTANCE dll_handle;
    // --- FIX: Reverted to log_set_verbosity ---
    void (*log_set_verbosity)(bladerf_log_level);
    int (*open)(struct bladerf **, const char *);
    void (*close)(struct bladerf *);
    const char * (*get_board_name)(struct bladerf *);
    int (*get_serial)(struct bladerf *, char *);
    int (*is_fpga_configured)(struct bladerf *);
    int (*get_fpga_size)(struct bladerf *, bladerf_fpga_size *);
    int (*load_fpga)(struct bladerf *, const char *);
    int (*set_sample_rate)(struct bladerf *, bladerf_channel, bladerf_sample_rate, bladerf_sample_rate *);
    int (*set_bandwidth)(struct bladerf *, bladerf_channel, bladerf_bandwidth, bladerf_bandwidth *);
    int (*set_frequency)(struct bladerf *, bladerf_channel, bladerf_frequency);
    int (*set_gain_mode)(struct bladerf *, bladerf_channel, bladerf_gain_mode);
    int (*set_gain)(struct bladerf *, bladerf_channel, int);
    int (*set_bias_tee)(struct bladerf *, bladerf_channel, bool);
    int (*sync_config)(struct bladerf *, bladerf_channel_layout, bladerf_format, unsigned int, unsigned int, unsigned int, unsigned int);
    int (*enable_module)(struct bladerf *, bladerf_module, bool);
    int (*sync_rx)(struct bladerf *, void *, unsigned int, struct bladerf_metadata *, unsigned int);
    const char * (*strerror)(int);
} BladerfApiFunctionPointers;

// Global variable to hold our function pointers
extern BladerfApiFunctionPointers bladerf_api;

// Functions to load and unload the API
bool bladerf_load_api(void);
void bladerf_unload_api(void);

// --- MACRO REDIRECTION ---
#define bladerf_log_set_verbosity   bladerf_api.log_set_verbosity
#define bladerf_open            bladerf_api.open
#define bladerf_close           bladerf_api.close
#define bladerf_get_board_name  bladerf_api.get_board_name
#define bladerf_get_serial      bladerf_api.get_serial
#define bladerf_is_fpga_configured bladerf_api.is_fpga_configured
#define bladerf_get_fpga_size   bladerf_api.get_fpga_size
#define bladerf_load_fpga       bladerf_api.load_fpga
#define bladerf_set_sample_rate bladerf_api.set_sample_rate
#define bladerf_set_bandwidth   bladerf_api.set_bandwidth
#define bladerf_set_frequency   bladerf_api.set_frequency
#define bladerf_set_gain_mode   bladerf_api.set_gain_mode
#define bladerf_set_gain        bladerf_api.set_gain
#define bladerf_set_bias_tee    bladerf_api.set_bias_tee
#define bladerf_sync_config     bladerf_api.sync_config
#define bladerf_enable_module   bladerf_api.enable_module
#define bladerf_sync_rx         bladerf_api.sync_rx
#define bladerf_strerror        bladerf_api.strerror

#endif // defined(_WIN32) && defined(WITH_BLADERF)

InputSourceOps* get_bladerf_input_ops(void);

/**
 * @brief Returns the command-line options specific to the BladeRF module.
 * @param count A pointer to an integer that will be filled with the number of options.
 * @return A pointer to a static array of argparse_option structs.
 */
const struct argparse_option* bladerf_get_cli_options(int* count);

/**
 * @brief Sets the default configuration values for the BladeRF module.
 */
void bladerf_set_default_config(AppConfig* config);

#endif // INPUT_BLADERF_H_
