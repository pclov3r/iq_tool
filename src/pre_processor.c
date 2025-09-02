#include "pre_processor.h"
#include "dc_block.h"
#include "iq_correct.h"

void pre_processor_apply_chain(AppResources* resources, complex_float_t* samples, size_t num_samples) {
    // This is now the single, authoritative sequence for pre-processing.

    if (resources->config->dc_block.enable) {
        dc_block_apply(resources, samples, num_samples);
    }

    if (resources->config->iq_correction.enable) {
        // Note: We only APPLY the correction here. The optimization/training
        // happens in a separate, lower-priority thread.
        iq_correct_apply(resources, samples, num_samples);
    }

    // Future pre-processing steps (e.g., AGC) would go here.
}
