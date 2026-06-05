/**
 * @file subcarrier.c
 */

/*
 * Original Work Copyright (c) 2007-2016 Oona Räisänen OH2EIQ
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * -----------------------------------------------------------------------------
 *
 * Modifications Copyright (C) 2026 iq_tool
 *
 * The modifications to this file are licensed under the GNU General Public License v3.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "rds_demodulator.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define AGC_BANDWIDTH_HZ 500.0f
#define AGC_INITIAL_GAIN 0.08f
#define LOWPASS_CUTOFF_HZ 2400.0f
#define SYMSYNC_BANDWIDTH_HZ 2200.0f
#define SYMSYNC_DELAY 3
#define RESAMPLER_DELAY 13
#define SYMSYNC_BETA 0.8f
#define PLL_BANDWIDTH_HZ 0.03f
#define PLL_MULTIPLIER 12.0f
#define SAMPLES_PER_SYMBOL 3

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#ifndef M_2PI
#define M_2PI (2.0f * M_PI)
#endif

static inline float angular_freq(float hertz, float samplerate) { return hertz * M_2PI / samplerate; }

void rds_subcarrier_init(RdsSubcarrierSet *set, float samplerate) {
    memset(set, 0, sizeof(RdsSubcarrierSet));
    set->resample_ratio = BPSK_TARGET_SAMPLE_RATE / samplerate;
    set->resampler = resamp_rrrf_create(set->resample_ratio, RESAMPLER_DELAY, 0.45f, 60.0f, 32);

    for (int i = 0; i < RDS_MAX_STREAMS; i++) {
        RdsDemod *demod = &set->datastream_demods[i];
        demod->agc = agc_crcf_create();
        agc_crcf_set_bandwidth(demod->agc, AGC_BANDWIDTH_HZ / BPSK_TARGET_SAMPLE_RATE);

        demod->fir_lpf = firfilt_crcf_create_kaiser(255, LOWPASS_CUTOFF_HZ / BPSK_TARGET_SAMPLE_RATE, 60.0f, 0.0f);

        int samples_per_symbol = 3;
        demod->symsync = symsync_crcf_create_rnyquist(LIQUID_FIRFILT_RRC, samples_per_symbol, 3, 0.8f, 32);
        symsync_crcf_set_lf_bw(demod->symsync, 2200.0f / BPSK_TARGET_SAMPLE_RATE);
        symsync_crcf_set_output_rate(demod->symsync, 1);

        demod->oscillator = nco_crcf_create(LIQUID_NCO);
        nco_crcf_set_frequency(demod->oscillator, angular_freq(57000.0f, BPSK_TARGET_SAMPLE_RATE));
        nco_crcf_pll_set_bandwidth(demod->oscillator, PLL_BANDWIDTH_HZ / BPSK_TARGET_SAMPLE_RATE);

        demod->modem = modemcf_create(LIQUID_MODEM_PSK2);
    }
}

void rds_subcarrier_free(RdsSubcarrierSet *set) {
    if (!set)
        return;
    resamp_rrrf_destroy(set->resampler);
    for (int i = 0; i < RDS_MAX_STREAMS; i++) {
        agc_crcf_destroy(set->datastream_demods[i].agc);
        firfilt_crcf_destroy(set->datastream_demods[i].fir_lpf);
        symsync_crcf_destroy(set->datastream_demods[i].symsync);
        nco_crcf_destroy(set->datastream_demods[i].oscillator);
        modemcf_destroy(set->datastream_demods[i].modem);
    }
}

void rds_subcarrier_reset(RdsSubcarrierSet *set) {
    set->sample_num_since_reset = 0;
    for (int i = 0; i < RDS_MAX_STREAMS; i++) {
        symsync_crcf_reset(set->datastream_demods[i].symsync);
        nco_crcf_reset(set->datastream_demods[i].oscillator);
        nco_crcf_set_frequency(set->datastream_demods[i].oscillator,
                               angular_freq(57000.0f, BPSK_TARGET_SAMPLE_RATE));
    }
}

// Internal decoder logic
static bool biphase_push(RdsBiphaseDecoder *dec, float complex psk_symbol, bool *out_bit) {
    float complex biphase_symbol = (psk_symbol - dec->prev_psk_symbol) * 0.5f;
    bool result = crealf(biphase_symbol) >= 0.0f;
    bool has_value = (dec->clock % 2 == dec->clock_polarity);

    dec->prev_psk_symbol = psk_symbol;
    dec->clock_history[dec->clock] = fabsf(crealf(biphase_symbol));
    dec->clock++;

    if (dec->clock == 128) {
        float even_sum = 0, odd_sum = 0;
        for (int i = 0; i < 128; i += 2) {
            even_sum += dec->clock_history[i];
            odd_sum += dec->clock_history[i + 1];
        }
        if (even_sum > odd_sum)
            dec->clock_polarity = 0;
        else if (odd_sum > even_sum)
            dec->clock_polarity = 1;

        memset(dec->clock_history, 0, sizeof(dec->clock_history));
        dec->clock = 0;
    }

    if (has_value) {
        *out_bit = result;
        return true;
    }
    return false;
}

static bool delta_decode(RdsDeltaDecoder *dec, bool input_bit) {
    bool output_bit = (input_bit != dec->prev_input);
    dec->prev_input = input_bit;
    return output_bit;
}

void rds_subcarrier_process(RdsSubcarrierSet *set, const float *mpx_data, int num_samples, int num_data_streams,
                            RdsTimedBit out_bits[RDS_MAX_STREAMS][4096],
                            int out_bit_counts[RDS_MAX_STREAMS]) {

    for (int i = 0; i < num_data_streams; i++)
        out_bit_counts[i] = 0;

    for (int i = 0; i < num_samples; i++) {
        float resamp_out[4];
        unsigned int num_resampled = 0;

        if (set->resample_ratio == 1.0f) {
            resamp_out[0] = mpx_data[i];
            num_resampled = 1;
        } else {
            resamp_rrrf_execute(set->resampler, mpx_data[i], resamp_out, &num_resampled);
        }

        const int kSamplesPerSymbol = 3;
        const int kDecimateRatio = (int)(BPSK_TARGET_SAMPLE_RATE / 1187.5f / 2.0f / kSamplesPerSymbol);
        int processing_delay = (int)(13 * set->resample_ratio + 127 + 1.5f * 3 * kDecimateRatio);

        for (unsigned int j = 0; j < num_resampled; j++) {
            float chunk_sample = resamp_out[j];

            for (int n = 0; n < num_data_streams; n++) {
                RdsDemod *demod = &set->datastream_demods[n];

                float complex sample_baseband;
                nco_crcf_mix_down(demod->oscillator, chunk_sample + 0.0f * I, &sample_baseband);

                firfilt_crcf_push(demod->fir_lpf, sample_baseband);

                if (set->sample_num % kDecimateRatio == 0) {
                    liquid_float_complex sample_lopass;
                    firfilt_crcf_execute(demod->fir_lpf, &sample_lopass);

                    agc_crcf_execute(demod->agc, sample_lopass, &sample_lopass);

                    liquid_float_complex symbols[4];
                    unsigned int num_symbols = 0;
                    symsync_crcf_execute(demod->symsync, &sample_lopass, 1, symbols, &num_symbols);

                    for (unsigned int s = 0; s < num_symbols; s++) {
                        unsigned int unused_sym_out;
                        modemcf_demodulate(demod->modem, symbols[s], &unused_sym_out);

                        float phase_error = modemcf_get_demodulator_phase_error(demod->modem);
                        if (phase_error > M_PI)
                            phase_error = M_PI;
                        if (phase_error < -M_PI)
                            phase_error = -M_PI;
                        nco_crcf_pll_step(demod->oscillator, phase_error * PLL_MULTIPLIER);

                        bool biphase_bit = false;
                        if (biphase_push(&demod->biphase_decoder, symbols[s], &biphase_bit)) {
                            bool bit = delta_decode(&demod->delta_decoder, biphase_bit);
                            int idx = out_bit_counts[n];
                            if (idx < 4096) {
                                out_bits[n][idx].bit = bit;
                                out_bits[n][idx].time_from_start =
                                    (float)((int)set->sample_num - processing_delay) / BPSK_TARGET_SAMPLE_RATE;
                                out_bit_counts[n]++;
                            }
                        }
                    }
                }
                nco_crcf_step(demod->oscillator);
            }
            set->sample_num++;
            set->sample_num_since_reset++;
        }
    }
}
