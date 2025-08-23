#ifndef ARENA_H_
#define ARENA_H_

#include "types.h" // Needed for MemoryArena struct definition
#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Initializes a memory arena with a specified capacity.
 * @param arena Pointer to the MemoryArena struct to initialize.
 * @param capacity The total size of the memory block to allocate.
 * @return true on success, false on memory allocation failure.
 */
bool mem_arena_init(MemoryArena* arena, size_t capacity);

/**
 * @brief Allocates a block of memory from the arena.
 * This is a simple, fast bump-pointer allocator.
 * @param arena Pointer to the initialized MemoryArena.
 * @param size The number of bytes to allocate.
 * @return A void pointer to the allocated memory, or NULL if the arena is full.
 */
void* mem_arena_alloc(MemoryArena* arena, size_t size);

/**
 * @brief Destroys a memory arena, freeing its main memory block.
 * @param arena Pointer to the MemoryArena to destroy.
 */
void mem_arena_destroy(MemoryArena* arena);

#endif // ARENA_H_
