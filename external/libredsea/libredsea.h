/**
 * @file libredsea.h
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

#ifndef LIBREDSEA_H
#define LIBREDSEA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


// --- RT+ Data Structures ---
typedef struct {
    int toggle;
    int item_running;
    char title[65];
    char artist[65];
    bool is_update;
} RdsRTPlusState;

// --- Extended Data Structures for RDS Groups ---

typedef struct {
    uint16_t location_id;
    uint16_t event_code;
    uint16_t extent;
    uint8_t direction;
    bool duration_flag;
    uint8_t duration;
    bool diversion_advised;
    char event_description[128]; // Raw event description from static table
    bool is_update;

    // Multi-group buffering
    uint8_t continuity_index;
    uint16_t multi_parts[5][2];
    bool multi_parts_received[5];

    // Supplementary
    uint16_t supplementary_code;
} RdsTmcState;

#define MAX_EON_NETWORKS 16

typedef struct {
    uint16_t pi;
    bool is_valid;
    bool is_update;

    // EON Flags
    bool tp;
    bool ta;
    uint8_t pty;

    // Program Service Name (Variant 0-3)
    char ps[9];
    bool ps_received[4]; // Track the 4 chunks
    bool ps_complete;

    // Alternative Frequencies
    int mapped_freq_khz;
    int alt_freqs[25];
    int alt_freq_count;
    int alt_freq_expected;
} RdsEonNetwork;

typedef struct {
    RdsEonNetwork networks[MAX_EON_NETWORKS];
} RdsEonState;

typedef struct {
    uint16_t app_id;     // AID
    uint16_t group_type; // e.g., 0x0A for 10A
} RdsOdaState;

typedef struct {
    uint8_t item_type_1;
    uint8_t item_type_2;
    uint8_t start_1;
    uint8_t length_1;
    uint8_t start_2;
    uint8_t length_2;
} RdsRtPlusState;

// --- Main RDS Decoder State ---
// Designed to be a drop-in replacement for RdsState while appending future fields

// Transparent Data Channel (Group 5A/5B)
typedef struct {
    uint8_t channel;
    uint8_t data[4];
    int data_length;
} RdsTdcEvent;

// In-House Applications (Group 6A/6B)
typedef struct {
    uint8_t address;
    uint8_t data[4];
    int data_length;
} RdsIhaEvent;

// RadioText Plus Event
typedef struct {
    char title[65];
    char artist[65];
} RdsRTPlusEvent;

// TMC Event
typedef struct {
    uint16_t location_id;
    uint16_t event_code;
    uint8_t extent;
    uint8_t direction;
    uint16_t duration;
    uint16_t supplementary_code;
    bool diversion_advised;
    char event_description[128];
} RdsTmcEvent;

typedef struct RdsState {
    // 1. Basic Identification (Compatible with iq_tool)
    uint16_t pi_code;      // Program Identification
    char callsign[64];     // RBDS Callsign
    char ps_name[9];       // Program Service Name (8 chars + null)
    char radiotext[65];    // RadioText (64 chars + null)
    char program_type[17]; // PTY Category (16 chars + null)
    char pty_name[9];      // PTY Name (PTYN) (8 chars + null)
    // 2. Dynamic ODA Routing
    uint16_t oda_app_for_group[32];
    bool valid;       // True if enough frames received
    bool is_rbds;     // True if using RBDS semantics
    bool is_music;    // True for music, false for speech
    bool tp;          // Traffic Program
    bool ta;          // Traffic Announcement
    int rt_ab_flag;   // RadioText A/B toggle flag
    int ptyn_ab_flag; // PTY Name A/B toggle flag

    RdsRTPlusState rt_plus; // RT+ Metadata

    // 3. Decoder Identification (DI) Flags
    bool stereo;     // Station broadcasting in Stereo
    bool compressed; // Audio is compressed
    bool dynamic;    // PTY is dynamic
    bool binaural;   // Artificial Head recording

    // 4. Extended Info
    char clock_time[128];   // ISO8601 or custom formatted time
    int alt_freqs[32];     // Alternative Frequencies
    int alt_freq_count;    // Count of AFs
    int alt_freq_expected; // Expected count of AFs
    char country_code[3];  // Country Code

    // ----------------------------------------------------
    // APPENDED FIELDS (Future Proofing for LibRedsea Groups)
    // ----------------------------------------------------
    uint16_t ecc;            // Extended Country Code
    char country_string[32]; // ECC Country String
    bool is_ecc_update;      // True if ECC just updated

    // Internal subsystem states
    RdsTmcState tmc;
    RdsEonState eon;

    // ----------------------------------------------------
    // MATHEMATICALLY SAFE EVENT BUFFERS (Max 11.4 per sec)
    // ----------------------------------------------------
#define RDS_MAX_EVENTS 16

    RdsTmcEvent tmc_events[RDS_MAX_EVENTS];
    int tmc_event_count;

    RdsTdcEvent tdc_events[RDS_MAX_EVENTS];
    int tdc_event_count;

    RdsIhaEvent iha_events[RDS_MAX_EVENTS];
    int iha_event_count;

    RdsRTPlusEvent rt_plus_events[RDS_MAX_EVENTS];
    int rt_plus_event_count;
} RdsState;

// Opaque context for the decoder
typedef struct RDSContext_T *LibRedseaHandle;

/**
 * @brief Initialize a new native C LibRedsea decoder.
 * @param sample_rate Sample rate of the MPX input buffer
 * @param is_rbds True for North American RBDS, False for World RDS
 * @param arena Pointer to a MemoryArena to allocate from (must outlive the handle)
 * @return Opaque handle to decoder instance
 */
LibRedseaHandle libredsea_init(float sample_rate, bool is_rbds, void *arena);

/**
 * @brief Process raw MPX audio and update the RDS state.
 * @param context Decoder handle
 * @param mpx_data Raw MPX samples (float)
 * @param num_samples Number of samples
 * @param out_state State struct populated with decoded RDS data
 */
void libredsea_process_mpx(LibRedseaHandle context, const float *mpx_data, int num_samples, RdsState *out_state);

/**
 * @brief Clear event buffers. Must be called after processing/printing events to avoid overflow.
 * @param context Decoder handle
 */
void libredsea_clear_events(LibRedseaHandle context);

// TMC Utilities
const char *get_tmc_event_description(uint16_t code);
const char *get_tmc_supplementary_description(uint16_t code);

// Country Utilities
void rds_get_country_string(uint16_t cc, uint16_t ecc, char *out_buffer, size_t buffer_size);

/**
 * @brief Get the Block Error Rate (BLER) percentage over the last 50 blocks
 * @param context Decoder handle
 * @return BLER percentage (0.0 to 100.0)
 */
float libredsea_get_bler(LibRedseaHandle context);

/**
 * @brief Check whether the block synchronizer is currently locked.
 * @param context Decoder handle
 * @return true if in sync, false if not synced
 */
bool libredsea_get_sync(LibRedseaHandle context);

/**
 * @brief Free the native C LibRedsea decoder.
 * @param context Decoder handle
 */
void libredsea_free(LibRedseaHandle context);


#endif // LIBREDSEA_H
