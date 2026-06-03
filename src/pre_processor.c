/**
 * @file pre_processor.c
 */

#include "pre_processor.h"
#include "dc_block.h"
#include "iq_correction.h"
#include "frequency_shift.h"
#include "sample_convert.h"
#include "filter.h"
#include "signal_handler.h"
#include "log.h"

void pre_processor_apply_chain(DspContext* dsp, SampleChunk* item) {
    AppConfig* config = (AppConfig*)dsp->config;

    // --- Stage Setup ---
    // For the pre-processor, all operations happen in the first buffer.
    // We read raw data and write the final pre-processed result to buffer_a.
    // All intermediate steps operate in-place on buffer_a.
    // Step 1: Convert sample block to complex float
    // UPDATED: Use input_gain instead of gain
    if (!sample_convert_block_to_cf32(item->raw_input_data, item->pre_resample_buffer,
                               item->frames_read, item->packet_sample_format, config->dsp.input_gain)) {
        log_fatal("Pre-Processor: Failed to convert samples.");
        item->frames_read = 0;
        return;
    }

    // Step 2: DC Blocking (if enabled)
    if (config->dsp.dc_block.enable) {
        dc_block_apply(dsp, item->pre_resample_buffer, item->frames_read);
    }

    // Step 3: I/Q Imbalance Correction (if enabled)
    if (config->dsp.iq_correction.enable) {
        iq_correction_apply(dsp, item->pre_resample_buffer, item->frames_read);
    }

    // Step 4: Pre-Resample Frequency Shifting (if enabled)
    if (dsp->pre_resample_nco) {
        // This is an in-place operation.
        frequency_shift_apply(dsp->pre_resample_nco,
                         dsp->nco_shift_hz,
                         item->pre_resample_buffer,
                         item->pre_resample_buffer,
                         item->frames_read);
    }

    // Step 5: Pre-Resample Filtering (if enabled)
    if (dsp->filter.object && !config->dsp.filter.apply_post_resample) {
        // filter_apply will now correctly handle its internal state, whether
        // it's an in-place FIR or an out-of-place FFT. The thread function
        // is responsible for the final ping-pong swap if needed.
        item->frames_read = filter_apply(dsp, item, false);
    }
}

void pre_processor_reset(DspContext* dsp) {
    dc_block_reset(dsp);
    frequency_shift_reset_nco(dsp->pre_resample_nco);
    filter_reset(dsp);
}
