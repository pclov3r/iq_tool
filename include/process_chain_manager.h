/**
 * @file process_chain_manager.h
 * @brief Defines the public interface for running the application's DSP
 * process_chain.
 *
 * This module is the central orchestrator for the application's concurrent
 * processing. It encapsulates the entire lifecycle of creating, running, and
 * destroying the process_chain's internal components and threads.
 */

#ifndef PROCESS_CHAIN_MANAGER_H_
#define PROCESS_CHAIN_MANAGER_H_

#include <stdbool.h>

// --- Forward Declaration ---
// This header is self-contained and only needs to know that this struct exists.
struct ProcessChainContext;

/**
 * @brief Sets up process_chain buffers and calculates resample ratio.
 * Must be called before initializing the output module.
 */
bool process_chain_setup_buffers(struct ProcessChainContext *context);

bool process_chain_init_dsp_modules(struct ProcessChainContext *context);
bool process_chain_execute(struct ProcessChainContext *context);
void process_chain_close_dsp_modules(struct ProcessChainContext *context);
void process_chain_teardown_buffers(struct ProcessChainContext *context);

#include "utilities.h" // For OutputSummaryInfo
void process_chain_get_summary_info(const struct AppContext *app,
                                    OutputSummaryInfo *info);

void *process_chain_get_module_state(const struct AppContext *app,
                                     const char *module_name,
                                     const char *stage_tag);

#endif // PROCESS_CHAIN_MANAGER_H_
