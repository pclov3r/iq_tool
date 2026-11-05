/**
 * @file wait_event.c
 * @brief Implements the cross-platform manual-reset event.
 */

#include "wait_event.h"
#include "log.h"
#include "mem_arena.h"
#include <stdatomic.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct WaitEvent {
  HANDLE handle;
  atomic_int state;
};

#else
#include <errno.h>
#include <pthread.h>
#include <time.h>

struct WaitEvent {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  atomic_int state;
};
#endif

WaitEvent *wait_event_create(struct MemoryArena *arena) {
  WaitEvent *ev = (WaitEvent *)mem_arena_alloc(arena, sizeof(WaitEvent), true);
  if (!ev)
    return NULL;

#ifdef _WIN32
  // CreateEvent(security, manual_reset, initial_state, name)
  // TRUE for manual_reset means it stays signaled until ResetEvent is called.
  ev->handle = CreateEvent(NULL, TRUE, FALSE, NULL);
  if (!ev->handle) {
    log_fatal("Failed to create Windows Event object.");
    return NULL;
  }
#else
  if (pthread_mutex_init(&ev->mutex, NULL) != 0) {
    log_fatal("Failed to init wait_event mutex.");
    return NULL;
  }
  pthread_condattr_t cond_attr;
  if (pthread_condattr_init(&cond_attr) != 0) {
    log_fatal("Failed to init wait_event cond attributes.");
    pthread_mutex_destroy(&ev->mutex);
    return NULL;
  }
  pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
  if (pthread_cond_init(&ev->cond, &cond_attr) != 0) {
    log_fatal("Failed to init wait_event cond.");
    pthread_condattr_destroy(&cond_attr);
    pthread_mutex_destroy(&ev->mutex);
    return NULL;
  }
  pthread_condattr_destroy(&cond_attr);
#endif
  atomic_store_explicit(&ev->state, WAIT_EVENT_STATE_WAITING,
                        memory_order_release);

  return ev;
}

void wait_event_destroy(WaitEvent *ev) {
  if (!ev)
    return;

#ifdef _WIN32
  int previous_state = atomic_exchange_explicit(
      &ev->state, WAIT_EVENT_STATE_DESTROYED, memory_order_acq_rel);
  if (previous_state != WAIT_EVENT_STATE_WAITING &&
      previous_state != WAIT_EVENT_STATE_SIGNALED)
    return;
  if (ev->handle) {
    CloseHandle(ev->handle);
    ev->handle = NULL;
  }
#else
  pthread_mutex_lock(&ev->mutex);
  int current_state = atomic_load_explicit(&ev->state, memory_order_acquire);
  if (current_state != WAIT_EVENT_STATE_WAITING &&
      current_state != WAIT_EVENT_STATE_SIGNALED) {
    pthread_mutex_unlock(&ev->mutex);
    return;
  }
  atomic_store_explicit(&ev->state, WAIT_EVENT_STATE_DESTROYED,
                        memory_order_release);
  pthread_cond_broadcast(&ev->cond);
  pthread_mutex_unlock(&ev->mutex);

  pthread_cond_destroy(&ev->cond);
  pthread_mutex_destroy(&ev->mutex);
#endif
}

void wait_event_signal(WaitEvent *ev) {
  if (!ev)
    return;
#ifdef _WIN32
  int expected_state = WAIT_EVENT_STATE_WAITING;
  if (!atomic_compare_exchange_strong_explicit(
          &ev->state, &expected_state, WAIT_EVENT_STATE_SIGNALED,
          memory_order_acq_rel, memory_order_acquire)) {
    if (expected_state != WAIT_EVENT_STATE_SIGNALED)
      return;
  }
  SetEvent(ev->handle);
#else
  pthread_mutex_lock(&ev->mutex);
  int current_state = atomic_load_explicit(&ev->state, memory_order_acquire);
  if (current_state != WAIT_EVENT_STATE_WAITING &&
      current_state != WAIT_EVENT_STATE_SIGNALED) {
    pthread_mutex_unlock(&ev->mutex);
    return;
  }
  atomic_store_explicit(&ev->state, WAIT_EVENT_STATE_SIGNALED,
                        memory_order_release);
  // Broadcast wakes up ALL waiting threads, not just one.
  pthread_cond_broadcast(&ev->cond);
  pthread_mutex_unlock(&ev->mutex);
#endif
}

void wait_event_wait(WaitEvent *ev) {
  if (!ev)
    return;
#ifdef _WIN32
  int current_state = atomic_load_explicit(&ev->state, memory_order_acquire);
  if (current_state != WAIT_EVENT_STATE_WAITING &&
      current_state != WAIT_EVENT_STATE_SIGNALED)
    return;
  WaitForSingleObject(ev->handle, INFINITE);
#else
  pthread_mutex_lock(&ev->mutex);
  while (atomic_load_explicit(&ev->state, memory_order_acquire) ==
         WAIT_EVENT_STATE_WAITING) {
    pthread_cond_wait(&ev->cond, &ev->mutex);
  }
  pthread_mutex_unlock(&ev->mutex);
#endif
}

WaitEventResult wait_event_wait_timeout(WaitEvent *ev, uint32_t timeout_ms) {
  if (!ev)
    return WAIT_EVENT_RESULT_INACTIVE;
#ifdef _WIN32
  int current_state = atomic_load_explicit(&ev->state, memory_order_acquire);
  if (current_state == WAIT_EVENT_STATE_UNINITIALIZED ||
      current_state == WAIT_EVENT_STATE_DESTROYED) {
    return WAIT_EVENT_RESULT_INACTIVE;
  }
  DWORD wait_status = WaitForSingleObject(ev->handle, (DWORD)timeout_ms);
  if (wait_status == WAIT_OBJECT_0) {
    return WAIT_EVENT_RESULT_SIGNALED;
  }
  if (wait_status == WAIT_TIMEOUT) {
    return WAIT_EVENT_RESULT_TIMEOUT;
  }
  return WAIT_EVENT_RESULT_INACTIVE;
#else
  struct timespec deadline;
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  deadline.tv_sec += (time_t)(timeout_ms / 1000U);
  deadline.tv_nsec += (long)((timeout_ms % 1000U) * 1000000L);
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec += 1;
    deadline.tv_nsec -= 1000000000L;
  }

  pthread_mutex_lock(&ev->mutex);
  while (atomic_load_explicit(&ev->state, memory_order_acquire) ==
         WAIT_EVENT_STATE_WAITING) {
    int wait_rc = pthread_cond_timedwait(&ev->cond, &ev->mutex, &deadline);
    if (wait_rc == ETIMEDOUT) {
      break;
    }
  }
  int final_state = atomic_load_explicit(&ev->state, memory_order_acquire);
  pthread_mutex_unlock(&ev->mutex);

  if (final_state == WAIT_EVENT_STATE_SIGNALED) {
    return WAIT_EVENT_RESULT_SIGNALED;
  }
  if (final_state == WAIT_EVENT_STATE_WAITING) {
    return WAIT_EVENT_RESULT_TIMEOUT;
  }
  return WAIT_EVENT_RESULT_INACTIVE;
#endif
}
