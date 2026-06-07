/**
 * @file group_decoder.c
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

#include "group_types.h"
#include "libredsea.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define timegm _mkgmtime
#endif

void rds_group_extract(RdsGroup *group) {
    if (!group->blocks[RDS_BLOCK_1].is_received) {
        group->has_pi = false;
    } else {
        group->pi = group->blocks[RDS_BLOCK_1].data;
        group->has_pi = true;
    }

    if (group->blocks[RDS_BLOCK_2].is_received) {
        uint16_t b2 = group->blocks[RDS_BLOCK_2].data;
        group->type = (b2 >> 12) & 0x0F;
        group->is_version_b = ((b2 >> 11) & 0x01) != 0;
    } else {
        group->type = 0xFFFF; // Unknown
    }
}

static void decode_0a(RdsState *state, const RdsGroup *group) {
    if (!group->blocks[RDS_BLOCK_2].is_received)
        return;

    uint16_t b2  = group->blocks[RDS_BLOCK_2].data;

    // PS Segment (0-3)
    int segment  = b2 & 0x03;

    // DI flags
    int di_bit   = (b2 >> 2) & 0x01; // DI value is at bit 2, addressing is segment

    switch (segment) {
    case 0: state->dynamic    = di_bit; break; // d0: Dynamic PTY (SA=0)
    case 1: state->compressed = di_bit; break; // d1: Compressed  (SA=1)
    case 2: state->binaural   = di_bit; break; // d2: Art. Head   (SA=2)
    case 3: state->stereo     = di_bit; break; // d3: Stereo      (SA=3)
    }

    // TA / Music
    state->ta       = (b2 >> 4) & 0x01;
    state->is_music = (b2 >> 3) & 0x01;

    // AF in Block 3 (if 0A)
    if (!group->is_version_b && group->blocks[RDS_BLOCK_3].is_received) {
        uint8_t af1 = (group->blocks[RDS_BLOCK_3].data >> 8) & 0xFF;
        uint8_t af2 = group->blocks[RDS_BLOCK_3].data & 0xFF;

        if (af1 >= 225 && af1 <= 249) {
            state->alt_freq_expected = af1 - 224;
            state->alt_freq_count = 0;
        } else if (af1 >= 1 && af1 <= 204 && state->alt_freq_expected > 0 &&
                   state->alt_freq_count < state->alt_freq_expected) {
            state->alt_freqs[state->alt_freq_count++] = 87500 + 100 * af1;
        }

        if (af2 >= 225 && af2 <= 249) {
            state->alt_freq_expected = af2 - 224;
            state->alt_freq_count = 0;
        } else if (af2 >= 1 && af2 <= 204 && state->alt_freq_expected > 0 &&
                   state->alt_freq_count < state->alt_freq_expected) {
            state->alt_freqs[state->alt_freq_count++] = 87500 + 100 * af2;
        }
    }

    // PS name in Block 4
    if (!group->blocks[RDS_BLOCK_4].is_received)
        return;

    uint16_t b4 = group->blocks[RDS_BLOCK_4].data;

    // PS chars
    char c1 = (b4 >> 8) & 0xFF;
    char c2 = b4 & 0xFF;

    // Proper line clearing per RDS specs: if segment 0 changes, the PS is new.
    // Clear the rest to prevent ghost characters.
    if (segment == 0 && (state->ps_name[0] != c1 || state->ps_name[1] != c2)) {
        memset(state->ps_name, 0, sizeof(state->ps_name));
    }

    state->ps_name[segment * 2]     = c1;
    state->ps_name[segment * 2 + 1] = c2;
}

static void decode_rt_plus(RdsState *state, const RdsGroup *group) {
    if (!group->blocks[RDS_BLOCK_2].is_received)
        return;
    uint16_t b2 = group->blocks[RDS_BLOCK_2].data;

    int item_toggle = (b2 >> 4) & 0x01;
    int item_running = (b2 >> 3) & 0x01;

    if (item_toggle != state->rt_plus.toggle || item_running != state->rt_plus.item_running) {
        state->rt_plus.toggle = item_toggle;
        state->rt_plus.item_running = item_running;
        memset(state->rt_plus.title, 0, sizeof(state->rt_plus.title));
        memset(state->rt_plus.artist, 0, sizeof(state->rt_plus.artist));
    }

    int num_tags = 0;
    if (group->blocks[RDS_BLOCK_3].is_received) {
        num_tags = group->blocks[RDS_BLOCK_4].is_received ? 2 : 1;
    }

    if (num_tags > 0) {
        uint16_t b3 = group->blocks[RDS_BLOCK_3].data;

        int tag1_type = ((b2 & 0x07) << 3) | ((b3 >> 13) & 0x07);
        int tag1_start = (b3 >> 7) & 0x3F;
        int tag1_length = ((b3 >> 1) & 0x3F) + 1;

        bool updated = false;

        if (tag1_type == 1 || tag1_type == 4) { // 1=Title, 4=Artist
            char *target = (tag1_type == 1) ? state->rt_plus.title : state->rt_plus.artist;
            if (tag1_start + tag1_length <= 64) {
                strncpy(target, state->radiotext + tag1_start, tag1_length);
                target[tag1_length] = '\0';
                updated = true;
            }
        }

        if (num_tags == 2) {
            uint16_t b4 = group->blocks[RDS_BLOCK_4].data;
            int tag2_type = ((b3 & 0x01) << 5) | ((b4 >> 11) & 0x1F);
            int tag2_start = (b4 >> 5) & 0x3F;
            int tag2_length = (b4 & 0x1F) + 1;

            if (tag2_type == 1 || tag2_type == 4) {
                char *target = (tag2_type == 1) ? state->rt_plus.title : state->rt_plus.artist;
                if (tag2_start + tag2_length <= 64) {
                    strncpy(target, state->radiotext + tag2_start, tag2_length);
                    target[tag2_length] = '\0';
                    updated = true;
                }
            }
        }

        if (updated && state->rt_plus_event_count < RDS_MAX_EVENTS) {
            RdsRTPlusEvent *ev = &state->rt_plus_events[state->rt_plus_event_count++];
            strncpy(ev->title, state->rt_plus.title, sizeof(ev->title));
            strncpy(ev->artist, state->rt_plus.artist, sizeof(ev->artist));
        }
    }
}

static void decode_1a(RdsState *state, const RdsGroup *group) {
    if (!group->blocks[RDS_BLOCK_3].is_received || !group->blocks[RDS_BLOCK_4].is_received)
        return;

    // Block 4 has PIN (Program Item Number)
    uint16_t pin = group->blocks[RDS_BLOCK_4].data;
    if (pin != 0x0000) {
        // We could decode PIN here, but the main feature used is ECC.
    }

    if (!group->is_version_b) {
        uint16_t b3 = group->blocks[RDS_BLOCK_3].data;
        int slow_label_variant = (b3 >> 12) & 0x07;

        switch (slow_label_variant) {
        case 0: {
            state->ecc = b3 & 0xFF; // Extended Country Code
            uint16_t cc = (group->pi >> 12) & 0x0F;
            if (state->ecc != 0x00) {
                rds_get_country_string(cc, state->ecc, state->country_code, sizeof(state->country_code));
            }
            break;
        }
            // Case 1 is TMC ID, Case 3 is Language...
        }
    }
}

static void decode_2a(RdsState *state, const RdsGroup *group) {
    if (!group->blocks[RDS_BLOCK_2].is_received)
        return;

    uint16_t b2 = group->blocks[RDS_BLOCK_2].data;
    int text_segment = b2 & 0x0F; // 0-15
    int ab_flag = (b2 >> 4) & 0x01;

    if (state->rt_ab_flag != -1 && ab_flag != state->rt_ab_flag) {
        memset(state->radiotext, 0, sizeof(state->radiotext));
    }
    state->rt_ab_flag = ab_flag;

    // A version has 4 chars, B version has 2 chars
    if (!group->is_version_b) {
        if (group->blocks[RDS_BLOCK_3].is_received && group->blocks[RDS_BLOCK_4].is_received) {
            uint16_t b3 = group->blocks[RDS_BLOCK_3].data;
            uint16_t b4 = group->blocks[RDS_BLOCK_4].data;
            char c1 = (b3 >> 8) & 0xFF;
            char c2 = b3 & 0xFF;
            char c3 = (b4 >> 8) & 0xFF;
            char c4 = b4 & 0xFF;

            if (text_segment == 0 && (state->radiotext[0] != c1 || state->radiotext[1] != c2 ||
                                      state->radiotext[2] != c3 || state->radiotext[3] != c4)) {
                memset(state->radiotext, 0, sizeof(state->radiotext));
            }

            state->radiotext[text_segment * 4] = c1;
            state->radiotext[text_segment * 4 + 1] = c2;
            state->radiotext[text_segment * 4 + 2] = c3;
            state->radiotext[text_segment * 4 + 3] = c4;
        }
    } else {
        if (group->blocks[RDS_BLOCK_4].is_received) {
            uint16_t b4 = group->blocks[RDS_BLOCK_4].data;
            char c1 = (b4 >> 8) & 0xFF;
            char c2 = b4 & 0xFF;

            if (text_segment == 0 && (state->radiotext[0] != c1 || state->radiotext[1] != c2)) {
                memset(state->radiotext, 0, sizeof(state->radiotext));
            }

            state->radiotext[text_segment * 2] = c1;
            state->radiotext[text_segment * 2 + 1] = c2;
        }
    }
}

static void decode_10a(RdsState *state, const RdsGroup *group) {
    if (!group->blocks[RDS_BLOCK_2].is_received)
        return;
    uint16_t b2 = group->blocks[RDS_BLOCK_2].data;
    int segment = b2 & 0x01;
    int ab_flag = (b2 >> 4) & 0x01;

    if (state->ptyn_ab_flag != -1 && ab_flag != state->ptyn_ab_flag) {
        memset(state->pty_name, 0, sizeof(state->pty_name));
    }
    state->ptyn_ab_flag = ab_flag;

    if (group->blocks[RDS_BLOCK_3].is_received && group->blocks[RDS_BLOCK_4].is_received) {
        uint16_t b3 = group->blocks[RDS_BLOCK_3].data;
        uint16_t b4 = group->blocks[RDS_BLOCK_4].data;
        char c1 = (b3 >> 8) & 0xFF;
        char c2 = b3 & 0xFF;
        char c3 = (b4 >> 8) & 0xFF;
        char c4 = b4 & 0xFF;

        if (segment == 0 && (state->pty_name[0] != c1 || state->pty_name[1] != c2 || state->pty_name[2] != c3 ||
                             state->pty_name[3] != c4)) {
            memset(state->pty_name, 0, sizeof(state->pty_name));
        }

        state->pty_name[segment * 4] = c1;
        state->pty_name[segment * 4 + 1] = c2;
        state->pty_name[segment * 4 + 2] = c3;
        state->pty_name[segment * 4 + 3] = c4;
    }
}

// 3A = ODA Identification
static void decode_3a(RdsState *state, const RdsGroup *group) {
    if (!group->blocks[RDS_BLOCK_2].is_received || !group->blocks[RDS_BLOCK_3].is_received ||
        !group->blocks[RDS_BLOCK_4].is_received)
        return;

    uint16_t b2 = group->blocks[RDS_BLOCK_2].data;
    uint16_t b4 = group->blocks[RDS_BLOCK_4].data;

    uint8_t target_group = b2 & 0x1F;
    state->oda_app_for_group[target_group] = b4;
}

// 5A/5B = Transparent Data Channel (TDC)
static void decode_5(RdsState *state, const RdsGroup *group) {
    if (!group->blocks[RDS_BLOCK_2].is_received)
        return;
    if (state->tdc_event_count >= RDS_MAX_EVENTS)
        return;

    RdsTdcEvent *event = &state->tdc_events[state->tdc_event_count];
    event->channel = group->blocks[RDS_BLOCK_2].data & 0x1F;

    if (!group->is_version_b) {
        if (!group->blocks[RDS_BLOCK_3].is_received || !group->blocks[RDS_BLOCK_4].is_received)
            return;
        event->data[0] = (group->blocks[RDS_BLOCK_3].data >> 8) & 0xFF;
        event->data[1] = group->blocks[RDS_BLOCK_3].data & 0xFF;
        event->data[2] = (group->blocks[RDS_BLOCK_4].data >> 8) & 0xFF;
        event->data[3] = group->blocks[RDS_BLOCK_4].data & 0xFF;
        event->data_length = 4;
    } else {
        if (!group->blocks[RDS_BLOCK_4].is_received)
            return;
        event->data[0] = (group->blocks[RDS_BLOCK_4].data >> 8) & 0xFF;
        event->data[1] = group->blocks[RDS_BLOCK_4].data & 0xFF;
        event->data_length = 2;
    }
    state->tdc_event_count++;
}

// 6A/6B = In-House Applications (IHA)
static void decode_6(RdsState *state, const RdsGroup *group) {
    if (!group->blocks[RDS_BLOCK_2].is_received)
        return;
    if (state->iha_event_count >= RDS_MAX_EVENTS)
        return;

    RdsIhaEvent *event = &state->iha_events[state->iha_event_count];
    event->address = group->blocks[RDS_BLOCK_2].data & 0x1F;

    if (!group->is_version_b) {
        if (!group->blocks[RDS_BLOCK_3].is_received || !group->blocks[RDS_BLOCK_4].is_received)
            return;
        event->data[0] = (group->blocks[RDS_BLOCK_3].data >> 8) & 0xFF;
        event->data[1] = group->blocks[RDS_BLOCK_3].data & 0xFF;
        event->data[2] = (group->blocks[RDS_BLOCK_4].data >> 8) & 0xFF;
        event->data[3] = group->blocks[RDS_BLOCK_4].data & 0xFF;
        event->data_length = 4;
    } else {
        if (!group->blocks[RDS_BLOCK_4].is_received)
            return;
        event->data[0] = (group->blocks[RDS_BLOCK_4].data >> 8) & 0xFF;
        event->data[1] = group->blocks[RDS_BLOCK_4].data & 0xFF;
        event->data_length = 2;
    }
    state->iha_event_count++;
}

static void decode_4a(RdsState *state, const RdsGroup *group) {
    if (!group->blocks[RDS_BLOCK_2].is_received || !group->blocks[RDS_BLOCK_3].is_received ||
        !group->blocks[RDS_BLOCK_4].is_received)
        return;

    uint16_t b2 = group->blocks[RDS_BLOCK_2].data;
    uint16_t b3 = group->blocks[RDS_BLOCK_3].data;
    uint16_t b4 = group->blocks[RDS_BLOCK_4].data;

    int mjd = ((b2 & 0x03) << 15) | ((b3 >> 1) & 0x7FFF);

    // Invalid/uninitialized MJD check
    if (mjd < 15079)
        return;

    int hour_utc = ((b3 & 0x01) << 4) | ((b4 >> 12) & 0x0F);
    int minute_utc = (b4 >> 6) & 0x3F;
    int local_offset_sign = (b4 & 0x20) ? -1 : 1;
    int local_offset_half_hours = b4 & 0x1F;
    double local_offset = local_offset_sign * local_offset_half_hours / 2.0;

    int yp = (int)((mjd - 15078.2) / 365.25);
    int mp = (int)((mjd - 14956.1 - (int)(yp * 365.25)) / 30.6001);
    int day_utc = mjd - 14956 - (int)(yp * 365.25) - (int)(mp * 30.6001);

    if (mp == 14 || mp == 15) {
        yp += 1;
        mp -= 12;
    }
    int year_utc = yp + 1900;
    int month_utc = mp - 1;

    if (hour_utc <= 23 && minute_utc <= 59 && fabs(local_offset) <= 14.0) {
        struct tm utc_tm = {0};
        utc_tm.tm_year = year_utc - 1900;
        utc_tm.tm_mon = month_utc - 1;
        utc_tm.tm_mday = day_utc;
        utc_tm.tm_hour = hour_utc;
        utc_tm.tm_min = minute_utc;
        utc_tm.tm_sec = 0;

        time_t absolute_utc_time = timegm(&utc_tm);
        time_t station_local_time = absolute_utc_time + (time_t)(local_offset * 3600.0);

        struct tm *local_tm = gmtime(&station_local_time);

        time_t sys_now = time(NULL);
        double diff_seconds = difftime(absolute_utc_time, sys_now);
        int diff_mins = (int)(diff_seconds / 60.0);
        int diff_hours = diff_mins / 60;
        diff_mins = abs(diff_mins % 60);

        int am_pm_hour = local_tm->tm_hour % 12;
        if (am_pm_hour == 0)
            am_pm_hour = 12;
        const char *am_pm = local_tm->tm_hour >= 12 ? "PM" : "AM";

        char diff_str[32];
        if (diff_hours == 0 && diff_mins == 0) {
            snprintf(diff_str, sizeof(diff_str), "in sync");
        } else if (diff_hours == 0) {
            snprintf(diff_str, sizeof(diff_str), "%s%dm", diff_seconds > 0 ? "+" : "-", diff_mins);
        } else {
            snprintf(diff_str, sizeof(diff_str), "%s%dh %02dm", diff_seconds > 0 ? "+" : "-", abs(diff_hours),
                     diff_mins);
        }

        if (state->is_rbds) {
            snprintf(state->clock_time, sizeof(state->clock_time),
                     "Date=%02d/%02d/%02d | Time=%02d:%02d %s | Offset=%+.1fh | SysClkDiff=%s", local_tm->tm_mon + 1, local_tm->tm_mday,
                     (local_tm->tm_year + 1900) % 100, am_pm_hour, local_tm->tm_min, am_pm, local_offset, diff_str);
        } else {
            snprintf(state->clock_time, sizeof(state->clock_time),
                     "Date=%02d/%02d/%02d | Time=%02d:%02d %s | Offset=%+.1fh | SysClkDiff=%s", local_tm->tm_mday, local_tm->tm_mon + 1,
                     (local_tm->tm_year + 1900) % 100, am_pm_hour, local_tm->tm_min, am_pm, local_offset, diff_str);
        }
    }
}

// 8A = TMC
static void decode_8a(RdsState *state, const RdsGroup *group) {
    if (!group->blocks[RDS_BLOCK_2].is_received)
        return;

    uint16_t b2 = group->blocks[RDS_BLOCK_2].data;
    bool is_tuning = (b2 >> 4) & 0x01; // T bit

    // For now, we only handle User messages (T=0)
    if (!is_tuning && group->blocks[RDS_BLOCK_3].is_received && group->blocks[RDS_BLOCK_4].is_received) {
        uint16_t b3 = group->blocks[RDS_BLOCK_3].data;
        uint16_t b4 = group->blocks[RDS_BLOCK_4].data;

        // Check if single group
        bool is_single_group = (b2 >> 3) & 0x01;
        uint8_t continuity_index = b2 & 0x07;

        if (is_single_group) {
            if (state->tmc_event_count >= RDS_MAX_EVENTS)
                return;
            RdsTmcEvent *event = &state->tmc_events[state->tmc_event_count++];

            event->location_id = b4;
            event->event_code = b3 & 0x07FF;
            event->extent = (b3 >> 11) & 0x07;
            event->direction = (b3 >> 14) & 0x01;
            event->diversion_advised = (b3 >> 15) & 0x01;
            event->duration = continuity_index;
            event->supplementary_code = 0;

            const char *description = get_tmc_event_description(event->event_code);
            strncpy(event->event_description, description, sizeof(event->event_description) - 1);
            event->event_description[sizeof(event->event_description) - 1] = '\0';
        } else {
            // Multi-group handling (redsea style buffering)
            if (state->tmc.continuity_index != continuity_index && continuity_index != 0) {
                // Reset on discontinuity
                for (int i = 0; i < 5; i++)
                    state->tmc.multi_parts_received[i] = false;
            }
            state->tmc.continuity_index = continuity_index;

            bool is_first_group = (b3 >> 15) & 0x01;
            uint8_t current_group = 0;
            bool is_last_group = false;

            if (is_first_group) {
                current_group = 0;
            } else if ((b3 >> 14) & 0x01) { // SG bit
                uint8_t gsi = (b3 >> 12) & 0x03;
                current_group = 1;
                is_last_group = (gsi == 0);
            } else {
                uint8_t gsi = (b3 >> 12) & 0x03;
                current_group = 4 - gsi;
                is_last_group = (gsi == 0);
            }

            if (current_group < 5) {
                state->tmc.multi_parts[current_group][0] = b3;
                state->tmc.multi_parts[current_group][1] = b4;
                state->tmc.multi_parts_received[current_group] = true;
            }

            if (is_last_group) {
                if (state->tmc.multi_parts_received[0]) {
                    if (state->tmc_event_count >= RDS_MAX_EVENTS)
                        return;
                    RdsTmcEvent *event = &state->tmc_events[state->tmc_event_count++];

                    uint16_t first_b3 = state->tmc.multi_parts[0][0];
                    uint16_t first_b4 = state->tmc.multi_parts[0][1];

                    event->location_id = first_b4;
                    event->event_code = first_b3 & 0x07FF;
                    event->extent = (first_b3 >> 11) & 0x07;
                    event->direction = (first_b3 >> 14) & 0x01;

                    // Parse Freeform Fields for Duration and Diversion
                    uint8_t bits[128];
                    int bit_count = 0;

                    for (int i = 1; i < 5; i++) {
                        if (state->tmc.multi_parts_received[i]) {
                            uint16_t pb3 = state->tmc.multi_parts[i][0];
                            uint16_t pb4 = state->tmc.multi_parts[i][1];
                            for (int b = 11; b >= 0; b--)
                                bits[bit_count++] = (pb3 >> b) & 1;
                            for (int b = 15; b >= 0; b--)
                                bits[bit_count++] = (pb4 >> b) & 1;
                        }
                    }

                    int bit_pos = 0;
                    event->duration = 0;
                    event->diversion_advised = false;
                    event->supplementary_code = 0;

                    int field_sizes[] = {3, 3, 5, 5, 5, 8, 8, 8, 8, 11, 16, 16, 16, 16, 0, 0};

                    while (bit_pos + 4 <= bit_count) {
                        uint8_t label = (bits[bit_pos] << 3) | (bits[bit_pos + 1] << 2) | (bits[bit_pos + 2] << 1) |
                                        bits[bit_pos + 3];
                        bit_pos += 4;

                        int fsize = field_sizes[label];
                        if (bit_pos + fsize > bit_count)
                            break;

                        uint16_t field_data = 0;
                        for (int i = 0; i < fsize; i++) {
                            field_data = (field_data << 1) | bits[bit_pos++];
                        }

                        if (label == 0 && field_data == 0)
                            break; // End of freeform data

                        if (label == 0) { // Duration
                            event->duration = field_data;
                        } else if (label == 1) { // Control Code
                            if (field_data == 5)
                                event->diversion_advised = true; // SetDiversion is 5
                        } else if (label == 6) {              // Supplementary
                            event->supplementary_code = field_data;
                        }
                    }

                    const char *description = get_tmc_event_description(event->event_code);
                    strncpy(event->event_description, description, sizeof(event->event_description) - 1);
                    event->event_description[sizeof(event->event_description) - 1] = '\0';
                }

                // Clear buffer
                for (int i = 0; i < 5; i++)
                    state->tmc.multi_parts_received[i] = false;
            }
        }
    }
}

static void get_rbds_callsign(uint16_t pi, char *out_callsign, size_t out_length) {
    out_callsign[0] = '\0';
    if (out_length == 0)
        return;

    // Exceptions for zero nybbles
    if ((pi & 0xFFF0) == 0xAFA0 && (pi & 0x000F) < 0x000A) {
        pi <<= 12;
    } else if ((pi & 0xFF00) == 0xAF00) {
        pi <<= 8;
    } else if ((pi & 0xF000) == 0xA000) {
        pi = ((pi & 0x0F00) << 4) | (pi & 0x00FF);
    }

    if (pi >= 0x9950 && pi <= 0x9EFF) {
        static const struct {
            uint16_t pi;
            const char *callsign;
        } three_letter_codes[] = {
            {0x99A5, "KBW"}, {0x9992, "KOY"}, {0x9978, "WHO"}, {0x99A6, "KCY"}, {0x9993, "KPQ"}, {0x999C, "WHP"},
            {0x9990, "KDB"}, {0x9964, "KQV"}, {0x999D, "WIL"}, {0x99A7, "KDF"}, {0x9994, "KSD"}, {0x997A, "WIP"},
            {0x9950, "KEX"}, {0x9965, "KSL"}, {0x99B3, "WIS"}, {0x9951, "KFH"}, {0x9966, "KUJ"}, {0x997B, "WJR"},
            {0x9952, "KFI"}, {0x9995, "KUT"}, {0x99B4, "WJW"}, {0x9953, "KGA"}, {0x9967, "KVI"}, {0x99B5, "WJZ"},
            {0x9991, "KGB"}, {0x9968, "KWG"}, {0x997C, "WKY"}, {0x9954, "KGO"}, {0x9996, "KXL"}, {0x997D, "WLS"},
            {0x9955, "KGU"}, {0x9997, "KXO"}, {0x997E, "WLW"}, {0x9956, "KGW"}, {0x996B, "KYW"}, {0x999E, "WMC"},
            {0x9957, "KGY"}, {0x9999, "WBT"}, {0x999F, "WMT"}, {0x99AA, "KHQ"}, {0x996D, "WBZ"}, {0x9981, "WOC"},
            {0x9958, "KID"}, {0x996E, "WDZ"}, {0x99A0, "WOI"}, {0x9959, "KIT"}, {0x996F, "WEW"}, {0x9983, "WOL"},
            {0x995A, "KJR"}, {0x999A, "WGH"}, {0x9984, "WOR"}, {0x995B, "KLO"}, {0x9971, "WGL"}, {0x99A1, "WOW"},
            {0x995C, "KLZ"}, {0x9972, "WGN"}, {0x99B9, "WRC"}, {0x995D, "KMA"}, {0x9973, "WGR"}, {0x99A2, "WRR"},
            {0x995E, "KMJ"}, {0x999B, "WGY"}, {0x99A3, "WSB"}, {0x995F, "KNX"}, {0x9975, "WHA"}, {0x99A4, "WSM"},
            {0x9960, "KOA"}, {0x9976, "WHB"}, {0x9988, "WWJ"}, {0x99AB, "KOB"}, {0x9977, "WHK"}, {0x9989, "WWL"}};
        int n = sizeof(three_letter_codes) / sizeof(three_letter_codes[0]);
        for (int i = 0; i < n; i++) {
            if (three_letter_codes[i].pi == pi) {
                strncpy(out_callsign, three_letter_codes[i].callsign, out_length - 1);
                out_callsign[out_length - 1] = '\0';
                return;
            }
        }
    } else if ((pi >> 12) == 0xB || (pi >> 12) == 0xD || (pi >> 12) == 0xE) {
        static const struct {
            uint16_t pi;
            const char *callsign;
        } linked_station_codes[] = {{0xB001, "NPR-1"},
                                    {0xB002, "CBC English - Radio One"},
                                    {0xB003, "CBC English - Radio Two"},
                                    {0xB004, "CBC French => Radio-Canada - Première Chaîne"},
                                    {0xB005, "CBC French => Radio-Canada - Espace Musique"},
                                    {0xB006, "CBC"},
                                    {0xB007, "CBC"},
                                    {0xB008, "CBC"},
                                    {0xB009, "CBC"},
                                    {0xB00A, "NPR-2"},
                                    {0xB00B, "NPR-3"},
                                    {0xB00C, "NPR-4"},
                                    {0xB00D, "NPR-5"},
                                    {0xB00E, "NPR-6"}};
        uint16_t masked_pi = pi & 0xF0FF;
        int n = sizeof(linked_station_codes) / sizeof(linked_station_codes[0]);
        for (int i = 0; i < n; i++) {
            if (linked_station_codes[i].pi == masked_pi) {
                strncpy(out_callsign, linked_station_codes[i].callsign, out_length - 1);
                out_callsign[out_length - 1] = '\0';
                return;
            }
        }
    } else if (pi >= 0x1000 && pi <= 0x994F) {
        out_callsign[0] = (pi <= 0x54A7) ? 'K' : 'W';
        pi -= (pi <= 0x54A7) ? 0x1000 : 0x54A8;
        out_callsign[1] = 'A' + (pi / (26 * 26)) % 26;
        out_callsign[2] = 'A' + (pi / 26) % 26;
        out_callsign[3] = 'A' + (pi % 26);
        out_callsign[4] = '\0';
    }
}

static void decode_14(RdsState *state, const RdsGroup *group) {
    if (!group->blocks[3].is_received)
        return;
    uint16_t on_pi = group->blocks[3].data;

    // Find or allocate network in state->eon.networks
    int network_index = -1;
    for (int i = 0; i < MAX_EON_NETWORKS; i++) {
        if (state->eon.networks[i].is_valid && state->eon.networks[i].pi == on_pi) {
            network_index = i;
            break;
        }
    }
    if (network_index == -1) {
        for (int i = 0; i < MAX_EON_NETWORKS; i++) {
            if (!state->eon.networks[i].is_valid) {
                network_index = i;
                state->eon.networks[i].is_valid = true;
                state->eon.networks[i].pi = on_pi;
                memset(state->eon.networks[i].ps, ' ', 8);
                state->eon.networks[i].ps[8] = '\0';
                for (int j = 0; j < 4; j++)
                    state->eon.networks[i].ps_received[j] = false;
                state->eon.networks[i].ps_complete = false;
                state->eon.networks[i].alt_freq_count = 0;
                state->eon.networks[i].alt_freq_expected = 0;
                state->eon.networks[i].mapped_freq_khz = 0;
                break;
            }
        }
    }

    if (network_index == -1)
        return; // No space left

    RdsEonNetwork *network = &state->eon.networks[network_index];

    // ON_TP is in Block 2 bit 4
    if (group->blocks[1].is_received) {
        network->tp = (group->blocks[1].data >> 4) & 0x01;
    }

    if (group->is_version_b) {
        // 14B only has TA
        if (group->blocks[1].is_received) {
            network->ta = (group->blocks[1].data >> 3) & 0x01;
            network->is_update = true;
        }
        return;
    }

    if (!group->blocks[2].is_received)
        return;

    // 14A Variant
    uint16_t variant = group->blocks[1].data & 0x0F;

    switch (variant) {
    case 0:
    case 1:
    case 2:
    case 3: {
        uint8_t c1 = (group->blocks[2].data >> 8) & 0xFF;
        uint8_t c2 = group->blocks[2].data & 0xFF;
        network->ps[2 * variant] = (c1 >= 0x20 && c1 <= 0x7E) ? c1 : ' ';
        network->ps[2 * variant + 1] = (c2 >= 0x20 && c2 <= 0x7E) ? c2 : ' ';
        network->ps_received[variant] = true;

        if (network->ps_received[0] && network->ps_received[1] && network->ps_received[2] && network->ps_received[3]) {
            if (!network->ps_complete) {
                network->ps_complete = true;
                network->is_update = true;
            }
        }
        break;
    }
    case 4: {
        uint8_t af1 = (group->blocks[2].data >> 8) & 0xFF;
        uint8_t af2 = group->blocks[2].data & 0xFF;

        if (af1 >= 225 && af1 <= 249) {
            network->alt_freq_expected = af1 - 224;
            network->alt_freq_count = 0;
        } else if (af1 >= 1 && af1 <= 204 && network->alt_freq_expected > 0 &&
                   network->alt_freq_count < network->alt_freq_expected) {
            network->alt_freqs[network->alt_freq_count++] = 87500 + 100 * af1;
        }

        if (af2 >= 225 && af2 <= 249) {
            network->alt_freq_expected = af2 - 224;
            network->alt_freq_count = 0;
        } else if (af2 >= 1 && af2 <= 204 && network->alt_freq_expected > 0 &&
                   network->alt_freq_count < network->alt_freq_expected) {
            network->alt_freqs[network->alt_freq_count++] = 87500 + 100 * af2;
        }

        if (network->alt_freq_expected > 0 && network->alt_freq_count == network->alt_freq_expected) {
            network->is_update = true;
        }
        break;
    }
    case 5:
    case 6:
    case 7:
    case 8:
    case 9: {
        uint8_t mapped_freq = group->blocks[2].data & 0xFF;
        if (mapped_freq >= 1 && mapped_freq <= 204) {
            network->mapped_freq_khz = 87500 + 100 * mapped_freq;
            network->is_update = true;
        }
        break;
    }
    case 13: {
        network->pty = (group->blocks[2].data >> 11) & 0x1F;
        network->ta = group->blocks[2].data & 0x01;
        network->is_update = true;
        break;
    }
    }
}
// 15A/15B = Fast Basic Tuning
static void decode_15(RdsState *state, const RdsGroup *group) {
    if (!group->is_version_b) {
        // Group 15A: RDS2 Long PS
        // TODO: RDS2 currently has virtually no global adoption, so 15A is intentionally skipped.
        return;
    }

    // Group 15B: Fast basic tuning and switching information
    // Block 2 and Block 4 contain the exact same data payload!
    RdsBlockNumber block_num = group->blocks[RDS_BLOCK_2].is_received ? RDS_BLOCK_2 : RDS_BLOCK_4;

    if (group->blocks[block_num].is_received) {
        uint16_t block_data = group->blocks[block_num].data;

        uint16_t segment_address = block_data & 0x03;
        bool is_di = (block_data & 0x04) != 0;

        if (segment_address == 0)
            state->dynamic    = is_di; // d0: Dynamic PTY
        else if (segment_address == 1)
            state->compressed = is_di; // d1: Compressed
        else if (segment_address == 2)
            state->binaural   = is_di; // d2: Art. Head
        else if (segment_address == 3)
            state->stereo     = is_di; // d3: Stereo

        state->ta = (block_data & 0x10) != 0;
        state->is_music = (block_data & 0x08) != 0;
    }
}

void rds_group_decode(RdsState *state, const RdsGroup *group) {
    if (group->has_pi) {
        state->pi_code = group->pi;
        state->valid = true;
        if (state->is_rbds) {
            get_rbds_callsign(state->pi_code, state->callsign, sizeof(state->callsign));
        }
    }

    if (!group->blocks[RDS_BLOCK_2].is_received)
        return;

    uint16_t b2 = group->blocks[RDS_BLOCK_2].data;

    // PTY and TP are always present in Block 2
    state->tp = (b2 >> 10) & 0x01;
    int pty = (b2 >> 5) & 0x1F;

    // Update PTY string
    static const char *pty_names_rds[32] = {"No PTY",
                                            "News",
                                            "Current affairs",
                                            "Information",
                                            "Sport",
                                            "Education",
                                            "Drama",
                                            "Culture",
                                            "Science",
                                            "Varied",
                                            "Pop music",
                                            "Rock music",
                                            "Easy listening",
                                            "Light classical",
                                            "Serious classical",
                                            "Other music",
                                            "Weather",
                                            "Finance",
                                            "Children's programmes",
                                            "Social affairs",
                                            "Religion",
                                            "Phone-in",
                                            "Travel",
                                            "Leisure",
                                            "Jazz music",
                                            "Country music",
                                            "National music",
                                            "Oldies music",
                                            "Folk music",
                                            "Documentary",
                                            "Alarm test",
                                            "Alarm"};

    static const char *pty_names_rbds[32] = {"No PTY",
                                             "News",
                                             "Information",
                                             "Sports",
                                             "Talk",
                                             "Rock",
                                             "Classic rock",
                                             "Adult hits",
                                             "Soft rock",
                                             "Top 40",
                                             "Country",
                                             "Oldies",
                                             "Soft",
                                             "Nostalgia",
                                             "Jazz",
                                             "Classical",
                                             "Rhythm and blues",
                                             "Soft rhythm and blues",
                                             "Language",
                                             "Religious music",
                                             "Religious talk",
                                             "Personality",
                                             "Public",
                                             "College",
                                             "Spanish talk",
                                             "Spanish music",
                                             "Hip hop",
                                             "",
                                             "",
                                             "Weather",
                                             "Emergency test",
                                             "Emergency"};

    const char *pty_str = "Unknown";
    if (pty < 32) {
        pty_str = state->is_rbds ? pty_names_rbds[pty] : pty_names_rds[pty];
        if (pty_str[0] == '\0')
            pty_str = "Unknown";
    }
    snprintf(state->program_type, sizeof(state->program_type), "%s", pty_str);

    uint8_t group_type_idx = (group->type << 1) | (group->is_version_b ? 1 : 0);
    uint16_t oda_aid = state->oda_app_for_group[group_type_idx];

    if (oda_aid != 0) {
        if (oda_aid == 0xCD46 || oda_aid == 0xCD47) { // TMC
            decode_8a(state, group);
        } else if (oda_aid == 0x4BD7) { // RT+
            decode_rt_plus(state, group);
        } else if (oda_aid == 0x4BD8) { // eRT / eRT+
            // TODO: eRT (Enhanced RadioText) is an RDS2 feature allowing 128-byte UTF-8 strings.
            // It was intentionally NOT ported from the original C++ redsea implementation 
            // because RDS2 is practically non-existent in the wild. Avoiding it removes
            // massive UTF-8 state-machine tracking and character table fallback overhead.
            decode_rt_plus(state, group); // Fallback to standard RT+ parsing if possible
        }
        // Return here because ODA has overridden the default group behavior
        return;
    }

    switch (group->type) {
    case 0:
        decode_0a(state, group);
        break;
    // Note: Group 1B was originally used for Radio Paging, which has been deprecated globally.
    // Therefore, we intentionally only decode Group 1A here.
    case 1:
        decode_1a(state, group);
        break;
    case 2:
        decode_2a(state, group);
        break;
    case 3:
        if (!group->is_version_b)
            decode_3a(state, group);
        break;
    case 4:
        decode_4a(state, group);
        break;
    case 5:
        decode_5(state, group);
        break;
    case 6:
        decode_6(state, group);
        break;
    case 8:
        decode_8a(state, group);
        break;
    // Note: Group 9A (Emergency Warning System) is rarely used globally and was
    // explicitly left unimplemented (TODO) in the original redsea reference code.
    case 9:
        break;
    case 10:
        decode_10a(state, group);
        break;
    case 14:
        decode_14(state, group);
        break;
    case 15:
        decode_15(state, group);
        break;
        // other groups go here
    }
}
