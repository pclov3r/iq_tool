/**
 * @file pipeline.h
 * @brief Defines the public interface for running the application's DSP pipeline.
 *
 * This module is the central orchestrator for the application's concurrent processing.
 * It encapsulates the entire lifecycle of creating, running, and destroying the
 * pipeline's internal components and threads.
 */

#ifndef PIPELINE_H_
#define PIPELINE_H_

#include <stdbool.h>

// --- Forward Declaration ---
// This header is self-contained and only needs to know that this struct exists.
struct PipelineContext;

/**
 * @brief Creates, runs, and waits for the entire processing pipeline to complete.
 *
 * This is the main high-level function that encapsulates the entire pipeline lifecycle.
 * It handles the creation of all DSP objects and queues, spawns all necessary threads
 * using the thread manager, waits for them to finish, and then cleans up all
 * pipeline-specific resources.
 *
 * @param context A pointer to the PipelineContext, containing the application config and resources.
 * @return true if the pipeline ran and shut down cleanly, false if there was a setup or execution error.
 */
bool pipeline_run(struct PipelineContext* context);

#endif // PIPELINE_H_
