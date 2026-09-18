/**
 * @file mem_arena.h
 */

#ifndef MEM_ARENA_H_
#define MEM_ARENA_H_

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Forward declaration of internal chunk block
typedef struct ArenaBlock ArenaBlock;

// --- Struct Definition ---

/**
 * @struct MemoryArena
 * @brief Dynamic chained-block memory arena allocator.
 *
 * Starts lean with a small initial block, and dynamically allocates
 * additional blocks as needed up to an intelligent memory-based cap.
 * Preserves 100% pointer stability and thread safety.
 *
 * This struct should be treated as an opaque handle by client code and only
 * manipulated through the mem_arena_* functions.
 */
typedef struct MemoryArena {
  ArenaBlock *first_block;   ///< Head of the linked list of blocks.
  ArenaBlock *current_block; ///< Current active block for bump allocations.
  size_t default_chunk_size; ///< Standard chunk size for subsequent blocks.
  size_t max_capacity; ///< Hard cap on total memory across all blocks (bytes).
  size_t
      total_allocated;  ///< Total bytes currently allocated across all blocks.
  pthread_mutex_t lock; ///< Mutex ensuring thread-safe allocation and growth.
  bool is_initialized;  ///< Tracks whether the arena is initialized.
} MemoryArena;

// --- Function Declarations ---

/**
 * @brief Initializes a dynamic memory arena with a 32 MB initial chunk and 80%
 * RAM cap.
 * @param arena Pointer to the MemoryArena struct to initialize.
 * @return true on success, false on memory allocation failure.
 */
bool mem_arena_init(MemoryArena *arena);

/**
 * @brief Allocates a block of memory from the arena.
 *
 * This function is now thread-safe. It is intended for use during the
 * setup phase of the application.
 *
 * @param arena Pointer to the initialized MemoryArena.
 * @param size The number of bytes to allocate.
 * @param zero_memory If true, the allocated memory will be zero-initialized.
 * @return A void pointer to the allocated memory, or NULL if the arena is full.
 */
void *mem_arena_alloc(MemoryArena *arena, size_t size, bool zero_memory);

/**
 * @brief Destroys a memory arena, freeing its main memory block.
 * @param arena Pointer to the MemoryArena to destroy.
 */
void mem_arena_destroy(MemoryArena *arena);

#endif // MEM_ARENA_H_
