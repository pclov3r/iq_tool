#include "memory_arena.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>

/**
 * @brief Initializes a memory arena with a specified capacity.
 * @param arena Pointer to the MemoryArena struct to initialize.
 * @param capacity The total size of the memory block to allocate.
 * @return true on success, false on memory allocation failure.
 */
bool arena_init(MemoryArena* arena, size_t capacity) {
    if (!arena) return false;
    arena->capacity = capacity;
    arena->offset = 0;
    arena->memory = malloc(capacity);
    if (!arena->memory) {
        log_fatal("Failed to allocate memory for setup arena (%zu bytes).", capacity);
        return false;
    }
    log_debug("Initialized setup memory arena with %zu bytes.", capacity);
    return true;
}

/**
 * @brief Allocates a block of memory from the arena.
 * This is a simple, fast bump-pointer allocator.
 * @param arena Pointer to the initialized MemoryArena.
 * @param size The number of bytes to allocate.
 * @return A void pointer to the allocated memory, or NULL if the arena is full.
 */
void* arena_alloc(MemoryArena* arena, size_t size) {
    // Align the size to the next multiple of the pointer size for performance
    size_t aligned_size = (size + sizeof(void*) - 1) & ~(sizeof(void*) - 1);

    if (!arena || !arena->memory || arena->offset + aligned_size > arena->capacity) {
        log_fatal("Memory arena exhausted. Requested %zu bytes, but only %zu remaining.",
                  size, arena->capacity - arena->offset);
        return NULL;
    }

    void* ptr = (char*)arena->memory + arena->offset;
    arena->offset += aligned_size;
    // Zero the memory, mimicking calloc's behavior which the old code relied on
    memset(ptr, 0, size);
    return ptr;
}

/**
 * @brief Destroys a memory arena, freeing its main memory block.
 * @param arena Pointer to the MemoryArena to destroy.
 */
void arena_destroy(MemoryArena* arena) {
    if (arena && arena->memory) {
        free(arena->memory);
        arena->memory = NULL;
        arena->capacity = 0;
        arena->offset = 0;
    }
}
