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

struct MemoryArena;

typedef enum ThreadManagerState {
  THREAD_MANAGER_STATE_UNINITIALIZED = 0,
  THREAD_MANAGER_STATE_ACTIVE,
  THREAD_MANAGER_STATE_JOINING,
  THREAD_MANAGER_STATE_DESTROYED
} ThreadManagerState;

typedef struct ManagedThread {
  pthread_t handle;
  char name[16];
  ThreadPriority priority;
  void *(*func)(void *);
  void *arg;
} ManagedThread;

typedef struct TrampolineArg {
  char name[16];
  ThreadPriority priority;
  void *(*func)(void *);
  void *arg;
} TrampolineArg;

typedef struct ThreadManager {
  ManagedThread threads[MAX_MANAGED_THREADS];
  TrampolineArg trampoline_args[MAX_MANAGED_THREADS];
  int num_threads_started;
  ThreadManagerState state;
  pthread_mutex_t lock;
  struct MemoryArena *arena;
} ThreadManager;

/**
 * @brief Initializes the thread manager.
 * @param manager Pointer to the ThreadManager.
 * @param arena Optional MemoryArena pointer for lifecycle allocations (can be
 * NULL).
 */
void thread_manager_init(ThreadManager *manager, struct MemoryArena *arena);

/**
 * @brief Sets or updates the memory arena for the thread manager.
 * @param manager Pointer to the ThreadManager.
 * @param arena Pointer to the MemoryArena to use.
 */
void thread_manager_set_arena(ThreadManager *manager,
                              struct MemoryArena *arena);

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

/**
 * @brief Waits for all spawned threads to complete and destroys manager
 * resources.
 */
void thread_manager_destroy(ThreadManager *manager);

#endif // THREAD_MANAGER_H_
