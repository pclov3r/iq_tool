/**
 * @file thread_manager.c
 * @brief Implements a generic utility for managing the lifecycle of application threads.
 */

#include "thread_manager.h"
#include "log.h"
#include <string.h>
#include <errno.h>

/**
 * @brief Initializes the thread manager.
 * @param manager A pointer to the ThreadManager struct to initialize.
 * @param context A void pointer to a context struct (e.g., PipelineContext) that will be
 *                passed as the sole argument to every thread function that is started.
 */
void thread_manager_init(ThreadManager* manager, void* context) {
    if (!manager) {
        return;
    }
    manager->num_threads_started = 0;
    manager->thread_context = context;
}

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
bool thread_manager_spawn_thread(ThreadManager* manager, const char* name, void* (*func)(void*)) {
    if (!manager || !name || !func) {
        log_error("thread_manager_spawn_thread called with NULL arguments.");
        return false;
    }

    if (manager->num_threads_started >= MAX_MANAGED_THREADS) {
        log_fatal("Cannot spawn thread '%s', manager has reached its maximum capacity of %d threads.", name, MAX_MANAGED_THREADS);
        return false;
    }

    int ret = pthread_create(&manager->thread_handles[manager->num_threads_started], NULL, func, manager->thread_context);
    if (ret != 0) {
        log_fatal("Failed to create '%s' thread: %s", name, strerror(ret));
        // Do not increment num_threads_started on failure.
        return false;
    }

    log_debug("Thread '%s' spawned successfully.", name);
    manager->num_threads_started++;
    return true;
}

/**
 * @brief Waits for all threads spawned by this manager to complete.
 * @param manager A pointer to the ThreadManager.
 */
void thread_manager_join_all(ThreadManager* manager) {
    if (!manager || manager->num_threads_started == 0) {
        return;
    }

    log_debug("Waiting for %d thread(s) to complete...", manager->num_threads_started);
    for (int i = 0; i < manager->num_threads_started; i++) {
        if (pthread_join(manager->thread_handles[i], NULL) != 0) {
            // This is not a fatal error, but it's important to log.
            log_warn("Error joining thread '%d'.", i);
        }
    }
    log_debug("All managed threads have joined.");
    // Reset for potential reuse.
    manager->num_threads_started = 0;
}
