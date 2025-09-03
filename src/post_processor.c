#include "post_processor.h"
#include "filter.h"
#include "frequency_shift.h"
#include "sample_convert.h"
#include "signal_handler.h"
#include "log.h"

void post_processor_apply_chain(AppResources* resources, SampleChunk* item) {
    AppConfig* config = (AppConfig*)resources->config;

    if (item->frames_to_write > 0) {
        complex_float_t* current_data_ptr = item->complex_resampled_data;

        // Step 1: Post-Resample Filtering (if enabled)
        if (resources->user_fir_filter_object && config->apply_user_filter_post_resample) {
            item->frames_to_write = filter_apply(resources, item, true);
        }

        // Step 2: Post-Resample Frequency Shifting (if enabled)
        if (resources->post_resample_nco) {
            // Use the scratch buffer for an out-of-place operation to be safe.
            freq_shift_apply(resources->post_resample_nco,
                             resources->nco_shift_hz,
                             current_data_ptr,
                             item->complex_scratch_data,
                             item->frames_to_write);
            // The result is now in the scratch buffer, so we point to it for the next stage.
            current_data_ptr = item->complex_scratch_data;
        }

        // Step 3: Final Sample Format Conversion
        if (!convert_cf32_to_block(current_data_ptr,
                                   item->final_output_data,
                                   item->frames_to_write,
                                   config->output_format)) {
            handle_fatal_thread_error("Post-Processor: Failed to convert samples.", resources);
            // Mark the chunk as having zero frames to prevent writing bad data
            item->frames_to_write = 0;
        }
    }
}

void post_processor_reset(AppResources* resources) {
    freq_shift_reset_nco(resources->post_resample_nco);
    filter_reset(resources); // Filter reset is applicable to both stages
}
