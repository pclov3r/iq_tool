#pragma once
#ifndef PROCESS_CHAIN_DEFAULT_H
#define PROCESS_CHAIN_DEFAULT_H

typedef struct {
  const char *module_name;
  const char *stage_tag;
} ProcessNodeDef;

// clang-format off
static const ProcessNodeDef DEFAULT_PROCESS_CHAIN[] = {
    {"dc_block",   NULL},
    {"iq_correct", NULL},
    {"freq_shift", "pre"},
    {"filter",     "pre"},
    {"resampler",  NULL},
    {"freq_shift", "post"},
    {"filter",     "post"},
    {"agc",        NULL}
};
// clang-format on

static const int DEFAULT_PROCESS_CHAIN_LENGTH =
    sizeof(DEFAULT_PROCESS_CHAIN) / sizeof(DEFAULT_PROCESS_CHAIN[0]);

#endif // PROCESS_CHAIN_DEFAULT_H
