/**
 * @file thread_manager.c
 * @brief Implements generic thread lifecycle management.
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "mem_arena.h"
#include "thread_manager.h"

static void *managed_thread_trampoline(void *arg) {
  TrampolineArg *targ = (TrampolineArg *)arg;
  TrampolineArg thread_info = *targ;

  if (thread_info.name[0] != '\0') {
    platform_set_thread_name(thread_info.name);
  }
  platform_set_thread_priority(thread_info.priority, thread_info.name[0] != '\0'
                                                         ? thread_info.name
                                                         : NULL);

  return thread_info.func(thread_info.arg);
}

void thread_manager_init(ThreadManager *manager, struct MemoryArena *arena) {
  if (!manager) {
    return;
  }
  memset(manager, 0, sizeof(ThreadManager));
  pthread_mutex_init(&manager->lock, NULL);
  manager->state = THREAD_MANAGER_STATE_ACTIVE;
  manager->num_threads_started = 0;
  manager->arena = arena;
}

void thread_manager_set_arena(ThreadManager *manager,
                              struct MemoryArena *arena) {
  if (!manager) {
    return;
  }
  pthread_mutex_lock(&manager->lock);
  manager->arena = arena;
  pthread_mutex_unlock(&manager->lock);
}

bool thread_manager_spawn(ThreadManager *manager, const char *name,
                          ThreadPriority priority, void *(*func)(void *),
                          void *arg) {
  if (!manager || !func) {
    log_error("thread_manager_spawn called with NULL arguments.");
    return false;
  }

  pthread_mutex_lock(&manager->lock);

  if (manager->state != THREAD_MANAGER_STATE_ACTIVE) {
    log_error("Cannot spawn thread: ThreadManager is not active.");
    pthread_mutex_unlock(&manager->lock);
    return false;
  }

  if (manager->num_threads_started >= MAX_MANAGED_THREADS) {
    if (name && name[0] != '\0') {
      log_fatal("Cannot spawn thread '%s': capacity of %d reached.", name,
                MAX_MANAGED_THREADS);
    } else {
      log_fatal("Cannot spawn thread: capacity of %d reached.",
                MAX_MANAGED_THREADS);
    }
    pthread_mutex_unlock(&manager->lock);
    return false;
  }

  TrampolineArg *targ = NULL;
  if (manager->arena) {
    targ = (TrampolineArg *)mem_arena_alloc(manager->arena,
                                            sizeof(TrampolineArg), true);
  }
  if (!targ) {
    targ = &manager->trampoline_args[manager->num_threads_started];
  }
  memset(targ, 0, sizeof(*targ));
  if (name && name[0] != '\0') {
    strncpy(targ->name, name, sizeof(targ->name) - 1);
    targ->name[sizeof(targ->name) - 1] = '\0';
  }
  targ->priority = priority;
  targ->func = func;
  targ->arg = arg;

  ManagedThread *info = &manager->threads[manager->num_threads_started];
  memset(info, 0, sizeof(*info));
  if (name && name[0] != '\0') {
    strncpy(info->name, name, sizeof(info->name) - 1);
    info->name[sizeof(info->name) - 1] = '\0';
  }
  info->priority = priority;
  info->func = func;
  info->arg = arg;

  int rc = pthread_create(&info->handle, NULL, managed_thread_trampoline, targ);
  if (rc != 0) {
    if (info->name[0] != '\0') {
      log_fatal("Failed to create '%s' thread: %s", info->name, strerror(rc));
    } else {
      log_fatal("Failed to create thread: %s", strerror(rc));
    }
    pthread_mutex_unlock(&manager->lock);
    return false;
  }

  if (info->name[0] != '\0') {
    log_debug("Spawned thread '%s' with priority %d.", info->name,
              (int)priority);
  } else {
    log_debug("Spawned thread with priority %d.", (int)priority);
  }
  manager->num_threads_started++;
  pthread_mutex_unlock(&manager->lock);
  return true;
}

void thread_manager_join_all(ThreadManager *manager) {
  if (!manager) {
    return;
  }

  pthread_mutex_lock(&manager->lock);
  if (manager->state != THREAD_MANAGER_STATE_ACTIVE &&
      manager->state != THREAD_MANAGER_STATE_JOINING) {
    pthread_mutex_unlock(&manager->lock);
    return;
  }

  int count = manager->num_threads_started;
  if (count == 0) {
    pthread_mutex_unlock(&manager->lock);
    return;
  }

  manager->state = THREAD_MANAGER_STATE_JOINING;
  log_debug("Waiting for %d thread(s) to complete...", count);

  pthread_t handles[MAX_MANAGED_THREADS];
  for (int i = 0; i < count; i++) {
    handles[i] = manager->threads[i].handle;
  }
  manager->num_threads_started = 0;
  pthread_mutex_unlock(&manager->lock);

  for (int i = 0; i < count; i++) {
    int rc = pthread_join(handles[i], NULL);
    if (rc != 0) {
      log_warn("Error joining thread index %d: %s", i, strerror(rc));
    }
  }

  pthread_mutex_lock(&manager->lock);
  if (manager->state == THREAD_MANAGER_STATE_JOINING) {
    manager->state = THREAD_MANAGER_STATE_ACTIVE;
  }
  pthread_mutex_unlock(&manager->lock);

  log_debug("All managed threads have joined.");
}

void thread_manager_destroy(ThreadManager *manager) {
  if (!manager) {
    return;
  }

  pthread_mutex_lock(&manager->lock);
  if (manager->state == THREAD_MANAGER_STATE_UNINITIALIZED ||
      manager->state == THREAD_MANAGER_STATE_DESTROYED) {
    pthread_mutex_unlock(&manager->lock);
    return;
  }

  manager->state = THREAD_MANAGER_STATE_DESTROYED;
  int count = manager->num_threads_started;
  pthread_t handles[MAX_MANAGED_THREADS];
  for (int i = 0; i < count; i++) {
    handles[i] = manager->threads[i].handle;
  }
  manager->num_threads_started = 0;
  pthread_mutex_unlock(&manager->lock);

  for (int i = 0; i < count; i++) {
    int rc = pthread_join(handles[i], NULL);
    if (rc != 0) {
      log_warn("Error joining thread index %d: %s", i, strerror(rc));
    }
  }

  pthread_mutex_destroy(&manager->lock);
}
