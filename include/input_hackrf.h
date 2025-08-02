// input_hackrf.h
#ifndef INPUT_HACKRF_H_
#define INPUT_HACKRF_H_

#include <stdbool.h>
#include <hackrf.h> // This provides hackrf_transfer, hackrf_device, etc.
#include "input_source.h" // ADDED: Include the generic input source interface

// REMOVED: bool hackrf_initialize_device(struct AppConfig *config, struct AppResources *resources);
// REMOVED: void hackrf_cleanup_device(struct AppResources *resources);

/**
 * @brief Returns a pointer to the InputSourceOps struct that implements
 *        the input source interface for HackRF device input.
 *
 * @return A pointer to the static InputSourceOps struct for HackRF.
 */
InputSourceOps* get_hackrf_input_ops(void);

/**
 * @brief The callback function passed to libhackrf to handle incoming samples.
 *        (Remains public as it's called by the libhackrf library).
 */
int hackrf_stream_callback(hackrf_transfer* transfer);

#endif // INPUT_HACKRF_H_
