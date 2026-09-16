/**
 * @file thread_manager.c
 * @brief Implements generic thread lifecycle management.
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "thread_manager.h"

static void *managed_thread_trampoline(void *arg) {
  ManagedThread *info = (ManagedThread *)arg;

  if (info->name[0] != '\0') {
    platform_set_thread_name(info->name);
  }
  platform_set_thread_priority(info->priority,
                               info->name[0] != '\0' ? info->name : NULL);

  return info->func(info->arg);
}

void thread_manager_init(ThreadManager *manager) {
  if (!manager) {
    return;
  }
  memset(manager, 0, sizeof(ThreadManager));
  manager->num_threads_started = 0;
}

bool thread_manager_spawn(ThreadManager *manager, const char *name,
                          ThreadPriority priority, void *(*func)(void *),
                          void *arg) {
  if (!manager || !func) {
    log_error("thread_manager_spawn called with NULL arguments.");
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
    return false;
  }

  ManagedThread *info = &manager->threads[manager->num_threads_started];
  memset(info, 0, sizeof(*info));
  if (name && name[0] != '\0') {
    strncpy(info->name, name, sizeof(info->name) - 1);
    info->name[sizeof(info->name) - 1] = '\0';
  }
  info->priority = priority;
  info->func = func;
  info->arg = arg;

  int rc = pthread_create(&info->handle, NULL, managed_thread_trampoline, info);
  if (rc != 0) {
    if (info->name[0] != '\0') {
      log_fatal("Failed to create '%s' thread: %s", info->name, strerror(rc));
    } else {
      log_fatal("Failed to create thread: %s", strerror(rc));
    }
    return false;
  }

  if (info->name[0] != '\0') {
    log_debug("Spawned thread '%s' with priority %d.", info->name,
              (int)priority);
  } else {
    log_debug("Spawned thread with priority %d.", (int)priority);
  }
  manager->num_threads_started++;
  return true;
}

void thread_manager_join_all(ThreadManager *manager) {
  if (!manager || manager->num_threads_started == 0) {
    return;
  }

  log_debug("Waiting for %d thread(s) to complete...",
            manager->num_threads_started);

  for (int i = 0; i < manager->num_threads_started; i++) {
    int rc = pthread_join(manager->threads[i].handle, NULL);
    if (rc != 0) {
      log_warn("Error joining thread index %d: %s", i, strerror(rc));
    }
  }

  log_debug("All managed threads have joined.");
  manager->num_threads_started = 0;
}
