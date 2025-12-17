/**
 * @file wait_event.h
 * @brief Defines a cross-platform manual-reset event for thread synchronization.
 */

#ifndef WAIT_EVENT_H_
#define WAIT_EVENT_H_

#include <stdbool.h>

// Forward declaration
struct MemoryArena;

/**
 * @brief Opaque handle to a synchronization event.
 */
typedef struct WaitEvent WaitEvent;

/**
 * @brief Creates a new manual-reset event, initially unsignaled.
 * @param arena The memory arena to allocate from.
 * @return A pointer to the event, or NULL on failure.
 */
WaitEvent* wait_event_create(struct MemoryArena* arena);

/**
 * @brief Destroys the event resources (handles/mutexes).
 * Note: Does not free the struct memory as it is managed by the arena.
 */
void wait_event_destroy(WaitEvent* ev);

/**
 * @brief Signals the event.
 * All threads currently waiting on this event will wake up immediately.
 * The event remains in the signaled state until manually reset.
 */
void wait_event_signal(WaitEvent* ev);

/**
 * @brief Resets the event to the unsignaled state.
 */
void wait_event_reset(WaitEvent* ev);

/**
 * @brief Blocks the calling thread until the event is signaled.
 * If the event is already signaled, this returns immediately.
 */
void wait_event_wait(WaitEvent* ev);

#endif // WAIT_EVENT_H_
