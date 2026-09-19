/**
 * @file driver_state.h
 * @brief Lock-free hardware driver lifecycle state machine.
 */

#ifndef DRIVER_STATE_H_
#define DRIVER_STATE_H_

#include <stdatomic.h>
#include <stdbool.h>

/**
 * @enum DriverState
 * @brief Lifecycle states for hardware capture/streaming drivers.
 */
typedef enum {
  DRIVER_STATE_UNINITIALIZED = 0,
  DRIVER_STATE_OPEN,      ///< Device opened and configured, ready to stream
  DRIVER_STATE_STREAMING, ///< Active hardware streaming in progress
  DRIVER_STATE_STOPPED,   ///< Stream cancelled/stopped
  DRIVER_STATE_CLOSED,    ///< Device handle closed
} DriverState;

/**
 * @brief Initializes the driver state to OPEN.
 */
static inline void driver_state_init(atomic_int *state) {
  if (state) {
    atomic_store_explicit(state, DRIVER_STATE_OPEN, memory_order_release);
  }
}

/**
 * @brief Transitions from OPEN to STREAMING.
 * @return true if successfully transitioned to STREAMING, false otherwise.
 */
static inline bool driver_start_streaming(atomic_int *state) {
  if (!state) {
    return false;
  }
  int expected = DRIVER_STATE_OPEN;
  return atomic_compare_exchange_strong_explicit(
      state, &expected, DRIVER_STATE_STREAMING, memory_order_acq_rel,
      memory_order_acquire);
}

/**
 * @brief Atomically transitions from STREAMING to STOPPED.
 * @return true if this call performed the transition (caller must cancel/stop
 * hardware), false if already stopped or not streaming.
 */
static inline bool driver_stop_streaming(atomic_int *state) {
  if (!state) {
    return false;
  }
  int expected = DRIVER_STATE_STREAMING;
  return atomic_compare_exchange_strong_explicit(
      state, &expected, DRIVER_STATE_STOPPED, memory_order_acq_rel,
      memory_order_acquire);
}

/**
 * @brief Marks streaming complete when the read loop finishes normally.
 */
static inline void driver_finish_streaming(atomic_int *state) {
  if (!state) {
    return;
  }
  int expected = DRIVER_STATE_STREAMING;
  atomic_compare_exchange_strong_explicit(
      state, &expected, DRIVER_STATE_STOPPED, memory_order_acq_rel,
      memory_order_acquire);
}

/**
 * @brief Transitions to CLOSED.
 * @return true if streaming was still active when close was called (caller
 * should stop hardware first), false otherwise.
 */
static inline bool driver_close(atomic_int *state) {
  if (!state) {
    return false;
  }
  int prev = atomic_exchange_explicit(state, DRIVER_STATE_CLOSED,
                                      memory_order_acq_rel);
  return (prev == DRIVER_STATE_STREAMING);
}

#endif // DRIVER_STATE_H_
