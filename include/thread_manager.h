/**
 * @file thread_manager.h
 * @brief Generic, application-agnostic utility for managing thread lifecycles.
 */

#ifndef THREAD_MANAGER_H_
#define THREAD_MANAGER_H_

#include <pthread.h>
#include <stdbool.h>

#define MAX_MANAGED_THREADS 16

typedef struct ThreadManager {
  pthread_t thread_handles[MAX_MANAGED_THREADS];
  int num_threads_started;
} ThreadManager;

/**
 * @brief Initializes the thread manager.
 */
void thread_manager_init(ThreadManager *manager);

/**
 * @brief Spawns a new thread to execute func(arg) and assigns an OS name.
 * @param manager Pointer to the ThreadManager.
 * @param name Diagnostic name for logging and OS profilers (htop/gdb).
 * @param func Function to execute in thread.
 * @param arg Pointer passed as sole argument to func.
 * @return true on success, false on failure.
 */
bool thread_manager_spawn(ThreadManager *manager, const char *name,
                          void *(*func)(void *), void *arg);

/**
 * @brief Waits for all spawned threads to complete and resets the manager.
 */
void thread_manager_join_all(ThreadManager *manager);

#endif // THREAD_MANAGER_H_
