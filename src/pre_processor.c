#include "pre_processor.h"
#include "dc_block.h"
#include "iq_correct.h"
#include "frequency_shift.h"
#include "sample_convert.h"
#include "filter.h"
#include "signal_handler.h"
#include "log.h"

void pre_processor_apply_chain(AppResources* resources, SampleChunk* item) {
    AppConfig* config = (AppConfig*)resources->config;

    // --- Stage Setup ---
    // For the pre-processor, all operations happen in the first buffer.
    // We read raw data and write the final pre-processed result to buffer_a.
    // All intermediate steps operate in-place on buffer_a.
    item->current_input_buffer = item->complex_sample_buffer_a;
    item->current_output_buffer = item->complex_sample_buffer_a;

    // Step 1: Convert sample block to complex float
    if (!convert_block_to_cf32(item->raw_input_data, item->current_output_buffer,
                               item->frames_read, item->packet_sample_format, config->gain)) {
        handle_fatal_thread_error("Pre-Processor: Failed to convert samples.", resources);
        item->frames_read = 0;
        return;
    }

    // Step 2: DC Blocking (if enabled)
    if (config->dc_block.enable) {
        dc_block_apply(resources, item->current_output_buffer, item->frames_read);
    }

    // Step 3: I/Q Imbalance Correction (if enabled)
    if (config->iq_correction.enable) {
        iq_correct_apply(resources, item->current_output_buffer, item->frames_read);
    }

    // Step 4: Pre-Resample Frequency Shifting (if enabled)
    if (resources->pre_resample_nco) {
        // This is an in-place operation.
        freq_shift_apply(resources->pre_resample_nco,
                         resources->nco_shift_hz,
                         item->current_output_buffer,
                         item->current_output_buffer,
                         item->frames_read);
    }

    // Step 5: Pre-Resample Filtering (if enabled)
    if (resources->user_fir_filter_object && !config->apply_user_filter_post_resample) {
        // filter_apply will now correctly handle its internal state, whether
        // it's an in-place FIR or an out-of-place FFT. The thread function
        // is responsible for the final ping-pong swap if needed.
        item->frames_read = filter_apply(resources, item, false);
    }
}

void pre_processor_reset(AppResources* resources) {
    dc_block_reset(resources);
    freq_shift_reset_nco(resources->pre_resample_nco);
    filter_reset(resources);
}
