/**
 * @file block_sync.c
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

#include "block_sync.h"
#include <string.h>

#define MAX_TOLERABLE_BLER 85
#define MAX_ERRORS_TOLERATED_OVER_50_BLOCKS (MAX_TOLERABLE_BLER / 2)
#define BLOCK_LENGTH 26
#define BLOCK_BITMASK ((1U << BLOCK_LENGTH) - 1U)
#define CHECKWORD_LENGTH 10

static RdsOffset get_offset_for_syndrome(uint32_t syndrome) {
    switch (syndrome) {
    case 0x3D8:
        return RDS_OFFSET_A;
    case 0x3D4:
        return RDS_OFFSET_B;
    case 0x25C:
        return RDS_OFFSET_C;
    case 0x3CC:
        return RDS_OFFSET_CPRIME;
    case 0x258:
        return RDS_OFFSET_D;
    default:
        return RDS_OFFSET_INVALID;
    }
}

static uint32_t calculate_syndrome(uint32_t input_vector) {
    // 16 Information Data Bits (Reversed to map directly to k = 0..15)
    static const uint32_t generator_matrix[16] = {
        0x31B, 0x38F, 0x2A7, 0x0F7, 
        0x1EE, 0x3DC, 0x201, 0x1BB, 
        0x376, 0x355, 0x313, 0x39F, 
        0x287, 0x0B7, 0x16E, 0x2DC
    };

    // The top 10 parity bits map exactly to the 0x001 -> 0x200 identity matrix.
    // We can extract them directly without looping.
    uint32_t result = (input_vector >> 16) & 0x3FF;

    // Only loop through the 16 data bits
    for (int k = 0; k < 16; k++) {
        if ((input_vector >> k) & 1U) {
            result ^= generator_matrix[k];
        }
    }
    
    return result;
}

typedef struct {
    uint32_t syndrome;
    uint32_t error_vector;
} RdsErrorLookupEntry;

static RdsErrorLookupEntry error_lookup_table[5][52];
static bool error_lookup_table_initialized = false;

static void init_error_lookup_table() {
    if (error_lookup_table_initialized)
        return;
    uint32_t offset_words[5] = {
        0x0FC, // A
        0x198, // B
        0x168, // C
        0x350, // Cprime
        0x1B4  // D
    };

    for (int offset = 0; offset < 5; offset++) {
        int entry_idx = 0;
        uint32_t error_bits_arr[2] = {0x1U, 0x3U};
        for (int i = 0; i < 2; i++) {
            uint32_t error_bits = error_bits_arr[i];
            for (int shift = 0; shift < 26; shift++) {
                uint32_t error_vector = (error_bits << shift) & BLOCK_BITMASK;
                uint32_t syndrome = calculate_syndrome(error_vector ^ offset_words[offset]);
                error_lookup_table[offset][entry_idx].syndrome = syndrome;
                error_lookup_table[offset][entry_idx].error_vector = error_vector;
                entry_idx++;
            }
        }
    }
    error_lookup_table_initialized = true;
}

static bool correct_burst_errors(uint32_t raw, RdsOffset expected_offset, uint32_t *corrected_bits) {
    if (expected_offset == RDS_OFFSET_INVALID)
        return false;
    uint32_t syndrome = calculate_syndrome(raw);
    *corrected_bits = raw;

    for (int i = 0; i < 52; i++) {
        if (error_lookup_table[expected_offset][i].syndrome == syndrome) {
            *corrected_bits ^= error_lookup_table[expected_offset][i].error_vector;
            return true;
        }
    }
    return false;
}

static bool sync_pulse_could_follow(const RdsSyncPulse *a, const RdsSyncPulse *b) {
    uint32_t sync_distance = a->bit_position - b->bit_position;
    return (sync_distance % BLOCK_LENGTH == 0) && (sync_distance / BLOCK_LENGTH <= 6) &&
           (a->offset != RDS_OFFSET_INVALID) && (b->offset != RDS_OFFSET_INVALID) &&
           ((rds_get_block_number_for_offset(b->offset) + sync_distance / BLOCK_LENGTH) % 4 ==
            rds_get_block_number_for_offset(a->offset));
}

static void sync_pulse_buffer_push(RdsSyncPulseBuffer *buffer, RdsOffset offset, uint32_t bitcount) {
    for (int i = 0; i < 3; i++) {
        buffer->pulses[i] = buffer->pulses[i + 1];
    }
    buffer->pulses[3].offset = offset;
    buffer->pulses[3].bit_position = bitcount;
}

static bool sync_pulse_buffer_is_sequence_found(const RdsSyncPulseBuffer *buffer) {
    const RdsSyncPulse *third = &buffer->pulses[3];
    for (int i = 0; i < 2; i++) {
        for (int j = i + 1; j < 3; j++) {
            if (sync_pulse_could_follow(third, &buffer->pulses[j]) &&
                sync_pulse_could_follow(&buffer->pulses[j], &buffer->pulses[i])) {
                return true;
            }
        }
    }
    return false;
}

void rds_block_stream_init(RdsBlockStream *stream) {
    memset(stream, 0, sizeof(RdsBlockStream));
    stream->num_bits_until_next_block = 1;
    stream->expected_offset = RDS_OFFSET_A;
    init_error_lookup_table();
}

static void acquire_sync(RdsBlockStream *stream, RdsOffset offset) {
    if (stream->is_in_sync)
        return;
    stream->num_bits_since_sync_lost++;

    if (offset != RDS_OFFSET_INVALID) {
        sync_pulse_buffer_push(&stream->sync_buffer, offset, stream->bitcount);
        if (sync_pulse_buffer_is_sequence_found(&stream->sync_buffer)) {
            stream->is_in_sync = true;
            stream->expected_offset = offset;
            memset(&stream->current_group, 0, sizeof(RdsGroup));
            stream->num_bits_since_sync_lost = 0;
        }
    }
}

static void find_block_in_input_register(RdsBlockStream *stream) {
    uint32_t raw = stream->input_register & BLOCK_BITMASK;
    uint32_t syndrome = calculate_syndrome(raw);
    RdsOffset offset = get_offset_for_syndrome(syndrome);

    acquire_sync(stream, offset);

    if (stream->is_in_sync) {
        if (stream->expected_offset == RDS_OFFSET_C && offset == RDS_OFFSET_CPRIME) {
            stream->expected_offset = RDS_OFFSET_CPRIME;
        }

        bool had_errors = (offset != stream->expected_offset);

        if (had_errors) {
            uint32_t corrected_bits;
            if (correct_burst_errors(raw, stream->expected_offset, &corrected_bits)) {
                raw = corrected_bits;
                offset = stream->expected_offset;
            }
        }

        stream->error_sum_total -= stream->error_sum_history[stream->error_sum_index];
        stream->error_sum_history[stream->error_sum_index] = had_errors ? 1 : 0;
        stream->error_sum_total += stream->error_sum_history[stream->error_sum_index];
        stream->error_sum_index = (stream->error_sum_index + 1) % 50;

        if (stream->error_sum_total > MAX_ERRORS_TOLERATED_OVER_50_BLOCKS) {
            stream->is_in_sync = false;
            stream->error_sum_total = 0;
            memset(stream->error_sum_history, 0, sizeof(stream->error_sum_history));
            return;
        }

        RdsBlock block;
        block.data_raw = raw;
        block.data = (uint16_t)(raw >> CHECKWORD_LENGTH);
        block.is_received = false;

        if (offset == stream->expected_offset) {
            block.is_received = true;
            stream->current_group.blocks[rds_get_block_number_for_offset(stream->expected_offset)] = block;
        }

        RdsOffset next_offset = rds_get_next_offset(stream->expected_offset);
        if (next_offset == RDS_OFFSET_A) {
            stream->ready_group = stream->current_group;
            stream->has_group_ready = true;
            memset(&stream->current_group, 0, sizeof(RdsGroup));
        }

        stream->expected_offset = next_offset;
    }
}

void rds_block_stream_push_bit(RdsBlockStream *stream, bool bit) {
    stream->input_register = (stream->input_register << 1U) | (bit ? 1 : 0);
    stream->num_bits_until_next_block--;
    stream->bitcount++;

    if (stream->num_bits_until_next_block == 0) {
        find_block_in_input_register(stream);
        stream->num_bits_until_next_block = stream->is_in_sync ? BLOCK_LENGTH : 1;
    }
}

RdsGroup rds_block_stream_pop_group(RdsBlockStream *stream) {
    stream->has_group_ready = false;
    return stream->ready_group;
}
