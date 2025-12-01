// memory_arena.c

#include "memory_arena.h"
#include "log.h"
#include "constants.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
#include <malloc.h> // For _aligned_malloc and _aligned_free
#endif

/**
 * @brief Initializes a memory arena with a specified capacity.
 * @param arena Pointer to the MemoryArena struct to initialize.
 * @param capacity The total size of the memory block to allocate.
 * @return true on success, false on memory allocation failure.
 */
bool mem_arena_init(MemoryArena* arena, size_t capacity) {
    if (!arena) return false;

    if (capacity == 0) {
        log_fatal("Memory arena initialized with zero capacity.");
        return false;
    }

#ifdef _WIN32
    // Windows: Use _aligned_malloc to ensure MEM_ARENA_ALIGNMENT
    arena->memory = _aligned_malloc(capacity, MEM_ARENA_ALIGNMENT);
#else
    // POSIX: Use posix_memalign to ensure MEM_ARENA_ALIGNMENT
    void* ptr = NULL;
    int ret = posix_memalign(&ptr, MEM_ARENA_ALIGNMENT, capacity);
    if (ret == 0) {
        arena->memory = ptr;
    } else {
        arena->memory = NULL;
    }
#endif

    if (!arena->memory) {
        log_fatal("Failed to allocate memory for setup arena (%zu bytes).", capacity);
        return false;
    }
    arena->capacity = capacity;
    arena->offset = 0;

    // Initialize the mutex
    int ret = pthread_mutex_init(&arena->mutex, NULL);
    if (ret != 0) {
        log_fatal("Failed to initialize memory arena mutex: %s", strerror(ret));
        if (arena->memory) {
#ifdef _WIN32
            _aligned_free(arena->memory);
#else
            free(arena->memory);
#endif
            arena->memory = NULL;
        }
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
 * @param zero_memory If true, the allocated memory will be zero-initialized.
 * @return A void pointer to the allocated memory, or NULL if the arena is full.
 */
void* mem_arena_alloc(MemoryArena* arena, size_t size, bool zero_memory) {
    // Align the size to the next multiple of the alignment constant for performance
    size_t aligned_size = (size + MEM_ARENA_ALIGNMENT - 1) & ~(MEM_ARENA_ALIGNMENT - 1);

    if (!arena) {
        return NULL;
    }

    pthread_mutex_lock(&arena->mutex);

    // FIX: Use an integer-overflow-safe check and log a non-fatal error.
    if (!arena->memory || (aligned_size > arena->capacity - arena->offset)) {
        log_error("Memory arena exhausted. Requested %zu bytes, but only %zu remaining.",
                  size, arena->capacity - arena->offset);
        pthread_mutex_unlock(&arena->mutex);
        return NULL;
    }

    void* ptr = (char*)arena->memory + arena->offset;
    arena->offset += aligned_size;

    pthread_mutex_unlock(&arena->mutex);

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
        pthread_mutex_destroy(&arena->mutex);
        if (arena->memory) {
#ifdef _WIN32
            _aligned_free(arena->memory);
#else
            free(arena->memory);
#endif
            arena->memory = NULL;
        }
        arena->capacity = 0;
        arena->offset = 0;
    }
}
