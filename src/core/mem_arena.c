/**
 * @file mem_arena.c
 * @brief Thread-safe dynamic chained-block memory arena allocator.
 */

#include "mem_arena.h"
#include "config/constants.h"
#include "log.h"
#include "platform.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

// Ensure arena alignment is a power of 2 for our bitwise alignment math
_Static_assert((MEM_ARENA_ALIGNMENT & (MEM_ARENA_ALIGNMENT - 1)) == 0,
               "MEM_ARENA_ALIGNMENT must be a power of 2");

struct ArenaBlock {
  struct ArenaBlock *next;
  size_t capacity;
  size_t offset;
  void *memory;
};

static ArenaBlock *create_arena_block(size_t capacity) {
  size_t header_size = (sizeof(ArenaBlock) + MEM_ARENA_ALIGNMENT - 1) &
                       ~(MEM_ARENA_ALIGNMENT - 1);
  size_t aligned_capacity =
      (capacity + MEM_ARENA_ALIGNMENT - 1) & ~(MEM_ARENA_ALIGNMENT - 1);
  size_t total_bytes = header_size + aligned_capacity;

  void *raw = aligned_alloc(MEM_ARENA_ALIGNMENT, total_bytes);
  if (!raw) {
    return NULL;
  }
  ArenaBlock *block = (ArenaBlock *)raw;
  block->next = NULL;
  block->capacity = aligned_capacity;
  block->offset = 0;
  block->memory = (char *)raw + header_size;
  return block;
}

static void free_arena_block(ArenaBlock *block) {
  if (block) {
    aligned_free(block);
  }
}

bool mem_arena_init(MemoryArena *arena) {
  if (!arena)
    return false;

  memset(arena, 0, sizeof(*arena));

  if (pthread_mutex_init(&arena->lock, NULL) != 0) {
    log_fatal("Failed to initialize memory arena mutex.");
    return false;
  }

  uint64_t total_ram = platform_get_total_memory();

  size_t default_chunk = MEM_ARENA_CHUNK_SIZE_BYTES;
  size_t max_cap;

  if (total_ram > 0) {
    max_cap =
        (size_t)((total_ram * (uint64_t)MEM_ARENA_RAM_PERCENT_CAP) / 100ULL);
    // On 32-bit systems, clamp to 1.5 GB to prevent virtual address space
    // exhaustion
    if (sizeof(void *) == 4 && max_cap > (size_t)MEM_ARENA_32BIT_CAP) {
      max_cap = (size_t)MEM_ARENA_32BIT_CAP;
    }
  } else {
    // Fallback if system RAM query is unavailable
    max_cap =
        (sizeof(void *) == 4) ? (512 * 1024 * 1024) : (1024 * 1024 * 1024);
  }

  if (max_cap < default_chunk) {
    max_cap = default_chunk;
  }

  arena->default_chunk_size = default_chunk;
  arena->max_capacity = max_cap;
  arena->total_allocated = 0;
  arena->first_block = NULL;
  arena->current_block = NULL;
  arena->state = MEM_ARENA_STATE_UNINITIALIZED;

  // Allocate initial 32 MB chunk immediately
  ArenaBlock *first = create_arena_block(arena->default_chunk_size);
  if (!first) {
    log_fatal("Failed to allocate initial memory arena block (%zu bytes).",
              arena->default_chunk_size);
    pthread_mutex_destroy(&arena->lock);
    arena->state = MEM_ARENA_STATE_DESTROYED;
    return false;
  }

  arena->first_block = first;
  arena->current_block = first;
  arena->total_allocated = first->capacity;
  arena->state = MEM_ARENA_STATE_ACTIVE;

  log_debug("Memory arena initialized: %zu MB (limit: %zu MB)",
            arena->total_allocated / (1024 * 1024),
            arena->max_capacity / (1024 * 1024));

  return true;
}

void *mem_arena_alloc(MemoryArena *arena, size_t size, bool zero_memory) {
  if (!arena || arena->state != MEM_ARENA_STATE_ACTIVE || size == 0)
    return NULL;

  size_t aligned_size =
      (size + MEM_ARENA_ALIGNMENT - 1) & ~(MEM_ARENA_ALIGNMENT - 1);

  pthread_mutex_lock(&arena->lock);

  // Fast path: current block has enough space
  if (arena->current_block && (arena->current_block->offset + aligned_size <=
                               arena->current_block->capacity)) {
    void *ptr =
        (char *)arena->current_block->memory + arena->current_block->offset;
    arena->current_block->offset += aligned_size;
    pthread_mutex_unlock(&arena->lock);

    if (zero_memory) {
      memset(ptr, 0, size);
    }
    return ptr;
  }

  // Slow path: need a new block
  size_t new_capacity = (aligned_size > arena->default_chunk_size)
                            ? aligned_size
                            : arena->default_chunk_size;

  // Check hard cap
  if (arena->max_capacity > 0 &&
      (arena->total_allocated + new_capacity > arena->max_capacity)) {
    log_error("Memory arena exhausted maximum capacity limit (%zu bytes). "
              "Allocated: %zu bytes, Requested chunk: %zu bytes.",
              arena->max_capacity, arena->total_allocated, new_capacity);
    pthread_mutex_unlock(&arena->lock);
    return NULL;
  }

  ArenaBlock *new_block = create_arena_block(new_capacity);
  if (!new_block) {
    log_error("Failed to allocate new memory arena block (%zu bytes).",
              new_capacity);
    pthread_mutex_unlock(&arena->lock);
    return NULL;
  }

  arena->total_allocated += new_block->capacity;
  log_debug("Memory arena expanded: +%zu MB (total: %zu MB, limit: %zu MB)",
            new_block->capacity / (1024 * 1024),
            arena->total_allocated / (1024 * 1024),
            arena->max_capacity / (1024 * 1024));
  void *ptr = new_block->memory;
  new_block->offset = aligned_size;

  if (aligned_size > arena->default_chunk_size) {
    // Dedicated oversized block: link at head so current_block remains
    // available
    new_block->next = arena->first_block;
    arena->first_block = new_block;
    if (!arena->current_block) {
      arena->current_block = new_block;
    }
  } else {
    // Normal chunk expansion: chain to current block and advance current_block
    if (arena->current_block) {
      arena->current_block->next = new_block;
      arena->current_block = new_block;
    } else {
      arena->first_block = new_block;
      arena->current_block = new_block;
    }
  }

  pthread_mutex_unlock(&arena->lock);

  if (zero_memory) {
    memset(ptr, 0, size);
  }
  return ptr;
}

void mem_arena_destroy(MemoryArena *arena) {
  if (!arena || arena->state != MEM_ARENA_STATE_ACTIVE)
    return;

  pthread_mutex_lock(&arena->lock);
  ArenaBlock *block = arena->first_block;
  while (block) {
    ArenaBlock *next = block->next;
    free_arena_block(block);
    block = next;
  }
  arena->first_block = NULL;
  arena->current_block = NULL;
  arena->total_allocated = 0;
  arena->max_capacity = 0;
  arena->state = MEM_ARENA_STATE_DESTROYED;
  pthread_mutex_unlock(&arena->lock);
  pthread_mutex_destroy(&arena->lock);
}
