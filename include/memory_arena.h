#ifndef MEMORY_ARENA_H_
#define MEMORY_ARENA_H_

#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>

// --- Struct Definition ---

/**
 * @struct MemoryArena
 * @brief Manages a single large block of memory for fast, contiguous allocations.
 *
 * This struct should be treated as an opaque handle by client code and only
 * manipulated through the mem_arena_* functions.
 */
typedef struct MemoryArena {
    void*  memory;      ///< Pointer to the start of the large allocated memory block.
    size_t capacity;    ///< The total size in bytes of the memory block.
    size_t offset;      ///< The current offset in bytes for the next allocation (the "bump pointer").
    pthread_mutex_t mutex; ///< Mutex to make allocations thread-safe.
} MemoryArena;


// --- Function Declarations ---

/**
 * @brief Initializes a memory arena with a specified capacity.
 * @param arena Pointer to the MemoryArena struct to initialize.
 * @param capacity The total size of the memory block to allocate.
 * @return true on success, false on memory allocation failure.
 */
bool mem_arena_init(MemoryArena* arena, size_t capacity);

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
void* mem_arena_alloc(MemoryArena* arena, size_t size, bool zero_memory);

/**
 * @brief Destroys a memory arena, freeing its main memory block.
 * @param arena Pointer to the MemoryArena to destroy.
 */
void mem_arena_destroy(MemoryArena* arena);

#endif // MEMORY_ARENA_H_
