#include "post_processor.h"
#include "filter.h"
#include "freq_shift.h"
#include "agc.h" // Added for Output AGC
#include "sample_convert.h"
#include "signal_handler.h"
#include "log.h"

void post_processor_apply_chain(DspContext* dsp, SampleChunk* item) {
    AppConfig* config = (AppConfig*)dsp->config;

    if (item->frames_to_write > 0) {
        // --- Stage Setup ---
        // The resampler thread has already set up the pointers for us:
        // - item->current_input_buffer points to the resampled data.
        // - item->current_output_buffer points to the free buffer, to be used as scratch space.

        // We use a local pointer to track the location of the valid data as it moves.
        ComplexFloat* current_data_ptr = item->current_input_buffer;

        // Step 1: Post-Resample Filtering (if enabled)
        if (dsp->filter.object && config->dsp.filter.apply_post_resample) {
            // Determine if the filter that will run is an out-of-place FFT filter.
            bool is_fft_filter = (dsp->filter.type_actual == FILTER_IMPL_FFT_SYMMETRIC ||
                                  dsp->filter.type_actual == FILTER_IMPL_FFT_ASYMMETRIC);

            item->frames_to_write = filter_apply(dsp, item, true);

            // --- CRITICAL FIX ---
            // If an out-of-place filter ran, its output is now in the 'output' buffer.
            // We must update our local pointer to track where the valid data is now located.
            if (is_fft_filter) {
                current_data_ptr = item->current_output_buffer;
            }
        }

        // Step 2: Post-Resample Frequency Shifting (if enabled)
        if (dsp->post_resample_nco) {
            // This is always an out-of-place operation. It reads from where the
            // valid data currently is (current_data_ptr) and writes to the other buffer.
            ComplexFloat* destination_buffer = (current_data_ptr == item->complex_sample_buffer_a)
                                                ? item->complex_sample_buffer_b
                                                : item->complex_sample_buffer_a;

            freq_shift_apply(dsp->post_resample_nco,
                             dsp->nco_shift_hz,
                             current_data_ptr,       // Input is the current valid data
                             destination_buffer,     // Output is the other buffer
                             item->frames_to_write);
 
            // The result is now in the destination buffer, so we update our local pointer.
            current_data_ptr = destination_buffer;
        }

        // Step 3: Output Automatic Gain Control (if enabled)
        // This runs in-place on the current data pointer.
        agc_apply(dsp, current_data_ptr, item->frames_to_write);

        // Step 3.5: Manual Output Gain (if configured)
        // Validation ensures this is mutually exclusive with AGC.
        if (config->dsp.output_gain != 1.0f) {
            float g = config->dsp.output_gain;
            for (unsigned int i = 0; i < item->frames_to_write; i++) {
                current_data_ptr[i] *= g;
            }
        }

        // Step 4: Final Sample Format Conversion
        // The current_data_ptr now points to the final, fully processed complex float data.
        if (!sample_convert_cf32_to_block(current_data_ptr,
                                   item->final_output_data,
                                   item->frames_to_write,
                                   config->output.format)) {
            log_fatal("Post-Processor: Failed to convert samples.");
            // Mark the chunk as having zero frames to prevent writing bad data
            item->frames_to_write = 0;
        }
    }
}

void post_processor_reset(DspContext* dsp) {
    freq_shift_reset_nco(dsp->post_resample_nco);
    filter_reset(dsp); // Filter reset is applicable to both stages
    agc_reset(dsp);    // Reset AGC state
}
