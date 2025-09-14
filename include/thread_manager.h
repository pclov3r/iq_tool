/**
 * @file thread_manager.h
 * @brief Defines the interface for managing the lifecycle of all processing threads.
 *
 * This module centralizes the logic for creating, starting, and joining the
 * various threads that form the DSP pipeline. It uses the "Dynamic Pipeline"
 * pattern to determine which threads are needed and wires up their communication
 * queues accordingly.
 */

#ifndef THREAD_MANAGER_H_
#define THREAD_MANAGER_H_

#include "app_context.h"
#include <pthread.h>
#include <stdbool.h>

// --- Forward Declarations ---
struct AppConfig;
struct AppResources;
// This is the standard, correct way to forward-declare a struct.
struct PipelineContext;

// --- Constants ---
#define MAX_PIPELINE_THREADS 8

// --- Struct Definition ---

/**
 * @struct ThreadManager
 * @brief Encapsulates all resources and state related to pipeline threads.
 *
 * This struct acts as a central control unit for the application's concurrency.
 * It is initialized based on the final application config and then used to
 * start and stop the entire processing pipeline.
 */
typedef struct {
    // --- Thread Handles & State ---
    pthread_t thread_handles[MAX_PIPELINE_THREADS];
    int       num_threads_started;

    // --- Configuration & Context ---
    struct AppConfig*   config;
    struct AppResources* resources;
    struct PipelineContext* pipeline_context;

    // --- Thread Creation Logic ---
    ThreadFlags threads_to_create;

} ThreadManager;


// --- Public Function Declarations ---

/**
 * @brief Initializes the thread manager and wires up the dynamic pipeline.
 *
 * This function is the core of the dynamic pipeline logic. It inspects the
 * application configuration to determine which processing threads are required.
 * It then creates and connects all the necessary communication queues between
 * those threads. It prepares everything but does NOT start the threads.
 *
 * @param manager A pointer to the ThreadManager struct to initialize.
 * @param config A pointer to the application's configuration.
 * @param resources A pointer to the application's resources.
 * @param pipeline_context A pointer to the context that will be passed to each thread.
 * @return true on success, false on a failure to create queues or other resources.
 */
bool threads_init(ThreadManager* manager, struct AppConfig* config, struct AppResources* resources, struct PipelineContext* pipeline_context);

/**
 * @brief Starts all the pipeline threads that were configured during initialization.
 *
 * @param manager A pointer to the initialized ThreadManager.
 * @return true if all required threads were started successfully, false otherwise.
 */
bool threads_start_all(ThreadManager* manager);

/**
 * @brief Waits for all started pipeline threads to complete.
 *
 * This function calls pthread_join() on every thread that was successfully
 * started, ensuring a clean shutdown.
 *
 * @param manager A pointer to the ThreadManager.
 */
void threads_join_all(ThreadManager* manager);

/**
 * @brief Destroys all queues and synchronization primitives created by the manager.
 *
 * This should be called during the final application cleanup phase.
 *
 * @param resources A pointer to the AppResources struct containing the queues to destroy.
 */
void threads_destroy_queues(struct AppResources* resources);


#endif // THREAD_MANAGER_H_
