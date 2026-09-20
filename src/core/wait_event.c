/**
 * @file wait_event.c
 * @brief Implements the cross-platform manual-reset event.
 */

#include "wait_event.h"
#include "log.h"
#include "mem_arena.h"
#include <stdatomic.h>
#include <stdlib.h>

typedef enum WaitEventState {
  WAIT_EVENT_STATE_UNINITIALIZED = 0,
  WAIT_EVENT_STATE_ACTIVE,
  WAIT_EVENT_STATE_DESTROYED
} WaitEventState;

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct WaitEvent {
  HANDLE handle;
  atomic_int state;
};

#else
#include <pthread.h>

struct WaitEvent {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  bool signaled;
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
  if (pthread_cond_init(&ev->cond, NULL) != 0) {
    log_fatal("Failed to init wait_event cond.");
    pthread_mutex_destroy(&ev->mutex);
    return NULL;
  }
  ev->signaled = false;
#endif
  atomic_store_explicit(&ev->state, WAIT_EVENT_STATE_ACTIVE,
                        memory_order_release);

  return ev;
}

void wait_event_destroy(WaitEvent *ev) {
  if (!ev)
    return;

#ifdef _WIN32
  if (atomic_exchange_explicit(&ev->state, WAIT_EVENT_STATE_DESTROYED,
                               memory_order_acq_rel) != WAIT_EVENT_STATE_ACTIVE)
    return;
  if (ev->handle) {
    CloseHandle(ev->handle);
    ev->handle = NULL;
  }
#else
  pthread_mutex_lock(&ev->mutex);
  if (atomic_load_explicit(&ev->state, memory_order_acquire) !=
      WAIT_EVENT_STATE_ACTIVE) {
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
  if (atomic_load_explicit(&ev->state, memory_order_acquire) !=
      WAIT_EVENT_STATE_ACTIVE)
    return;
  SetEvent(ev->handle);
#else
  pthread_mutex_lock(&ev->mutex);
  if (atomic_load_explicit(&ev->state, memory_order_acquire) !=
      WAIT_EVENT_STATE_ACTIVE) {
    pthread_mutex_unlock(&ev->mutex);
    return;
  }
  ev->signaled = true;
  // Broadcast wakes up ALL waiting threads, not just one.
  pthread_cond_broadcast(&ev->cond);
  pthread_mutex_unlock(&ev->mutex);
#endif
}

void wait_event_wait(WaitEvent *ev) {
  if (!ev)
    return;
#ifdef _WIN32
  if (atomic_load_explicit(&ev->state, memory_order_acquire) !=
      WAIT_EVENT_STATE_ACTIVE)
    return;
  WaitForSingleObject(ev->handle, INFINITE);
#else
  pthread_mutex_lock(&ev->mutex);
  while (!ev->signaled &&
         atomic_load_explicit(&ev->state, memory_order_relaxed) ==
             WAIT_EVENT_STATE_ACTIVE) {
    pthread_cond_wait(&ev->cond, &ev->mutex);
  }
  pthread_mutex_unlock(&ev->mutex);
#endif
}
