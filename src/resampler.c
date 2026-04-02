#include "resampler.h"
#include "constants.h"
#include "log.h"
#include "app_context.h"
#include <stdlib.h> // For exit()
#include <liquid.h>

// The resampler_s struct is just an alias for the liquid-dsp object.
// This definition is private to this .c file.
struct resampler_s {
    msresamp_crcf liquid_object;
};

Resampler* resampler_create(const AppConfig *config, AppContext* app, float resample_ratio) {
    (void)config; // config is not used here but kept for API consistency
    if (app->dsp.is_passthrough) {
        return NULL; // No resampler needed in passthrough mode.
    }

    // We cast the liquid-dsp object to our opaque type.
    Resampler* resampler = (Resampler*)msresamp_crcf_create(resample_ratio, RESAMPLER_QUALITY_ATTENUATION_DB);

    if (!resampler) {
        log_fatal("Error: Failed to create liquid-dsp resampler object.");
        return NULL;
    }
    return resampler;
}

void resampler_destroy(Resampler* resampler) {
    if (resampler) {
        // We cast our opaque type back to the liquid-dsp type to destroy it.
        msresamp_crcf_destroy((msresamp_crcf)resampler);
    }
}

void resampler_reset(Resampler* resampler) {
    if (resampler) {
        msresamp_crcf_reset((msresamp_crcf)resampler);
    }
}

void resampler_execute(Resampler* resampler, ComplexFloat* input, unsigned int num_input_frames, ComplexFloat* output, unsigned int* num_output_frames) {
    if (resampler) {
        msresamp_crcf_execute((msresamp_crcf)resampler, (liquid_float_complex*)input, num_input_frames, (liquid_float_complex*)output, num_output_frames);
    }
}
