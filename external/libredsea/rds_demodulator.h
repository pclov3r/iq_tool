/**
 * @file subcarrier.h
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

#ifndef RDS_DEMODULATOR_H
#define RDS_DEMODULATOR_H

#include <complex.h>
#include <liquid.h>
#include <stdbool.h>
#include <stdint.h>

#define RDS_MAX_STREAMS 4
#define BPSK_TARGET_SAMPLE_RATE 171000.0f
#define RDS_BITRATE_BPS 1187.5f

// A timed bit from the demodulator
typedef struct {
    bool bit;
    float time_from_start;
} RdsTimedBit;

typedef struct {
    float complex prev_psk_symbol;
    float clock_history[128];
    uint32_t clock;
    uint32_t clock_polarity;
} RdsBiphaseDecoder;

typedef struct {
    bool prev_input;
} RdsDeltaDecoder;

typedef struct {
    agc_crcf agc;
    firfilt_crcf fir_lpf;
    symsync_crcf symsync;
    nco_crcf oscillator;
    modemcf modem; // BPSK

    RdsDeltaDecoder delta_decoder;
    RdsBiphaseDecoder biphase_decoder;
} RdsDemod;

typedef struct {
    uint32_t sample_num;
    uint32_t sample_num_since_reset;
    float resample_ratio;

    resamp_rrrf resampler;
    RdsDemod datastream_demods[RDS_MAX_STREAMS];

} RdsSubcarrierSet;

void rds_subcarrier_init(RdsSubcarrierSet *set, float samplerate);
void rds_subcarrier_free(RdsSubcarrierSet *set);
void rds_subcarrier_reset(RdsSubcarrierSet *set);

// Process a chunk and output bits.
// Since C doesn't have vectors, we output to a provided array.
// Returns the number of bits decoded in this chunk for each stream.
void rds_subcarrier_process(RdsSubcarrierSet *set, const float *mpx_data, int num_samples, int num_data_streams,
                            RdsTimedBit out_bits[RDS_MAX_STREAMS][4096],
                            int out_bit_counts[RDS_MAX_STREAMS]);

#endif // RDS_DEMODULATOR_H
