/**
 * @file block_sync.h
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

#ifndef BLOCK_SYNC_H
#define BLOCK_SYNC_H

#include "group_types.h"

typedef struct {
    RdsOffset offset;
    uint32_t bit_position;
} RdsSyncPulse;

typedef struct {
    RdsSyncPulse pulses[4];
} RdsSyncPulseBuffer;

typedef struct {
    uint32_t bitcount;
    uint32_t num_bits_until_next_block;
    uint32_t input_register;
    RdsOffset expected_offset;
    bool is_in_sync;

    int error_sum_history[50];
    int error_sum_index;
    int error_sum_total;

    RdsGroup current_group;
    RdsGroup ready_group;
    bool has_group_ready;
    uint32_t num_bits_since_sync_lost;
    RdsSyncPulseBuffer sync_buffer;
} RdsBlockStream;

void rds_block_stream_init(RdsBlockStream *stream);
void rds_block_stream_push_bit(RdsBlockStream *stream, bool bit);
RdsGroup rds_block_stream_pop_group(RdsBlockStream *stream);

#endif // LIBREDSEA_BLOCK_SYNC_H
