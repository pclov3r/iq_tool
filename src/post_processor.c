/**
 * @file post_processor.c
 */

#include "post_processor.h"
#include "filter.h"
#include "frequency_shift.h"
#include "agc.h"
#include "sample_convert.h"
#include "signal_handler.h"
#include "log.h"

void post_processor_apply_chain(DspContext* dsp, SampleChunk* item) {
    AppConfig* config = (AppConfig*)dsp->config;

    if (item->frames_to_write > 0) {
        // The resampler thread has safely placed the valid audio into post_resample_buffer.
        // We will execute the entire post-processing chain IN-PLACE on this buffer.
        ComplexFloat* buffer = item->post_resample_buffer;

        // Step 1: Post-Resample Frequency Shifting (NCO runs FIRST)
        if (dsp->post_resample_nco) {
            frequency_shift_apply(dsp->post_resample_nco,
                             dsp->nco_shift_hz,
                             buffer,  // Input
                             buffer,  // Output (In-place)
                             item->frames_to_write);
        }

        // Step 2: Post-Resample Filtering (Filter runs SECOND)
        if (dsp->filter.object && config->dsp.filter.apply_post_resample) {
            item->frames_to_write = filter_apply(dsp, item, true);
        }

        // Step 3: Output Automatic Gain Control (if enabled)
        agc_apply(dsp, buffer, item->frames_to_write);

        // Step 3.5: Manual Output Gain (if configured)
        if (config->dsp.output_gain != 1.0f) {
            float g = config->dsp.output_gain;
            for (unsigned int i = 0; i < item->frames_to_write; i++) {
                buffer[i] *= g;
            }
        }

        // Step 4: Final Sample Format Conversion
        if (!sample_convert_cf32_to_block(buffer,
                                   item->final_output_data,
                                   item->frames_to_write,
                                   config->output.sample_format)) {
            log_fatal("Post-Processor: Failed to convert samples.");
            item->frames_to_write = 0;
        }
    }
}

void post_processor_reset(DspContext* dsp) {
    frequency_shift_reset_nco(dsp->post_resample_nco);
    filter_reset(dsp);
    agc_reset(dsp);
}
