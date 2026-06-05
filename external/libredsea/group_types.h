/**
 * @file group_types.h
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

#ifndef GROUP_TYPES_H
#define GROUP_TYPES_H

#include <stdbool.h>
#include <stdint.h>

// 16-bit blocks A, B, C, D
typedef struct {
    uint32_t data_raw; // 26 bits
    uint16_t data;     // 16 bits payload
    uint32_t bitcount;
    bool is_received;
    bool had_errors;
} RdsBlock;

typedef enum {
    RDS_OFFSET_A,
    RDS_OFFSET_B,
    RDS_OFFSET_C,
    RDS_OFFSET_CPRIME,
    RDS_OFFSET_D,
    RDS_OFFSET_INVALID
} RdsOffset;

typedef enum { RDS_BLOCK_1, RDS_BLOCK_2, RDS_BLOCK_3, RDS_BLOCK_4 } RdsBlockNumber;

typedef struct {
    RdsBlock blocks[4];
    uint16_t pi;
    bool has_pi;
    bool has_c_prime;

    // Extracted Group Info
    uint16_t type;     // e.g. 0 for 0A, 2 for 2A, etc
    bool is_version_b; // A or B version

    float time_received;
} RdsGroup;

static inline RdsBlockNumber rds_get_block_number_for_offset(RdsOffset offset) {
    switch (offset) {
    case RDS_OFFSET_A:
        return RDS_BLOCK_1;
    case RDS_OFFSET_B:
        return RDS_BLOCK_2;
    case RDS_OFFSET_C:
    case RDS_OFFSET_CPRIME:
        return RDS_BLOCK_3;
    case RDS_OFFSET_D:
        return RDS_BLOCK_4;
    default:
        return RDS_BLOCK_1;
    }
}

static inline RdsOffset rds_get_c_offset(bool is_c_prime) { return is_c_prime ? RDS_OFFSET_CPRIME : RDS_OFFSET_C; }

// Internal Pipeline Functions
struct RdsState;
void rds_group_extract(RdsGroup *group);
void rds_group_decode(struct RdsState *state, const RdsGroup *group);

static inline RdsOffset rds_get_next_offset(RdsOffset offset) {
    switch (offset) {
    case RDS_OFFSET_A:
        return RDS_OFFSET_B;
    case RDS_OFFSET_B:
        return RDS_OFFSET_C;
    case RDS_OFFSET_C:
    case RDS_OFFSET_CPRIME:
        return RDS_OFFSET_D;
    case RDS_OFFSET_D:
        return RDS_OFFSET_A;
    default:
        return RDS_OFFSET_A;
    }
}

#endif // LIBREDSEA_GROUP_H
