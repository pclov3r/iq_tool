/**
 * @file process_chain_default.h
 * @brief Defines the default DSP processing chain order and stage nodes.
 */

#ifndef PROCESS_CHAIN_DEFAULT_H_
#define PROCESS_CHAIN_DEFAULT_H_

#include "config/constants.h"
#include <assert.h>
#include <stddef.h>

typedef struct {
  const char *module_name;
  const char *stage_tag;
} ProcessNodeDef;

// clang-format off
static const ProcessNodeDef DEFAULT_PROCESS_CHAIN[] = {
    {"dc_block",   NULL},
    {"iq_correction", NULL},
    {"nco",        "pre"},
    {"filter",     "pre"},
    {"resampler",  NULL},
    {"nco",        "post"},
    {"filter",     "post"},
    {"agc",        NULL}
};
// clang-format on

static const int DEFAULT_PROCESS_CHAIN_LENGTH =
    sizeof(DEFAULT_PROCESS_CHAIN) / sizeof(DEFAULT_PROCESS_CHAIN[0]);

static_assert(
    sizeof(DEFAULT_PROCESS_CHAIN) / sizeof(DEFAULT_PROCESS_CHAIN[0]) <=
        DSP_MAX_MODULES,
    "DEFAULT_PROCESS_CHAIN length exceeds DSP_MAX_MODULES — raise the cap.");

#endif // PROCESS_CHAIN_DEFAULT_H_
