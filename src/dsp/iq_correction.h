/**
 * @file dsp/iq_correction.h
 * @brief Helper declarations for initial I/Q calibration.
 */

#ifndef DSP_IQ_CORRECTION_H_
#define DSP_IQ_CORRECTION_H_

#include <stdbool.h>
#include <stddef.h>

struct ModuleContext;

bool iq_correction_run_initial_calibration(
    struct ModuleContext *context,
    size_t (*read_callback)(void *user_data, void *buffer, size_t bytes),
    void *user_data);

#endif // DSP_IQ_CORRECTION_H_
