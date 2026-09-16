/**
 * @file thread_manager.h
 * @brief Generic, application-agnostic utility for managing thread lifecycles.
 */

#ifndef THREAD_MANAGER_H_
#define THREAD_MANAGER_H_

#include "config/constants.h"
#include "platform.h"
#include <pthread.h>
#include <stdbool.h>

typedef struct ManagedThread {
  pthread_t handle;
  char name[16];
  ThreadPriority priority;
  void *(*func)(void *);
  void *arg;
} ManagedThread;

typedef struct ThreadManager {
  ManagedThread threads[MAX_MANAGED_THREADS];
  int num_threads_started;
} ThreadManager;

/**
 * @brief Initializes the thread manager.
 */
void thread_manager_init(ThreadManager *manager);

/**
 * @brief Spawns a new thread with specified OS name and scheduling priority.
 * @param manager Pointer to the ThreadManager.
 * @param name Diagnostic name for logging and OS profilers (htop/gdb).
 * @param priority Scheduling priority to apply to the thread.
 * @param func Function to execute in thread.
 * @param arg Pointer passed as sole argument to func.
 * @return true on success, false on failure.
 */
bool thread_manager_spawn(ThreadManager *manager, const char *name,
                          ThreadPriority priority, void *(*func)(void *),
                          void *arg);

/**
 * @brief Waits for all spawned threads to complete and resets the manager.
 */
void thread_manager_join_all(ThreadManager *manager);

#endif // THREAD_MANAGER_H_
