/*
 * Redsea C Wrapper Stub
 *
 * This file is used when WITH_REDSEA=OFF in CMake.
 * It provides empty implementations of the API to satisfy the linker.
 */

#include "redsea_wrapper.h"
#include <stddef.h>

RedseaHandle redsea_init(float sample_rate, bool is_rbds, bool show_partial) {
    (void)sample_rate;
    (void)is_rbds;
    (void)show_partial;
    return NULL;
}

void redsea_free(RedseaHandle context) {
    (void)context;
}

void redsea_process_mpx(RedseaHandle context, const float* mpx_data, int num_samples, RdsState* out_state) {
    (void)context;
    (void)mpx_data;
    (void)num_samples;

    // Always return invalid state so the UI stays silent
    if (out_state) {
        out_state->valid = false;
        out_state->pi_code = 0;
        out_state->ps_name[0] = '\0';
        out_state->radiotext[0] = '\0';
        out_state->program_type[0] = '\0';
        out_state->pty_name[0] = '\0'; // Initialize
        out_state->callsign[0] = '\0';
        out_state->alt_freq_count = 0;
        out_state->clock_time[0] = '\0';
        out_state->country_code[0] = '\0';
    }
}
