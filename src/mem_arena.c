/**
 * @file mem_arena.c
 */

// memory_arena.c
#include "mem_arena.h"
#include "log.h"
#include "constants.h"
#include "platform.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// Ensure arena alignment is a power of 2 for our bitwise alignment math
_Static_assert((MEM_ARENA_ALIGNMENT & (MEM_ARENA_ALIGNMENT - 1)) == 0, "MEM_ARENA_ALIGNMENT must be a power of 2");

/**
 * @brief Initializes a memory arena with a specified capacity.
 * @param arena Pointer to the MemoryArena struct to initialize.
 * @param capacity The total size of the memory block to allocate.
 * @return true on success, false on memory allocation failure.
 */
bool mem_arena_init(MemoryArena* arena, size_t capacity) {
    if (!arena) return false;
    if (capacity == 0) {
        log_fatal("Cannot initialize memory arena with zero capacity.");
        return false;
    }
    // C11: Use standard aligned_alloc. The size MUST be a multiple of the alignment.
    size_t aligned_capacity = (capacity + MEM_ARENA_ALIGNMENT - 1) & ~(MEM_ARENA_ALIGNMENT - 1);
    arena->memory = aligned_alloc(MEM_ARENA_ALIGNMENT, aligned_capacity);

    if (!arena->memory) {
        log_fatal("Failed to allocate memory for setup arena (%zu bytes).", capacity);
        return false;
    }
    arena->capacity = capacity;
    atomic_init(&arena->offset, 0);
    log_debug("Initialized setup memory arena with %zu bytes.", capacity);
    return true;
}

/**
 * @brief Allocates a block of memory from the arena.
 * This is a simple, fast bump-pointer allocator.
 * @param arena Pointer to the initialized MemoryArena.
 * @param size The number of bytes to allocate.
 * @param zero_memory If true, the allocated memory will be zero-initialized.
 * @return A void pointer to the allocated memory, or NULL if the arena is full.
 */
void* mem_arena_alloc(MemoryArena* arena, size_t size, bool zero_memory) {
    if (!arena || !arena->memory) return NULL;

    if (size > arena->capacity) {
        log_fatal("Requested arena allocation size (%zu) exceeds total capacity.", size);
        assert(size <= arena->capacity && "Allocation request exceeds total arena capacity.");
        return NULL;
    }

    // Align the size to the next multiple of the alignment constant for performance
    size_t aligned_size = (size + MEM_ARENA_ALIGNMENT - 1) & ~(MEM_ARENA_ALIGNMENT - 1);

    size_t old_offset = atomic_fetch_add_explicit(&arena->offset, aligned_size, memory_order_relaxed);
    if (old_offset + aligned_size > arena->capacity) {
        log_error("Memory arena exhausted. Requested %zu bytes, but only %zu remaining.",
                  size,
                  (old_offset > arena->capacity) ? 0 : (arena->capacity - old_offset));
        return NULL;
    }
    void* ptr = (char*)arena->memory + old_offset;
    // FIX: Make zero-initialization optional for performance.
    if (zero_memory) {
        memset(ptr, 0, size);
    }
    return ptr;
}

/**
 * @brief Destroys a memory arena, freeing its main memory block.
 * @param arena Pointer to the MemoryArena to destroy.
 */
void mem_arena_destroy(MemoryArena* arena) {
    if (arena) {
        if (arena->memory) {
        aligned_free(arena->memory);
            arena->memory = NULL;
        }
        arena->capacity = 0;
        atomic_store(&arena->offset, 0);
    }
}
