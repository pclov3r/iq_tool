#include "dc_block.h"
#include "constants.h"
#include "log.h"
#include "app_context.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <liquid.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


bool dc_block_create(AppConfig* config, AppContext* app) {
    if (!config->dsp.dc_block.enable) {
        app->dsp.dc_block.dc_block_filter = NULL; // Ensure no filter if disabled
        return true;
    }

    if (app->module.source_info.sample_rate <= 0.0) {
        log_error("DC Block: Cannot initialize with invalid sample rate (%.0f Hz).",
                  app->module.source_info.sample_rate);
        return false;
    }

    // Calculate normalized cutoff frequency based on the input sample rate.
    // This assumes the DC block is applied before resampling, so it uses the source_info.sample_rate.
    // liquid-dsp's iirfilt_crcf_create_dc_blocker expects a normalized bandwidth (alpha)
    // where alpha = 2 * pi * fc / Fs.
    // The documentation for iirfilt_crcf_create_dc_blocker states it creates a first-order
    // DC-blocking filter with transfer function H(z) = (1 - z^-1) / (1 - (1-alpha)z^-1).
    float normalized_alpha = (float)(2.0 * M_PI * DC_BLOCK_CUTOFF_HZ / app->module.source_info.sample_rate);

    // Ensure alpha is within a reasonable range (e.g., small positive value)
    // A very small alpha means a very narrow notch at DC, a larger alpha means wider.
    // It should be > 0.
    if (normalized_alpha <= 0.0f) {
        log_error("DC Block: Calculated normalized alpha (%.6f) is invalid. Ensure DC_BLOCK_CUTOFF_HZ > 0.", normalized_alpha);
        return false;
    }
    // Cap alpha to avoid excessively wide bandwidth for a DC block, though liquid-dsp handles it.
    if (normalized_alpha > 1.0f) { // Arbitrary upper bound for sanity, 1.0f is ~Fs/(2*pi)
        log_warn("DC Block: Calculated normalized alpha (%.6f) is very large. Consider reducing DC_BLOCK_CUTOFF_HZ.", normalized_alpha);
        // It's not a fatal error, but a warning. Continue.
    }

    // Create the IIR filter object.
    // NOTE: iirfilt_crcf_create_dc_blocker always creates a 1st-order filter,
    // regardless of the DC_BLOCK_FILTER_ORDER constant in config.h.
    // If a higher-order DC block were needed, a more general filter design
    // function (e.g., iirfilt_crcf_create_prototype with LIQUID_IIRDES_HIGHPASS)
    // would be required. For typical DC removal, 1st order is often sufficient.
    app->dsp.dc_block.dc_block_filter = (struct dc_blocker_s*)iirfilt_crcf_create_dc_blocker(normalized_alpha);

    if (!app->dsp.dc_block.dc_block_filter) {
        log_fatal("Failed to create liquid-dsp DC block filter.");
        return false;
    }

    log_info("DC Block enabled");
    log_debug("DC Block: Initialized with normalized_alpha = %.6f", normalized_alpha);


    return true;
}

void dc_block_reset(DspContext* dsp) {
    if (!dsp->config->dsp.dc_block.enable || !dsp->dc_block.dc_block_filter) {
        return; // DC block is disabled or not initialized
    }
    log_debug("DC block filter reset due to stream discontinuity.");
    iirfilt_crcf_reset((iirfilt_crcf)dsp->dc_block.dc_block_filter);
}

void dc_block_apply(DspContext* dsp, ComplexFloat* samples, int num_samples) {
    if (!dsp->config->dsp.dc_block.enable || !dsp->dc_block.dc_block_filter) {
        return; // DC block is disabled or not initialized
    }

    // Apply the filter in-place
    iirfilt_crcf_execute_block((iirfilt_crcf)dsp->dc_block.dc_block_filter,
                               (liquid_float_complex*)samples,
                               num_samples,
                               (liquid_float_complex*)samples);
}

void dc_block_destroy(AppContext* app) {
    if (app->dsp.dc_block.dc_block_filter) {
        iirfilt_crcf_destroy((iirfilt_crcf)app->dsp.dc_block.dc_block_filter);
        app->dsp.dc_block.dc_block_filter = NULL;
    }
}
