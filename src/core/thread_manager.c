/**
 * @file thread_manager.c
 * @brief Implements generic thread lifecycle management.
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "thread_manager.h"

static void set_os_thread_name(pthread_t thread, const char *name) {
  if (!name) {
    return;
  }
#if defined(__linux__) || defined(__MINGW32__)
  char buf[16];
  strncpy(buf, name, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  pthread_setname_np(thread, buf);
#endif
}

void thread_manager_init(ThreadManager *manager) {
  if (!manager) {
    return;
  }
  manager->num_threads_started = 0;
}

bool thread_manager_spawn(ThreadManager *manager, const char *name,
                          void *(*func)(void *), void *arg) {
  if (!manager || !func) {
    log_error("thread_manager_spawn called with NULL arguments.");
    return false;
  }

  if (manager->num_threads_started >= MAX_MANAGED_THREADS) {
    log_fatal("Cannot spawn thread '%s': capacity of %d reached.",
              name ? name : "unnamed", MAX_MANAGED_THREADS);
    return false;
  }

  int rc = pthread_create(
      &manager->thread_handles[manager->num_threads_started], NULL, func, arg);
  if (rc != 0) {
    log_fatal("Failed to create '%s' thread: %s", name ? name : "unnamed",
              strerror(rc));
    return false;
  }

  // Set OS kernel thread name for htop, top -H, and gdb
  set_os_thread_name(manager->thread_handles[manager->num_threads_started],
                     name);

  log_debug("Spawned thread '%s'.", name ? name : "unnamed");
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
    int rc = pthread_join(manager->thread_handles[i], NULL);
    if (rc != 0) {
      log_warn("Error joining thread index %d: %s", i, strerror(rc));
    }
  }

  log_debug("All managed threads have joined.");
  manager->num_threads_started = 0;
}
