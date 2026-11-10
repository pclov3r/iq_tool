/**
 * @file wait_event.c
 * @brief Implements the cross-platform manual-reset event.
 */

#include "wait_event.h"
#include "log.h"
#include "mem_arena.h"
#include "platform.h"
#include <stdatomic.h>
#include <stdlib.h>

#if defined(_WIN32)
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
  WaitEvent *wait_event =
      (WaitEvent *)mem_arena_alloc(arena, sizeof(WaitEvent), true);
  if (!wait_event)
    return NULL;

#if defined(_WIN32)
  // CreateEvent(security, manual_reset, initial_state, name)
  // TRUE for manual_reset means it stays signaled until ResetEvent is called.
  wait_event->handle = CreateEvent(NULL, TRUE, FALSE, NULL);
  if (!wait_event->handle) {
    log_fatal("Failed to create Windows Event object.");
    return NULL;
  }
#elif defined(__APPLE__)
  if (pthread_mutex_init(&wait_event->mutex, NULL) != 0) {
    log_fatal("Failed to init wait_event mutex.");
    return NULL;
  }
  if (pthread_cond_init(&wait_event->cond, NULL) != 0) {
    log_fatal("Failed to init wait_event cond.");
    pthread_mutex_destroy(&wait_event->mutex);
    return NULL;
  }
#else
  if (pthread_mutex_init(&wait_event->mutex, NULL) != 0) {
    log_fatal("Failed to init wait_event mutex.");
    return NULL;
  }
  pthread_condattr_t condition_attribute;
  if (pthread_condattr_init(&condition_attribute) != 0) {
    log_fatal("Failed to init wait_event cond attributes.");
    pthread_mutex_destroy(&wait_event->mutex);
    return NULL;
  }
  pthread_condattr_setclock(&condition_attribute, CLOCK_MONOTONIC);
  if (pthread_cond_init(&wait_event->cond, &condition_attribute) != 0) {
    log_fatal("Failed to init wait_event cond.");
    pthread_condattr_destroy(&condition_attribute);
    pthread_mutex_destroy(&wait_event->mutex);
    return NULL;
  }
  pthread_condattr_destroy(&condition_attribute);
#endif
  atomic_store_explicit(&wait_event->state, WAIT_EVENT_STATE_WAITING,
                        memory_order_release);

  return wait_event;
}

void wait_event_destroy(WaitEvent *wait_event) {
  if (!wait_event)
    return;

#if defined(_WIN32)
  int previous_state = atomic_exchange_explicit(
      &wait_event->state, WAIT_EVENT_STATE_DESTROYED, memory_order_acq_rel);
  if (previous_state != WAIT_EVENT_STATE_WAITING &&
      previous_state != WAIT_EVENT_STATE_SIGNALED)
    return;
  if (wait_event->handle) {
    CloseHandle(wait_event->handle);
    wait_event->handle = NULL;
  }
#else
  pthread_mutex_lock(&wait_event->mutex);
  int current_state =
      atomic_load_explicit(&wait_event->state, memory_order_acquire);
  if (current_state != WAIT_EVENT_STATE_WAITING &&
      current_state != WAIT_EVENT_STATE_SIGNALED) {
    pthread_mutex_unlock(&wait_event->mutex);
    return;
  }
  atomic_store_explicit(&wait_event->state, WAIT_EVENT_STATE_DESTROYED,
                        memory_order_release);
  pthread_cond_broadcast(&wait_event->cond);
  pthread_mutex_unlock(&wait_event->mutex);

  pthread_cond_destroy(&wait_event->cond);
  pthread_mutex_destroy(&wait_event->mutex);
#endif
}

void wait_event_signal(WaitEvent *wait_event) {
  if (!wait_event)
    return;
#if defined(_WIN32)
  int expected_state = WAIT_EVENT_STATE_WAITING;
  if (!atomic_compare_exchange_strong_explicit(
          &wait_event->state, &expected_state, WAIT_EVENT_STATE_SIGNALED,
          memory_order_acq_rel, memory_order_acquire)) {
    if (expected_state != WAIT_EVENT_STATE_SIGNALED)
      return;
  }
  SetEvent(wait_event->handle);
#else
  pthread_mutex_lock(&wait_event->mutex);
  int current_state =
      atomic_load_explicit(&wait_event->state, memory_order_acquire);
  if (current_state != WAIT_EVENT_STATE_WAITING &&
      current_state != WAIT_EVENT_STATE_SIGNALED) {
    pthread_mutex_unlock(&wait_event->mutex);
    return;
  }
  atomic_store_explicit(&wait_event->state, WAIT_EVENT_STATE_SIGNALED,
                        memory_order_release);
  // Broadcast wakes up ALL waiting threads, not just one.
  pthread_cond_broadcast(&wait_event->cond);
  pthread_mutex_unlock(&wait_event->mutex);
#endif
}

void wait_event_wait(WaitEvent *wait_event) {
  if (!wait_event)
    return;
#if defined(_WIN32)
  int current_state =
      atomic_load_explicit(&wait_event->state, memory_order_acquire);
  if (current_state != WAIT_EVENT_STATE_WAITING &&
      current_state != WAIT_EVENT_STATE_SIGNALED)
    return;
  WaitForSingleObject(wait_event->handle, INFINITE);
#else
  pthread_mutex_lock(&wait_event->mutex);
  while (atomic_load_explicit(&wait_event->state, memory_order_acquire) ==
         WAIT_EVENT_STATE_WAITING) {
    pthread_cond_wait(&wait_event->cond, &wait_event->mutex);
  }
  pthread_mutex_unlock(&wait_event->mutex);
#endif
}

WaitEventResult wait_event_wait_timeout(WaitEvent *wait_event,
                                        uint32_t timeout_ms) {
  if (!wait_event)
    return WAIT_EVENT_RESULT_INACTIVE;
#if defined(_WIN32)
  int current_state =
      atomic_load_explicit(&wait_event->state, memory_order_acquire);
  if (current_state == WAIT_EVENT_STATE_UNINITIALIZED ||
      current_state == WAIT_EVENT_STATE_DESTROYED) {
    return WAIT_EVENT_RESULT_INACTIVE;
  }
  DWORD wait_status =
      WaitForSingleObject(wait_event->handle, (DWORD)timeout_ms);
  if (wait_status == WAIT_OBJECT_0) {
    return WAIT_EVENT_RESULT_SIGNALED;
  }
  if (wait_status == WAIT_TIMEOUT) {
    return WAIT_EVENT_RESULT_TIMEOUT;
  }
  return WAIT_EVENT_RESULT_INACTIVE;
#elif defined(__APPLE__)
  struct timespec deadline;
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  platform_timespec_add_ms(&deadline, timeout_ms);

  pthread_mutex_lock(&wait_event->mutex);
  while (atomic_load_explicit(&wait_event->state, memory_order_acquire) ==
         WAIT_EVENT_STATE_WAITING) {
    struct timespec current_time;
    clock_gettime(CLOCK_MONOTONIC, &current_time);

    struct timespec relative_timeout;
    if (!platform_timespec_remaining(&deadline, &current_time,
                                     &relative_timeout)) {
      break;
    }

    int wait_status = pthread_cond_timedwait_relative_np(
        &wait_event->cond, &wait_event->mutex, &relative_timeout);
    if (wait_status == ETIMEDOUT) {
      break;
    }
  }
  int final_state =
      atomic_load_explicit(&wait_event->state, memory_order_acquire);
  pthread_mutex_unlock(&wait_event->mutex);

  if (final_state == WAIT_EVENT_STATE_SIGNALED) {
    return WAIT_EVENT_RESULT_SIGNALED;
  }
  if (final_state == WAIT_EVENT_STATE_WAITING) {
    return WAIT_EVENT_RESULT_TIMEOUT;
  }
  return WAIT_EVENT_RESULT_INACTIVE;
#else
  struct timespec deadline;
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  platform_timespec_add_ms(&deadline, timeout_ms);

  pthread_mutex_lock(&wait_event->mutex);
  while (atomic_load_explicit(&wait_event->state, memory_order_acquire) ==
         WAIT_EVENT_STATE_WAITING) {
    int wait_result =
        pthread_cond_timedwait(&wait_event->cond, &wait_event->mutex, &deadline);
    if (wait_result == ETIMEDOUT) {
      break;
    }
  }
  int final_state =
      atomic_load_explicit(&wait_event->state, memory_order_acquire);
  pthread_mutex_unlock(&wait_event->mutex);

  if (final_state == WAIT_EVENT_STATE_SIGNALED) {
    return WAIT_EVENT_RESULT_SIGNALED;
  }
  if (final_state == WAIT_EVENT_STATE_WAITING) {
    return WAIT_EVENT_RESULT_TIMEOUT;
  }
  return WAIT_EVENT_RESULT_INACTIVE;
#endif
}
