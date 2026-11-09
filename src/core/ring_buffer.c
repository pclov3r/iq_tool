/**
 * @file ring_buffer.c
 * @brief Lock-free single-producer single-consumer ring buffer implementation.
 */

#include "ring_buffer.h"
#include "config/constants.h"
#include "log.h"
#include "mem_arena.h"
#include "platform.h"
#include <stdalign.h>  // C11 Alignment
#include <stdatomic.h> // C11 Atomics
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <malloc.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <dispatch/dispatch.h>
#else
#include <errno.h>
#include <semaphore.h>
#include <time.h>
#endif

// Internal sentinel stored in clear_target when no clear request is pending.
// Not a tunable: any value >= capacity works, SIZE_MAX can never be a valid
// buffer index.
#define RING_BUFFER_NO_CLEAR_PENDING SIZE_MAX

/*
 * WakeSignal: a binary, auto-reset wakeup with no user-space mutex.
 * Posting never blocks, so it is safe to call from SDR and audio callbacks.
 *   Windows: auto-reset Event object
 *   macOS:   dispatch_semaphore (unnamed POSIX semaphores are unsupported)
 *   Linux:   POSIX sem_t (futex-backed), monotonic timeout via sem_clockwait
 */
typedef struct {
#if defined(_WIN32)
  HANDLE handle;
#elif defined(__APPLE__)
  dispatch_semaphore_t handle;
#else
  sem_t handle;
#endif
  bool initialized;
} WakeSignal;

struct RingBuffer {
  unsigned char *buffer;
  size_t capacity;

  // C11 Lock-Free Implementation:
  // alignas(CACHE_LINE_PADDING) pushes 'write_pos' and 'read_pos' into
  // completely different cache lines (and prevents spatial prefetcher
  // collisions). This permanently eliminates False Sharing between Producer and
  // Consumer cores.
  alignas(CACHE_LINE_PADDING) atomic_size_t write_pos;
  alignas(CACHE_LINE_PADDING) atomic_size_t read_pos;

  // Isolates the reader index from the state and wake signals below.
  alignas(CACHE_LINE_PADDING) atomic_int state;

  // Pending clear request: the write position to discard up to, or
  // RING_BUFFER_NO_CLEAR_PENDING. Only the consumer applies it, so read_pos
  // always has exactly one writer.
  atomic_size_t clear_target;

  // Latency: the consumer parks here when empty; the producer posts after
  // publishing new data.
  WakeSignal data_ready;

  // Backpressure: the producer parks here when full; the consumer posts after
  // freeing space. Not used by the lock-free writer (SDR hardware thread).
  WakeSignal space_free;
};

// --- Wake Signal ---

static bool wake_signal_init(WakeSignal *signal) {
#if defined(_WIN32)
  signal->handle = CreateEvent(NULL, FALSE, FALSE, NULL);
  signal->initialized = (signal->handle != NULL);
#elif defined(__APPLE__)
  signal->handle = dispatch_semaphore_create(0);
  signal->initialized = (signal->handle != NULL);
#else
  signal->initialized = (sem_init(&signal->handle, 0, 0) == 0);
#endif
  return signal->initialized;
}

static void wake_signal_destroy(WakeSignal *signal) {
  if (!signal->initialized)
    return;
  signal->initialized = false;
#if defined(_WIN32)
  CloseHandle(signal->handle);
  signal->handle = NULL;
#elif defined(__APPLE__)
  dispatch_release(signal->handle);
  signal->handle = NULL;
#else
  sem_destroy(&signal->handle);
#endif
}

static void wake_signal_post(WakeSignal *signal) {
#if defined(_WIN32)
  SetEvent(signal->handle);
#elif defined(__APPLE__)
  dispatch_semaphore_signal(signal->handle);
#else
  sem_post(&signal->handle);
#endif
}

// Discards stale posts left over from an earlier park. Must be called BEFORE
// registering as a waiter, so a post meant for the upcoming park is never lost.
static void wake_signal_discard_pending(WakeSignal *signal) {
#if defined(_WIN32)
  ResetEvent(signal->handle);
#elif defined(__APPLE__)
  while (dispatch_semaphore_wait(signal->handle, DISPATCH_TIME_NOW) == 0) {
  }
#else
  while (sem_trywait(&signal->handle) == 0) {
  }
#endif
}

static void wake_signal_wait(WakeSignal *signal, uint32_t timeout_ms) {
#if defined(_WIN32)
  WaitForSingleObject(signal->handle, timeout_ms);
#elif defined(__APPLE__)
  dispatch_semaphore_wait(
      signal->handle,
      dispatch_time(DISPATCH_TIME_NOW,
                    (int64_t)timeout_ms * (int64_t)NSEC_PER_MSEC));
#else
#ifdef HAVE_SEM_CLOCKWAIT
  const clockid_t wait_clock = CLOCK_MONOTONIC; // Immune to wall-clock jumps
#else
  const clockid_t wait_clock = CLOCK_REALTIME; // sem_timedwait requires it
#endif
  struct timespec deadline;
  clock_gettime(wait_clock, &deadline);
  deadline.tv_sec += (time_t)(timeout_ms / 1000U);
  deadline.tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec += 1;
    deadline.tv_nsec -= 1000000000L;
  }

  int wait_result;
  do {
#ifdef HAVE_SEM_CLOCKWAIT
    wait_result = sem_clockwait(&signal->handle, wait_clock, &deadline);
#else
    wait_result = sem_timedwait(&signal->handle, &deadline);
#endif
  } while (wait_result != 0 && errno == EINTR);
#endif
}

// --- Internal Helpers ---

static inline bool is_writable_state(int state) {
  return state == RING_BUFFER_STATE_ACTIVE ||
         state == RING_BUFFER_STATE_CONSUMER_WAITING ||
         state == RING_BUFFER_STATE_PRODUCER_WAITING;
}

static inline bool is_terminal_state(int state) {
  return state == RING_BUFFER_STATE_SHUTDOWN ||
         state == RING_BUFFER_STATE_DESTROYED ||
         state == RING_BUFFER_STATE_UNINITIALIZED;
}

static inline size_t used_bytes_between(size_t read_index, size_t write_index,
                                        size_t capacity) {
  return (write_index >= read_index) ? (write_index - read_index)
                                     : (capacity - (read_index - write_index));
}

// Returns a parked thread's state to ACTIVE. Fails harmlessly if the other
// side already woke us, or if the buffer moved to DRAINING/SHUTDOWN.
static inline void restore_active_state(RingBuffer *ring_buffer,
                                        int waiting_state) {
  int expected_state = waiting_state;
  atomic_compare_exchange_strong_explicit(
      &ring_buffer->state, &expected_state, RING_BUFFER_STATE_ACTIVE,
      memory_order_acq_rel, memory_order_relaxed);
}

// Producer side, called after publishing write_pos. The seq_cst fence pairs
// with the fence in the consumer's park path: either this thread observes
// CONSUMER_WAITING, or the consumer observes the new write_pos before parking.
static inline void wake_waiting_consumer(RingBuffer *ring_buffer) {
  atomic_thread_fence(memory_order_seq_cst);
  if (atomic_load_explicit(&ring_buffer->state, memory_order_relaxed) !=
      RING_BUFFER_STATE_CONSUMER_WAITING)
    return;
  int expected_state = RING_BUFFER_STATE_CONSUMER_WAITING;
  if (atomic_compare_exchange_strong_explicit(
          &ring_buffer->state, &expected_state, RING_BUFFER_STATE_ACTIVE,
          memory_order_acq_rel, memory_order_relaxed)) {
    wake_signal_post(&ring_buffer->data_ready);
  }
}

// Consumer side, called after publishing read_pos. Mirror of
// wake_waiting_consumer() for the backpressure path.
static inline void wake_waiting_producer(RingBuffer *ring_buffer) {
  atomic_thread_fence(memory_order_seq_cst);
  if (atomic_load_explicit(&ring_buffer->state, memory_order_relaxed) !=
      RING_BUFFER_STATE_PRODUCER_WAITING)
    return;
  int expected_state = RING_BUFFER_STATE_PRODUCER_WAITING;
  if (atomic_compare_exchange_strong_explicit(
          &ring_buffer->state, &expected_state, RING_BUFFER_STATE_ACTIVE,
          memory_order_acq_rel, memory_order_relaxed)) {
    wake_signal_post(&ring_buffer->space_free);
  }
}

// CONSUMER ONLY: applies a pending ring_buffer_clear() request and returns the
// (possibly advanced) read index.
static size_t apply_pending_clear(RingBuffer *ring_buffer) {
  size_t read_index =
      atomic_load_explicit(&ring_buffer->read_pos, memory_order_relaxed);

  // Fast path: plain load, no read-modify-write when nothing is pending.
  if (atomic_load_explicit(&ring_buffer->clear_target, memory_order_relaxed) ==
      RING_BUFFER_NO_CLEAR_PENDING)
    return read_index;

  size_t clear_target = atomic_exchange_explicit(
      &ring_buffer->clear_target, RING_BUFFER_NO_CLEAR_PENDING,
      memory_order_acquire);
  if (clear_target == RING_BUFFER_NO_CLEAR_PENDING)
    return read_index;

  size_t write_index =
      atomic_load_explicit(&ring_buffer->write_pos, memory_order_acquire);
  size_t capacity = ring_buffer->capacity;
  size_t discard_bytes = used_bytes_between(read_index, clear_target, capacity);
  size_t used_bytes = used_bytes_between(read_index, write_index, capacity);

  // Skip if the consumer has already read past the requested position.
  if (discard_bytes == 0 || discard_bytes > used_bytes)
    return read_index;

  atomic_store_explicit(&ring_buffer->read_pos, clear_target,
                        memory_order_release);
  wake_waiting_producer(ring_buffer);
  return clear_target;
}

// CONSUMER ONLY: copies out bytes starting at read_index and publishes the new
// read position.
static size_t consume_bytes(RingBuffer *ring_buffer, void *buffer,
                            size_t read_index, size_t bytes_to_read) {
  size_t capacity = ring_buffer->capacity;
  size_t first_chunk_size = (read_index + bytes_to_read > capacity)
                                ? (capacity - read_index)
                                : bytes_to_read;
  memcpy(buffer, ring_buffer->buffer + read_index, first_chunk_size);

  size_t second_chunk_size = bytes_to_read - first_chunk_size;
  if (second_chunk_size > 0) {
    memcpy((unsigned char *)buffer + first_chunk_size, ring_buffer->buffer,
           second_chunk_size);
  }

  atomic_store_explicit(&ring_buffer->read_pos,
                        (read_index + bytes_to_read) % capacity,
                        memory_order_release);

  // Signal Backpressure: wake the producer ONLY if parked in PRODUCER_WAITING
  wake_waiting_producer(ring_buffer);

  return bytes_to_read;
}

// --- Public API ---

RingBuffer *ring_buffer_create(size_t capacity, MemoryArena *arena) {
  RingBuffer *ring_buffer =
      (RingBuffer *)mem_arena_alloc(arena, sizeof(RingBuffer), true);
  if (!ring_buffer) {
    log_fatal("Failed to allocate memory for RingBuffer struct from arena.");
    return NULL;
  }

  size_t aligned_capacity =
      (capacity + MEM_ARENA_ALIGNMENT - 1) & ~(MEM_ARENA_ALIGNMENT - 1);

  ring_buffer->buffer =
      (unsigned char *)mem_arena_alloc(arena, aligned_capacity, false);
  if (!ring_buffer->buffer) {
    log_fatal(
        "Failed to allocate memory for RingBuffer data buffer from arena.");
    return NULL;
  }

  ring_buffer->capacity = aligned_capacity;
  atomic_init(&ring_buffer->write_pos, 0);
  atomic_init(&ring_buffer->read_pos, 0);
  atomic_init(&ring_buffer->clear_target, RING_BUFFER_NO_CLEAR_PENDING);

  if (!wake_signal_init(&ring_buffer->data_ready)) {
    log_fatal("Failed to create RingBuffer data-ready signal.");
    return NULL;
  }
  if (!wake_signal_init(&ring_buffer->space_free)) {
    log_fatal("Failed to create RingBuffer space-free signal.");
    wake_signal_destroy(&ring_buffer->data_ready);
    return NULL;
  }

  atomic_init(&ring_buffer->state, RING_BUFFER_STATE_ACTIVE);
  return ring_buffer;
}

void ring_buffer_destroy(RingBuffer *ring_buffer) {
  if (!ring_buffer)
    return;

  int expected_state = atomic_load(&ring_buffer->state);
  if (expected_state == RING_BUFFER_STATE_UNINITIALIZED ||
      expected_state == RING_BUFFER_STATE_DESTROYED) {
    return;
  }
  atomic_store(&ring_buffer->state, RING_BUFFER_STATE_DESTROYED);

  wake_signal_destroy(&ring_buffer->space_free);
  wake_signal_destroy(&ring_buffer->data_ready);
}

// PRODUCER: High Priority, Lock-Free, Never Sleeps
size_t ring_buffer_write(RingBuffer *ring_buffer, const void *data,
                         size_t bytes) {
  if (!ring_buffer || !data || bytes == 0 ||
      !is_writable_state(
          atomic_load_explicit(&ring_buffer->state, memory_order_acquire)))
    return 0;

  size_t write_index =
      atomic_load_explicit(&ring_buffer->write_pos, memory_order_relaxed);
  size_t read_index =
      atomic_load_explicit(&ring_buffer->read_pos, memory_order_acquire);
  size_t capacity = ring_buffer->capacity;

  size_t used_bytes = used_bytes_between(read_index, write_index, capacity);
  size_t available_space = (capacity - 1) - used_bytes;

  if (bytes > available_space) {
    return 0;
  }

  size_t first_chunk_size =
      (write_index + bytes > capacity) ? (capacity - write_index) : bytes;
  memcpy(ring_buffer->buffer + write_index, data, first_chunk_size);

  size_t second_chunk_size = bytes - first_chunk_size;
  if (second_chunk_size > 0) {
    memcpy(ring_buffer->buffer, (const unsigned char *)data + first_chunk_size,
           second_chunk_size);
  }

  atomic_store_explicit(&ring_buffer->write_pos,
                        (write_index + bytes) % capacity,
                        memory_order_release);

  wake_waiting_consumer(ring_buffer);

  return bytes;
}

// PRODUCER: High Priority, Lock-Free, Scatter-Gather
size_t ring_buffer_write_packet(RingBuffer *ring_buffer, const void *header,
                                size_t header_length, const void *payload,
                                size_t payload_length) {
  if (!ring_buffer || !header ||
      !is_writable_state(
          atomic_load_explicit(&ring_buffer->state, memory_order_acquire)))
    return 0;

  size_t total_bytes = header_length + payload_length;
  size_t write_index =
      atomic_load_explicit(&ring_buffer->write_pos, memory_order_relaxed);
  size_t read_index =
      atomic_load_explicit(&ring_buffer->read_pos, memory_order_acquire);
  size_t capacity = ring_buffer->capacity;

  size_t used_bytes = used_bytes_between(read_index, write_index, capacity);
  size_t available_space = (capacity - 1) - used_bytes;

  if (total_bytes > available_space) {
    return 0; // Drop packet
  }

  // 1. Write Header
  size_t header_chunk1 = (write_index + header_length > capacity)
                             ? (capacity - write_index)
                             : header_length;
  memcpy(ring_buffer->buffer + write_index, header, header_chunk1);

  size_t header_chunk2 = header_length - header_chunk1;
  if (header_chunk2 > 0) {
    memcpy(ring_buffer->buffer, (const unsigned char *)header + header_chunk1,
           header_chunk2);
  }
  write_index = (write_index + header_length) % capacity;

  // 2. Write Payload (if exists)
  if (payload && payload_length > 0) {
    size_t payload_chunk1 = (write_index + payload_length > capacity)
                                ? (capacity - write_index)
                                : payload_length;
    memcpy(ring_buffer->buffer + write_index, payload, payload_chunk1);

    size_t payload_chunk2 = payload_length - payload_chunk1;
    if (payload_chunk2 > 0) {
      memcpy(ring_buffer->buffer,
             (const unsigned char *)payload + payload_chunk1, payload_chunk2);
    }
    write_index = (write_index + payload_length) % capacity;
  }

  // 3. Update write_pos and signal ONCE
  atomic_store_explicit(&ring_buffer->write_pos, write_index,
                        memory_order_release);

  wake_waiting_consumer(ring_buffer);

  return total_bytes;
}

// CONSUMER: Low Priority, Blocking (Sleeps if empty)
size_t ring_buffer_read(RingBuffer *ring_buffer, void *buffer,
                        size_t max_bytes) {
  if (!ring_buffer || !buffer || max_bytes == 0)
    return 0;

  size_t capacity = ring_buffer->capacity;
  size_t read_index;
  size_t write_index;
  size_t available_bytes;

  while (true) {
    int current_state =
        atomic_load_explicit(&ring_buffer->state, memory_order_acquire);
    if (is_terminal_state(current_state))
      return 0;

    read_index = apply_pending_clear(ring_buffer);
    write_index =
        atomic_load_explicit(&ring_buffer->write_pos, memory_order_acquire);
    available_bytes = used_bytes_between(read_index, write_index, capacity);

    if (available_bytes > 0) {
      break;
    }

    if (current_state == RING_BUFFER_STATE_DRAINING) {
      return 0;
    }

    // 1. Drop stale posts, then register: ACTIVE -> CONSUMER_WAITING
    wake_signal_discard_pending(&ring_buffer->data_ready);
    int expected_state = RING_BUFFER_STATE_ACTIVE;
    if (!atomic_compare_exchange_strong_explicit(
            &ring_buffer->state, &expected_state,
            RING_BUFFER_STATE_CONSUMER_WAITING, memory_order_seq_cst,
            memory_order_acquire)) {
      if (expected_state == RING_BUFFER_STATE_DRAINING) {
        continue;
      }
      if (!is_writable_state(expected_state)) {
        return 0;
      }
      // Could not register (producer is parked): fall through to a bounded
      // timed wait instead of spinning.
    }

    // 2. Pairs with the fence in wake_waiting_consumer(): re-check for data
    // published while we were registering.
    atomic_thread_fence(memory_order_seq_cst);
    write_index =
        atomic_load_explicit(&ring_buffer->write_pos, memory_order_acquire);
    available_bytes = used_bytes_between(read_index, write_index, capacity);
    if (available_bytes > 0) {
      restore_active_state(ring_buffer, RING_BUFFER_STATE_CONSUMER_WAITING);
      break;
    }

    // 3. Buffer still empty: park until the producer posts or timeout.
    wake_signal_wait(&ring_buffer->data_ready, RING_BUFFER_WAIT_TIMEOUT_MS);
    restore_active_state(ring_buffer, RING_BUFFER_STATE_CONSUMER_WAITING);
  }

  size_t bytes_to_read =
      (max_bytes > available_bytes) ? available_bytes : max_bytes;
  return consume_bytes(ring_buffer, buffer, read_index, bytes_to_read);
}

// CONSUMER: Real-Time Safe, Lock-Free, Never Sleeps
size_t ring_buffer_try_read(RingBuffer *ring_buffer, void *buffer,
                            size_t max_bytes) {
  if (!ring_buffer || !buffer || max_bytes == 0)
    return 0;

  if (is_terminal_state(
          atomic_load_explicit(&ring_buffer->state, memory_order_acquire)))
    return 0;

  size_t read_index = apply_pending_clear(ring_buffer);
  size_t write_index =
      atomic_load_explicit(&ring_buffer->write_pos, memory_order_acquire);
  size_t available_bytes =
      used_bytes_between(read_index, write_index, ring_buffer->capacity);
  if (available_bytes == 0)
    return 0;

  size_t bytes_to_read =
      (max_bytes > available_bytes) ? available_bytes : max_bytes;
  return consume_bytes(ring_buffer, buffer, read_index, bytes_to_read);
}

// BACKPRESSURE WAIT: Used by File Readers / Audio Demodulators (Producers)
void ring_buffer_wait_for_threshold(RingBuffer *ring_buffer,
                                    size_t target_size) {
  if (!ring_buffer)
    return;

  while (true) {
    if (!is_writable_state(
            atomic_load_explicit(&ring_buffer->state, memory_order_acquire)))
      return;

    if (ring_buffer_get_size(ring_buffer) <= target_size)
      return;

    // 1. Drop stale posts, then register: ACTIVE -> PRODUCER_WAITING
    wake_signal_discard_pending(&ring_buffer->space_free);
    int expected_state = RING_BUFFER_STATE_ACTIVE;
    if (!atomic_compare_exchange_strong_explicit(
            &ring_buffer->state, &expected_state,
            RING_BUFFER_STATE_PRODUCER_WAITING, memory_order_seq_cst,
            memory_order_acquire)) {
      if (!is_writable_state(expected_state)) {
        return;
      }
      // Could not register (consumer is parked): fall through to a bounded
      // timed wait instead of spinning.
    }

    // 2. Pairs with the fence in wake_waiting_producer(): re-check for space
    // freed while we were registering.
    atomic_thread_fence(memory_order_seq_cst);
    if (ring_buffer_get_size(ring_buffer) <= target_size) {
      restore_active_state(ring_buffer, RING_BUFFER_STATE_PRODUCER_WAITING);
      return;
    }

    // 3. Still above threshold: park until the consumer posts or timeout.
    wake_signal_wait(&ring_buffer->space_free, RING_BUFFER_WAIT_TIMEOUT_MS);
    restore_active_state(ring_buffer, RING_BUFFER_STATE_PRODUCER_WAITING);
  }
}

void ring_buffer_drain(RingBuffer *ring_buffer) {
  if (!ring_buffer)
    return;
  int expected_state =
      atomic_load_explicit(&ring_buffer->state, memory_order_acquire);
  while (is_writable_state(expected_state)) {
    if (atomic_compare_exchange_weak_explicit(
            &ring_buffer->state, &expected_state, RING_BUFFER_STATE_DRAINING,
            memory_order_acq_rel, memory_order_acquire)) {
      // Wake any parked thread so it observes DRAINING
      wake_signal_post(&ring_buffer->space_free);
      wake_signal_post(&ring_buffer->data_ready);
      return;
    }
  }
}

void ring_buffer_shutdown(RingBuffer *ring_buffer) {
  if (!ring_buffer)
    return;
  int current_state =
      atomic_load_explicit(&ring_buffer->state, memory_order_acquire);
  if (is_terminal_state(current_state))
    return;
  atomic_store_explicit(&ring_buffer->state, RING_BUFFER_STATE_SHUTDOWN,
                        memory_order_seq_cst);

  // Wake any parked thread so it observes SHUTDOWN
  wake_signal_post(&ring_buffer->space_free);
  wake_signal_post(&ring_buffer->data_ready);
}

size_t ring_buffer_get_size(RingBuffer *ring_buffer) {
  if (!ring_buffer)
    return 0;
  int current_state =
      atomic_load_explicit(&ring_buffer->state, memory_order_acquire);
  if (current_state == RING_BUFFER_STATE_UNINITIALIZED ||
      current_state == RING_BUFFER_STATE_DESTROYED)
    return 0;
  size_t write_index =
      atomic_load_explicit(&ring_buffer->write_pos, memory_order_acquire);
  size_t read_index =
      atomic_load_explicit(&ring_buffer->read_pos, memory_order_acquire);
  return used_bytes_between(read_index, write_index, ring_buffer->capacity);
}

size_t ring_buffer_get_capacity(RingBuffer *ring_buffer) {
  if (!ring_buffer)
    return 0;
  int current_state =
      atomic_load_explicit(&ring_buffer->state, memory_order_acquire);
  if (current_state == RING_BUFFER_STATE_UNINITIALIZED ||
      current_state == RING_BUFFER_STATE_DESTROYED)
    return 0;
  return ring_buffer->capacity;
}

void ring_buffer_clear(RingBuffer *ring_buffer) {
  if (!ring_buffer)
    return;
  int current_state =
      atomic_load_explicit(&ring_buffer->state, memory_order_acquire);
  if (current_state == RING_BUFFER_STATE_UNINITIALIZED ||
      current_state == RING_BUFFER_STATE_DESTROYED)
    return;

  // Request the clear instead of writing read_pos here: read_pos must only be
  // written by the consumer, otherwise a concurrent read can store its own
  // position over ours and silently undo the clear. The consumer discards
  // everything published up to this write position on its next read.
  size_t write_index =
      atomic_load_explicit(&ring_buffer->write_pos, memory_order_acquire);
  atomic_store_explicit(&ring_buffer->clear_target, write_index,
                        memory_order_release);
}

RingBufferState ring_buffer_get_state(const RingBuffer *ring_buffer) {
  if (!ring_buffer)
    return RING_BUFFER_STATE_UNINITIALIZED;
  return (RingBufferState)atomic_load_explicit(&ring_buffer->state,
                                               memory_order_acquire);
}
