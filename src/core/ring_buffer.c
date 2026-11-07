/**
 * @file ring_buffer.c
 * @brief Lock-free single-producer single-consumer ring buffer implementation.
 */

#include "ring_buffer.h"
#include "config/constants.h"
#include "log.h"
#include "mem_arena.h"
#include "platform.h"
#include <pthread.h>
#include <stdalign.h>  // C11 Alignment
#include <stdatomic.h> // C11 Atomics
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <malloc.h>
#include <windows.h>
#else
#include <errno.h>
#include <time.h>
#endif

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

  // Isolates the reader index from the state and mutexes below.
  alignas(CACHE_LINE_PADDING) atomic_int state;

  // Synchronization for Backpressure (Producer Waiting)
  // These are NOT used by the lock-free writer (SDR hardware thread).
  pthread_mutex_t sync_mutex;
  pthread_cond_t space_free_cond;

  // Synchronization for Latency (Consumer Waiting)
  // Used to wake up the reader immediately when a block arrives.
#ifdef _WIN32
  HANDLE block_ready_event;
#else
  // Replaced sem_t with manual Auto-Reset Event logic to match Windows behavior
  // and ensure CLOCK_MONOTONIC usage (immune to time jumps).
  pthread_mutex_t event_mutex;
  pthread_cond_t event_cond;
  bool event_signaled;
#endif
};

static inline bool is_writable_state(int state) {
  return state == RING_BUFFER_STATE_ACTIVE ||
         state == RING_BUFFER_STATE_CONSUMER_WAITING ||
         state == RING_BUFFER_STATE_PRODUCER_WAITING;
}

static inline void wake_waiting_consumer(RingBuffer *iob) {
  int current_state =
      atomic_load_explicit(&iob->state, memory_order_seq_cst);
  if (current_state == RING_BUFFER_STATE_CONSUMER_WAITING) {
    int expected_state = RING_BUFFER_STATE_CONSUMER_WAITING;
    atomic_compare_exchange_strong_explicit(
        &iob->state, &expected_state, RING_BUFFER_STATE_ACTIVE,
        memory_order_acq_rel, memory_order_acquire);
#ifdef _WIN32
    if (iob->block_ready_event) {
      SetEvent(iob->block_ready_event);
    }
#else
    pthread_mutex_lock(&iob->event_mutex);
    iob->event_signaled = true;
    pthread_cond_signal(&iob->event_cond);
    pthread_mutex_unlock(&iob->event_mutex);
#endif
  }
}

RingBuffer *ring_buffer_create(size_t capacity, MemoryArena *arena) {
  RingBuffer *iob =
      (RingBuffer *)mem_arena_alloc(arena, sizeof(RingBuffer), true);
  if (!iob) {
    log_fatal("Failed to allocate memory for RingBuffer struct from arena.");
    return NULL;
  }

  size_t aligned_capacity =
      (capacity + MEM_ARENA_ALIGNMENT - 1) & ~(MEM_ARENA_ALIGNMENT - 1);

  iob->buffer =
      (unsigned char *)mem_arena_alloc(arena, aligned_capacity, false);
  if (!iob->buffer) {
    log_fatal(
        "Failed to allocate memory for RingBuffer data buffer from arena.");
    return NULL;
  }

  iob->capacity = aligned_capacity;
  atomic_init(&iob->write_pos, 0);
  atomic_init(&iob->read_pos, 0);
  atomic_init(&iob->state, RING_BUFFER_STATE_ACTIVE);

  pthread_mutex_init(&iob->sync_mutex, NULL);
  pthread_cond_init(&iob->space_free_cond, NULL);

#ifdef _WIN32
  iob->block_ready_event = CreateEvent(NULL, FALSE, FALSE, NULL);
  if (!iob->block_ready_event) {
    log_fatal("Failed to create RingBuffer event.");
    return NULL;
  }
#else
  if (pthread_mutex_init(&iob->event_mutex, NULL) != 0) {
    log_fatal("Failed to initialize RingBuffer event mutex.");
    return NULL;
  }

  pthread_condattr_t attr;
  pthread_condattr_init(&attr);
  pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);

  if (pthread_cond_init(&iob->event_cond, &attr) != 0) {
    log_fatal("Failed to initialize RingBuffer event cond.");
    pthread_condattr_destroy(&attr);
    return NULL;
  }
  pthread_condattr_destroy(&attr);

  iob->event_signaled = false;
#endif

  return iob;
}

void ring_buffer_destroy(RingBuffer *iob) {
  if (!iob)
    return;

  int expected = atomic_load(&iob->state);
  if (expected == RING_BUFFER_STATE_UNINITIALIZED ||
      expected == RING_BUFFER_STATE_DESTROYED) {
    return;
  }
  atomic_store(&iob->state, RING_BUFFER_STATE_DESTROYED);

  pthread_mutex_destroy(&iob->sync_mutex);
  pthread_cond_destroy(&iob->space_free_cond);

#ifdef _WIN32
  if (iob->block_ready_event) {
    CloseHandle(iob->block_ready_event);
    iob->block_ready_event = NULL;
  }
#else
  pthread_mutex_destroy(&iob->event_mutex);
  pthread_cond_destroy(&iob->event_cond);
#endif
}

// PRODUCER: High Priority, Lock-Free, Never Sleeps
size_t ring_buffer_write(RingBuffer *iob, const void *data, size_t bytes) {
  if (!iob || !data || bytes == 0 ||
      !is_writable_state(
          atomic_load_explicit(&iob->state, memory_order_acquire)))
    return 0;

  size_t write_index =
      atomic_load_explicit(&iob->write_pos, memory_order_relaxed);
  size_t read_index =
      atomic_load_explicit(&iob->read_pos, memory_order_acquire);
  size_t capacity = iob->capacity;

  size_t used_bytes = (write_index >= read_index)
                          ? (write_index - read_index)
                          : (capacity - (read_index - write_index));
  size_t available_space = (capacity - 1) - used_bytes;

  if (bytes > available_space) {
    return 0;
  }

  size_t first_chunk_size =
      (write_index + bytes > capacity) ? (capacity - write_index) : bytes;
  memcpy(iob->buffer + write_index, data, first_chunk_size);

  size_t second_chunk_size = bytes - first_chunk_size;
  if (second_chunk_size > 0) {
    memcpy(iob->buffer, (const unsigned char *)data + first_chunk_size,
           second_chunk_size);
  }

  atomic_store_explicit(&iob->write_pos, (write_index + bytes) % capacity,
                        memory_order_release);

  wake_waiting_consumer(iob);

  return bytes;
}

// PRODUCER: High Priority, Lock-Free, Scatter-Gather
size_t ring_buffer_write_packet(RingBuffer *iob, const void *header,
                                size_t h_length, const void *payload,
                                size_t p_length) {
  if (!iob || !header ||
      !is_writable_state(
          atomic_load_explicit(&iob->state, memory_order_acquire)))
    return 0;

  size_t total_bytes = h_length + p_length;
  size_t write_index =
      atomic_load_explicit(&iob->write_pos, memory_order_relaxed);
  size_t read_index =
      atomic_load_explicit(&iob->read_pos, memory_order_acquire);
  size_t capacity = iob->capacity;

  size_t used_bytes = (write_index >= read_index)
                          ? (write_index - read_index)
                          : (capacity - (read_index - write_index));
  size_t available_space = (capacity - 1) - used_bytes;

  if (total_bytes > available_space) {
    return 0; // Drop packet
  }

  // 1. Write Header
  size_t header_chunk1 =
      (write_index + h_length > capacity) ? (capacity - write_index) : h_length;
  memcpy(iob->buffer + write_index, header, header_chunk1);

  size_t header_chunk2 = h_length - header_chunk1;
  if (header_chunk2 > 0) {
    memcpy(iob->buffer, (const unsigned char *)header + header_chunk1,
           header_chunk2);
  }
  write_index = (write_index + h_length) % capacity;

  // 2. Write Payload (if exists)
  if (payload && p_length > 0) {
    size_t payload_chunk1 = (write_index + p_length > capacity)
                                ? (capacity - write_index)
                                : p_length;
    memcpy(iob->buffer + write_index, payload, payload_chunk1);

    size_t payload_chunk2 = p_length - payload_chunk1;
    if (payload_chunk2 > 0) {
      memcpy(iob->buffer, (const unsigned char *)payload + payload_chunk1,
             payload_chunk2);
    }
    write_index = (write_index + p_length) % capacity;
  }

  // 3. Update write_pos and signal ONCE
  atomic_store_explicit(&iob->write_pos, write_index, memory_order_release);

  wake_waiting_consumer(iob);

  return total_bytes;
}

// CONSUMER: Low Priority, Blocking (Sleeps if empty)
size_t ring_buffer_read(RingBuffer *iob, void *buffer, size_t max_bytes) {
  if (!iob || !buffer || max_bytes == 0)
    return 0;

  size_t write_index;
  size_t read_index;
  size_t capacity;
  size_t available_bytes;

  while (true) {
    write_index = atomic_load_explicit(&iob->write_pos, memory_order_acquire);
    read_index = atomic_load_explicit(&iob->read_pos, memory_order_relaxed);
    capacity = iob->capacity;

    int current_state = atomic_load_explicit(&iob->state, memory_order_acquire);
    if (current_state == RING_BUFFER_STATE_SHUTDOWN ||
        current_state == RING_BUFFER_STATE_DESTROYED ||
        current_state == RING_BUFFER_STATE_UNINITIALIZED)
      return 0;

    available_bytes = (write_index >= read_index)
                          ? (write_index - read_index)
                          : (capacity - (read_index - write_index));

    if (available_bytes > 0) {
      break;
    }

    if (current_state == RING_BUFFER_STATE_DRAINING) {
      return 0;
    }

    // 1. Transition ACTIVE -> CONSUMER_WAITING (seq_cst prevents reordering)
    int expected_state = RING_BUFFER_STATE_ACTIVE;
    if (!atomic_compare_exchange_strong_explicit(
            &iob->state, &expected_state, RING_BUFFER_STATE_CONSUMER_WAITING,
            memory_order_seq_cst, memory_order_seq_cst)) {
      if (expected_state == RING_BUFFER_STATE_DRAINING) {
        continue;
      }
      if (!is_writable_state(expected_state)) {
        return 0;
      }
    }

    // 2. Double-check if data arrived while transitioning to CONSUMER_WAITING
    write_index = atomic_load_explicit(&iob->write_pos, memory_order_acquire);
    available_bytes = (write_index >= read_index)
                          ? (write_index - read_index)
                          : (capacity - (read_index - write_index));
    if (available_bytes > 0) {
      int wake_expected = RING_BUFFER_STATE_CONSUMER_WAITING;
      atomic_compare_exchange_strong_explicit(
          &iob->state, &wake_expected, RING_BUFFER_STATE_ACTIVE,
          memory_order_acq_rel, memory_order_acquire);
      break;
    }

    // Buffer still empty, wait efficiently for the producer signal
#ifdef _WIN32
    if (iob->block_ready_event) {
      WaitForSingleObject(iob->block_ready_event, 100);
    }
#else
    struct timespec timeout_spec;
    clock_gettime(CLOCK_MONOTONIC, &timeout_spec);

    // Add 100ms
    timeout_spec.tv_nsec += 100000000;
    if (timeout_spec.tv_nsec >= 1000000000) {
      timeout_spec.tv_sec += 1;
      timeout_spec.tv_nsec -= 1000000000;
    }

    pthread_mutex_lock(&iob->event_mutex);

    // Auto-Reset Event Logic:
    // Wait until signaled or timeout.
    while (!iob->event_signaled &&
           atomic_load_explicit(&iob->state, memory_order_acquire) ==
               RING_BUFFER_STATE_CONSUMER_WAITING) {
      int wait_result = pthread_cond_timedwait(
          &iob->event_cond, &iob->event_mutex, &timeout_spec);
      if (wait_result == ETIMEDOUT)
        break;
    }

    // Clear flag immediately (Auto-Reset) so we sleep again next time
    // unless producer signals again.
    iob->event_signaled = false;

    pthread_mutex_unlock(&iob->event_mutex);
#endif
    int post_wait_expected = RING_BUFFER_STATE_CONSUMER_WAITING;
    atomic_compare_exchange_strong_explicit(
        &iob->state, &post_wait_expected, RING_BUFFER_STATE_ACTIVE,
        memory_order_acq_rel, memory_order_acquire);
  }

  size_t bytes_to_read =
      (max_bytes > available_bytes) ? available_bytes : max_bytes;

  size_t first_chunk_size = (read_index + bytes_to_read > capacity)
                                ? (capacity - read_index)
                                : bytes_to_read;
  memcpy(buffer, iob->buffer + read_index, first_chunk_size);

  size_t second_chunk_size = bytes_to_read - first_chunk_size;
  if (second_chunk_size > 0) {
    memcpy((unsigned char *)buffer + first_chunk_size, iob->buffer,
           second_chunk_size);
  }

  atomic_store_explicit(&iob->read_pos, (read_index + bytes_to_read) % capacity,
                        memory_order_release);

  // Signal Backpressure: Wake up producer ONLY if parked in PRODUCER_WAITING
  int post_read_state =
      atomic_load_explicit(&iob->state, memory_order_seq_cst);
  if (post_read_state == RING_BUFFER_STATE_PRODUCER_WAITING) {
    int wake_producer_expected = RING_BUFFER_STATE_PRODUCER_WAITING;
    atomic_compare_exchange_strong_explicit(
        &iob->state, &wake_producer_expected, RING_BUFFER_STATE_ACTIVE,
        memory_order_acq_rel, memory_order_acquire);
    pthread_mutex_lock(&iob->sync_mutex);
    pthread_cond_broadcast(&iob->space_free_cond);
    pthread_mutex_unlock(&iob->sync_mutex);
  }

  return bytes_to_read;
}

// BACKPRESSURE WAIT: Used by File Readers / Audio Demodulators (Producers)
void ring_buffer_wait_for_threshold(RingBuffer *iob, size_t target_size) {
  if (!iob ||
      !is_writable_state(atomic_load_explicit(&iob->state, memory_order_acquire)))
    return;

  if (ring_buffer_get_size(iob) <= target_size)
    return;

  pthread_mutex_lock(&iob->sync_mutex);
  while (ring_buffer_get_size(iob) > target_size) {
    int expected_state = RING_BUFFER_STATE_ACTIVE;
    if (!atomic_compare_exchange_strong_explicit(
            &iob->state, &expected_state, RING_BUFFER_STATE_PRODUCER_WAITING,
            memory_order_seq_cst, memory_order_seq_cst)) {
      if (!is_writable_state(expected_state)) {
        break;
      }
    }
    if (ring_buffer_get_size(iob) <= target_size) {
      break;
    }
    pthread_cond_wait(&iob->space_free_cond, &iob->sync_mutex);
    if (!is_writable_state(
            atomic_load_explicit(&iob->state, memory_order_acquire))) {
      break;
    }
  }
  int restore_expected = RING_BUFFER_STATE_PRODUCER_WAITING;
  atomic_compare_exchange_strong_explicit(
      &iob->state, &restore_expected, RING_BUFFER_STATE_ACTIVE,
      memory_order_acq_rel, memory_order_acquire);
  pthread_mutex_unlock(&iob->sync_mutex);
}

void ring_buffer_drain(RingBuffer *iob) {
  if (!iob)
    return;
  int expected_state = atomic_load_explicit(&iob->state, memory_order_acquire);
  while (is_writable_state(expected_state)) {
    if (atomic_compare_exchange_weak_explicit(
            &iob->state, &expected_state, RING_BUFFER_STATE_DRAINING,
            memory_order_acq_rel, memory_order_acquire)) {
      // Wake up backpressure waiters
      pthread_mutex_lock(&iob->sync_mutex);
      pthread_cond_broadcast(&iob->space_free_cond);
      pthread_mutex_unlock(&iob->sync_mutex);

      // Wake up consumer
#ifdef _WIN32
      if (iob->block_ready_event) {
        SetEvent(iob->block_ready_event);
      }
#else
      pthread_mutex_lock(&iob->event_mutex);
      iob->event_signaled = true;
      pthread_cond_signal(&iob->event_cond);
      pthread_mutex_unlock(&iob->event_mutex);
#endif
      return;
    }
  }
}

void ring_buffer_shutdown(RingBuffer *iob) {
  if (!iob)
    return;
  int current_state = atomic_load_explicit(&iob->state, memory_order_acquire);
  if (current_state == RING_BUFFER_STATE_UNINITIALIZED ||
      current_state == RING_BUFFER_STATE_DESTROYED ||
      current_state == RING_BUFFER_STATE_SHUTDOWN)
    return;
  atomic_store_explicit(&iob->state, RING_BUFFER_STATE_SHUTDOWN,
                        memory_order_release);

  pthread_mutex_lock(&iob->sync_mutex);
  pthread_cond_broadcast(&iob->space_free_cond);
  pthread_mutex_unlock(&iob->sync_mutex);

  // Wake up consumer
#ifdef _WIN32
  if (iob->block_ready_event) {
    SetEvent(iob->block_ready_event);
  }
#else
  pthread_mutex_lock(&iob->event_mutex);
  iob->event_signaled = true;
  pthread_cond_signal(&iob->event_cond);
  pthread_mutex_unlock(&iob->event_mutex);
#endif
}

size_t ring_buffer_get_size(RingBuffer *iob) {
  if (!iob)
    return 0;
  int current_state = atomic_load_explicit(&iob->state, memory_order_acquire);
  if (current_state == RING_BUFFER_STATE_UNINITIALIZED ||
      current_state == RING_BUFFER_STATE_DESTROYED)
    return 0;
  size_t write_index =
      atomic_load_explicit(&iob->write_pos, memory_order_acquire);
  size_t read_index =
      atomic_load_explicit(&iob->read_pos, memory_order_acquire);
  if (write_index >= read_index)
    return write_index - read_index;
  return iob->capacity - (read_index - write_index);
}

size_t ring_buffer_get_capacity(RingBuffer *iob) {
  if (!iob)
    return 0;
  int current_state = atomic_load_explicit(&iob->state, memory_order_acquire);
  if (current_state == RING_BUFFER_STATE_UNINITIALIZED ||
      current_state == RING_BUFFER_STATE_DESTROYED)
    return 0;
  return iob->capacity;
}

void ring_buffer_clear(RingBuffer *iob) {
  if (!iob)
    return;
  int current_state = atomic_load_explicit(&iob->state, memory_order_acquire);
  if (current_state == RING_BUFFER_STATE_UNINITIALIZED ||
      current_state == RING_BUFFER_STATE_DESTROYED)
    return;
  // Safe Lock-Free Clear: Advance read_pos to match write_pos
  size_t write_index =
      atomic_load_explicit(&iob->write_pos, memory_order_acquire);
  atomic_store_explicit(&iob->read_pos, write_index, memory_order_release);

  int expected_state = RING_BUFFER_STATE_PRODUCER_WAITING;
  atomic_compare_exchange_strong_explicit(
      &iob->state, &expected_state, RING_BUFFER_STATE_ACTIVE,
      memory_order_acq_rel, memory_order_acquire);

  pthread_mutex_lock(&iob->sync_mutex);
  pthread_cond_broadcast(&iob->space_free_cond);
  pthread_mutex_unlock(&iob->sync_mutex);
}

RingBufferState ring_buffer_get_state(const RingBuffer *iob) {
  if (!iob)
    return RING_BUFFER_STATE_UNINITIALIZED;
  return (RingBufferState)atomic_load_explicit(&iob->state,
                                               memory_order_acquire);
}
