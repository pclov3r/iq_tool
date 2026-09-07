#ifndef DSP_IQ_CORRECTION_H
#define DSP_IQ_CORRECTION_H

#include "app_context.h"
#include "module.h"

const struct DspModuleInterface *dsp_iq_correct_get_api(void);

void iq_correction_run_estimation(void *state, const ComplexFloat *samples);

bool iq_correction_run_initial_calibration(
    struct ModuleContext *context, void *raw_buffer, size_t num_bytes,
    size_t (*read_cb)(void *user_data, void *buffer, size_t bytes),
    void *user_data);

#endif // DSP_IQ_CORRECTION_H
