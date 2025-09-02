#include "resampler.h"
#include "constants.h"
#include "log.h"
#include "app_context.h"

// This is now the ONLY file in our application outside of the processing
// threads that knows about the specific liquid-dsp implementation.
#ifdef _WIN32
#include <liquid.h>
#else
#include <liquid/liquid.h>
#endif

// The resampler_s struct is just an alias for the liquid-dsp object.
// This definition is private to this .c file.
struct resampler_s {
    msresamp_crcf liquid_object;
};

resampler_t* create_resampler(const AppConfig *config, AppResources *resources, float resample_ratio) {
    (void)config; // config is not used here but kept for API consistency
    if (resources->is_passthrough) {
        return NULL; // No resampler needed in passthrough mode.
    }

    // We cast the liquid-dsp object to our opaque type.
    resampler_t* resampler = (resampler_t*)msresamp_crcf_create(resample_ratio, RESAMPLER_QUALITY_ATTENUATION_DB);

    if (!resampler) {
        log_fatal("Error: Failed to create liquid-dsp resampler object.");
        return NULL;
    }
    return resampler;
}

void destroy_resampler(resampler_t* resampler) {
    if (resampler) {
        // We cast our opaque type back to the liquid-dsp type to destroy it.
        msresamp_crcf_destroy((msresamp_crcf)resampler);
    }
}

void resampler_reset(resampler_t* resampler) {
    if (resampler) {
        msresamp_crcf_reset((msresamp_crcf)resampler);
    }
}

void resampler_execute(resampler_t* resampler, complex_float_t* input, unsigned int num_input_frames, complex_float_t* output, unsigned int* num_output_frames) {
    if (resampler) {
        msresamp_crcf_execute((msresamp_crcf)resampler, (liquid_float_complex*)input, num_input_frames, (liquid_float_complex*)output, num_output_frames);
    }
}
