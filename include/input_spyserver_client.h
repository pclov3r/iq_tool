/**
 * @file input_spyserver_client.h
 * @brief Defines the public interface for the SpyServer network client input source.
 */

#ifndef INPUT_SPYSERVER_CLIENT_H_
#define INPUT_SPYSERVER_CLIENT_H_

#include "module.h"
#include "argparse.h"

// --- Forward Declaration ---
struct AppConfig;

// --- Function Declarations ---

/**
 * @brief Returns a pointer to the ModuleInterface struct that implements
 *        the input source interface for SpyServer client input.
 */
ModuleInterface* get_spyserver_client_input_module_api(void);

/**
 * @brief Returns the command-line options specific to the SpyServer client module.
 */
const struct argparse_option* spyserver_client_get_cli_options(int* count);

/**
 * @brief Sets the default configuration values for the SpyServer client module.
 */
void spyserver_client_set_default_config(struct AppConfig* config);

#endif // INPUT_SPYSERVER_CLIENT_H_
