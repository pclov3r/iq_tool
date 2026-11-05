/**
 * @file wait_event.h
 * @brief Defines a cross-platform manual-reset event for thread
 * synchronization.
 */

#ifndef WAIT_EVENT_H_
#define WAIT_EVENT_H_

#include <stdint.h>

// Forward declaration
struct MemoryArena;

/**
 * @brief Opaque handle to a synchronization event.
 */
typedef struct WaitEvent WaitEvent;

/**
 * @brief Lifecycle state of a manual-reset synchronization event.
 */
typedef enum WaitEventState {
  WAIT_EVENT_STATE_UNINITIALIZED = 0,
  WAIT_EVENT_STATE_WAITING,
  WAIT_EVENT_STATE_SIGNALED,
  WAIT_EVENT_STATE_DESTROYED
} WaitEventState;

/**
 * @brief Outcome of waiting on a synchronization event.
 */
typedef enum WaitEventResult {
  WAIT_EVENT_RESULT_SIGNALED = 0,
  WAIT_EVENT_RESULT_TIMEOUT,
  WAIT_EVENT_RESULT_INACTIVE
} WaitEventResult;

/**
 * @brief Creates a new manual-reset event, initially unsignaled.
 * @param arena The memory arena to allocate from.
 * @return A pointer to the event, or NULL on failure.
 */
WaitEvent *wait_event_create(struct MemoryArena *arena);

/**
 * @brief Destroys the event resources (handles/mutexes).
 * Note: Does not free the struct memory as it is managed by the arena.
 */
void wait_event_destroy(WaitEvent *ev);

/**
 * @brief Signals the event.
 * All threads currently waiting on this event will wake up immediately.
 * The event remains in the signaled state until destroyed.
 */
void wait_event_signal(WaitEvent *ev);

/**
 * @brief Blocks the calling thread until the event is signaled.
 * If the event is already signaled, this returns immediately.
 */
void wait_event_wait(WaitEvent *ev);

/**
 * @brief Blocks the calling thread until the event is signaled or the timeout
 * expires.
 * @param ev The synchronization event to wait on.
 * @param timeout_ms Maximum time to wait in milliseconds.
 * @return WAIT_EVENT_RESULT_SIGNALED if signaled, WAIT_EVENT_RESULT_TIMEOUT if
 * the timeout expired, or WAIT_EVENT_RESULT_INACTIVE if the event is invalid or
 * destroyed.
 */
WaitEventResult wait_event_wait_timeout(WaitEvent *ev, uint32_t timeout_ms);

#endif // WAIT_EVENT_H_
