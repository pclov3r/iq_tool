/**
 * @file libredsea.c
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

#include "libredsea.h"
#include "rds_demodulator.h"
#include "block_sync.h"
#include "group_types.h"
#include "mem_arena.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint16_t ecc;
    const char *cc_strs[15];
} CountryCodeEntry;

static const CountryCodeEntry country_codes[] = {
    {0xA0, {"us", "us", "us", "us", "us", "us", "us", "us", "us", "us", "us", "--", "us", "us", "--"}},
    {0xA1, {"--", "--", "--", "--", "--", "--", "--", "--", "--", "--", "ca", "ca", "ca", "ca", "gl"}},
    {0xA2, {"ai", "ag", "ec", "fk", "bb", "bz", "ky", "cr", "cu", "ar", "br", "bm", "an", "gp", "bs"}},
    {0xA3, {"bo", "co", "jm", "mq", "gf", "py", "ni", "--", "pa", "dm", "do", "cl", "gd", "tc", "gy"}},
    {0xA4, {"gt", "hn", "aw", "--", "ms", "tt", "pe", "sr", "uy", "kn", "lc", "sv", "ht", "ve", "--"}},
    {0xA5, {"--", "--", "--", "--", "--", "--", "--", "--", "--", "--", "mx", "vc", "mx", "mx", "mx"}},
    {0xA6, {"--", "--", "--", "--", "--", "--", "--", "--", "--", "--", "--", "--", "--", "--", "pm"}},
    {0xD0, {"cm", "cf", "dj", "mg", "ml", "ao", "gq", "ga", "gn", "za", "bf", "cg", "tg", "bj", "mw"}},
    {0xD1, {"na", "lr", "gh", "mr", "st", "cv", "sn", "gm", "bi", "--", "bw", "km", "tz", "et", "bg"}},
    {0xD2, {"sl", "zw", "mz", "ug", "sz", "ke", "so", "ne", "td", "gw", "zr", "ci", "tz", "zm", "--"}},
    {0xD3, {"--", "--", "eh", "--", "rw", "ls", "--", "sc", "--", "mu", "--", "sd", "--", "--", "--"}},
    {0xE0, {"de", "dz", "ad", "il", "it", "be", "ru", "ps", "al", "at", "hu", "mt", "de", "--", "eg"}},
    {0xE1, {"gr", "cy", "sm", "ch", "jo", "fi", "lu", "bg", "dk", "gi", "iq", "gb", "ly", "ro", "fr"}},
    {0xE2, {"ma", "cz", "pl", "va", "sk", "sy", "tn", "--", "li", "is", "mc", "lt", "rs", "es", "no"}},
    {0xE3, {"me", "ie", "tr", "mk", "--", "--", "--", "nl", "lv", "lb", "az", "hr", "kz", "se", "by"}},
    {0xE4, {"md", "ee", "kg", "--", "--", "ua", "ks", "pt", "si", "am", "--", "ge", "--", "--", "ba"}},
    {0xF0, {"au", "au", "au", "au", "au", "au", "au", "au", "sa", "af", "mm", "cn", "kp", "bh", "my"}},
    {0xF1, {"ki", "bt", "bd", "pk", "fj", "om", "nr", "ir", "nz", "sb", "bn", "lk", "tw", "kr", "hk"}},
    {0xF2, {"kw", "qa", "kh", "ws", "in", "mo", "vn", "ph", "jp", "sg", "mv", "id", "ae", "np", "vu"}},
    {0xF3, {"la", "th", "to", "--", "--", "--", "--", "--", "pg", "--", "ye", "--", "--", "fm", "mn"}}};

void rds_get_country_string(uint16_t cc, uint16_t ecc, char *out_buffer, size_t buffer_size) {
    if (out_buffer == NULL || buffer_size == 0)
        return;
    strncpy(out_buffer, "--", buffer_size);

    for (size_t i = 0; i < sizeof(country_codes) / sizeof(CountryCodeEntry); i++) {
        if (country_codes[i].ecc == ecc) {
            if (cc > 0 && cc <= 15) {
                strncpy(out_buffer, country_codes[i].cc_strs[cc - 1], buffer_size);
                out_buffer[buffer_size - 1] = '\0';

                // Convert to uppercase
                for (int j = 0; out_buffer[j] != '\0'; j++) {
                    if (out_buffer[j] >= 'a' && out_buffer[j] <= 'z') {
                        out_buffer[j] -= 32;
                    }
                }

                return;
            }
        }
    }
}

struct RDSContext_T {
    RdsSubcarrierSet dsp;
    RdsBlockStream stream;
    bool is_rbds;
    RdsState state;
};

// Forward declaration
void rds_group_decode(RdsState *state, const RdsGroup *group);
void rds_group_extract(RdsGroup *group);

LibRedseaHandle libredsea_init(float sample_rate, bool is_rbds, void *arena) {
    struct RDSContext_T *ctx = (struct RDSContext_T *)mem_arena_alloc((MemoryArena *)arena, sizeof(struct RDSContext_T), true);
    if (!ctx)
        return NULL;

    rds_subcarrier_init(&ctx->dsp, sample_rate);
    rds_block_stream_init(&ctx->stream);
    ctx->is_rbds = is_rbds;
    memset(&ctx->state, 0, sizeof(RdsState));
    ctx->state.is_rbds = is_rbds;
    ctx->state.rt_ab_flag = -1;
    ctx->state.ptyn_ab_flag = -1;

    return ctx;
}

void libredsea_free(LibRedseaHandle context) {
    if (!context)
        return;
    // context struct is arena-owned; only free liquid-dsp internal objects.
    rds_subcarrier_free(&context->dsp);
}

void libredsea_process_mpx(LibRedseaHandle context, const float *mpx_data, int num_samples, RdsState *out_state) {
    if (!context || !mpx_data || !out_state || num_samples <= 0)
        return;

    RdsTimedBit out_bits[RDS_MAX_STREAMS][4096];
    int out_bit_counts[RDS_MAX_STREAMS];

    rds_subcarrier_process(&context->dsp, mpx_data, num_samples, 1, out_bits, out_bit_counts);

    // We only process stream 0 for standard RDS
    int bit_count = out_bit_counts[0];
    for (int i = 0; i < bit_count; i++) {
        rds_block_stream_push_bit(&context->stream, out_bits[0][i].bit);

        if (context->stream.has_group_ready) {
            RdsGroup group = rds_block_stream_pop_group(&context->stream);
            rds_group_extract(&group);
            rds_group_decode(&context->state, &group);
        }
    }

    *out_state = context->state;
}

void libredsea_clear_events(LibRedseaHandle context) {
    if (!context)
        return;
    context->state.tmc_event_count = 0;
    context->state.tdc_event_count = 0;
    context->state.iha_event_count = 0;
    context->state.rt_plus_event_count = 0;
}

float libredsea_get_bler(LibRedseaHandle context) {
    if (!context) return 0.0f;
    // error_sum_total counts bad blocks over the last 50 blocks
    return (context->stream.error_sum_total / 50.0f) * 100.0f;
}

bool libredsea_get_sync(LibRedseaHandle context) {
    if (!context) return false;
    return context->stream.is_in_sync;
}
