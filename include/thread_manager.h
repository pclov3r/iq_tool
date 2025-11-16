/**
 * @file thread_manager.h
 * @brief Defines a generic, application-agnostic utility for managing thread lifecycle.
 *
 * This module provides a clean interface for creating, starting, and joining a group
 * of threads. It is designed to be completely decoupled from the application's specific
 * logic; it simply executes functions in threads when commanded to do so.
 */

#ifndef THREAD_MANAGER_H_
#define THREAD_MANAGER_H_

#include <pthread.h>
#include <stdbool.h>

// --- Constants ---
#define MAX_MANAGED_THREADS 16

// --- Type Definitions ---

/**
 * @struct ThreadManager
 * @brief The manager struct that holds thread handles and state.
 */
typedef struct {
    pthread_t thread_handles[MAX_MANAGED_THREADS];
    int       num_threads_started;
    void*     thread_context; // A generic context pointer to pass to all threads.
} ThreadManager;


// --- Function Declarations ---

/**
 * @brief Initializes the thread manager.
 * @param manager A pointer to the ThreadManager struct to initialize.
 * @param context A void pointer to a context struct (e.g., PipelineContext) that will be
 *                passed as the sole argument to every thread function that is started.
 */
void thread_manager_init(ThreadManager* manager, void* context);

/**
 * @brief Spawns a single task in a new thread.
 *
 * This function immediately creates a new thread to execute the given function.
 *
 * @param manager A pointer to the initialized ThreadManager.
 * @param name The name of the thread for logging.
 * @param func The function pointer for the thread to execute.
 * @return true if the thread was spawned successfully, false otherwise.
 */
bool thread_manager_spawn_thread(ThreadManager* manager, const char* name, void* (*func)(void*));

/**
 * @brief Waits for all threads spawned by this manager to complete.
 * @param manager A pointer to the ThreadManager.
 */
void thread_manager_join_all(ThreadManager* manager);

#endif // THREAD_MANAGER_H_
