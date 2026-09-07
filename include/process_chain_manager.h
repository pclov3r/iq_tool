/**
 * @file process_chain.h
 * @brief Defines the public interface for running the application's DSP
 * process_chain.
 *
 * This module is the central orchestrator for the application's concurrent
 * processing. It encapsulates the entire lifecycle of creating, running, and
 * destroying the process_chain's internal components and threads.
 */

#ifndef PROCESS_CHAIN_H_
#define PROCESS_CHAIN_H_

#include <stdbool.h>

// --- Forward Declaration ---
// This header is self-contained and only needs to know that this struct exists.
struct ProcessChainContext;

/**
 * @brief Creates, runs, and waits for the entire processing process_chain to
 * complete.
 *
 * This is the main high-level function that encapsulates the entire
 * process_chain lifecycle. It handles the creation of all DSP objects and
 * queues, spawns all necessary threads using the thread manager, waits for them
 * to finish, and then cleans up all process_chain-specific app.
 *
 * @param context A pointer to the ProcessChainContext, containing the
 * application config and app.
 * @return true if the process_chain ran and shut down cleanly, false if there
 * was a setup or execution error.
 */
/**
 * @brief Sets up process_chain buffers and calculates resample ratio.
 * Must be called before initializing the output module.
 */
bool process_chain_setup_buffers(struct ProcessChainContext *context);

bool process_chain_run(struct ProcessChainContext *context);
void process_chain_teardown(struct ProcessChainContext *context);

#include "utilities.h" // For OutputSummaryInfo
void process_chain_get_summary_info(const struct AppContext *app,
                                    OutputSummaryInfo *info);

void *process_chain_get_module_state(const struct AppContext *app,
                                     const char *module_name);

#endif // PROCESS_CHAIN_H_
