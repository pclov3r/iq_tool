/**
 * @file file_writer.h
 * @brief Defines the interface for writing sample data to an output file or stream.
 *
 * This module uses a factory pattern to abstract the details of the output
 * container format. It provides a generic interface (`FileWriterOps`) that can be
 * implemented by different "writers" (e.g., for raw binary files or WAV files).
 *
 * The file_writer_init() function selects the correct implementation based on the
 * user's configuration and populates a context struct with the appropriate
 * function pointers.
 */

#ifndef FILE_WRITER_H_
#define FILE_WRITER_H_

#include <stdbool.h>
#include <stddef.h>

// --- Forward Declarations ---
// These break circular dependencies and reduce include overhead by allowing us
// to use pointers to these types without needing their full definitions.
struct AppConfig;
struct AppResources;
struct MemoryArena;
struct FileWriterContext;

// --- Type Definitions ---

/**
 * @struct FileWriterOps
 * @brief A structure of function pointers defining the interface for a file writer.
 *
 * This "vtable" allows the core application logic to interact with different
 * output formats (raw, WAV, etc.) through a common, generic interface.
 */
typedef struct FileWriterOps {
    /**
     * @brief Opens the output file or stream.
     * @param ctx Pointer to the context, which will hold the file handle.
     * @param config The application configuration.
     * @param resources The application resources.
     * @param arena The memory arena for any necessary allocations.
     * @return true on success, false on failure.
     */
    bool (*open)(struct FileWriterContext* ctx, const struct AppConfig* config, struct AppResources* resources, struct MemoryArena* arena);

    /**
     * @brief Writes a block of data to the output.
     * @param ctx Pointer to the context containing the file handle.
     * @param buffer Pointer to the data to write.
     * @param bytes_to_write The number of bytes to write from the buffer.
     * @return The number of bytes successfully written.
     */
    size_t (*write)(struct FileWriterContext* ctx, const void* buffer, size_t bytes_to_write);

    /**
     * @brief Closes the output file or stream and releases resources.
     * @param ctx Pointer to the context to be cleaned up.
     */
    void (*close)(struct FileWriterContext* ctx);

    /**
     * @brief Gets the total number of bytes written so far.
     * @param ctx A read-only pointer to the context.
     * @return The total bytes written as a 64-bit integer.
     */
    long long (*get_total_bytes_written)(const struct FileWriterContext* ctx);
} FileWriterOps;

/**
 * @struct FileWriterContext
 * @brief Holds the state and function pointers for an active file writer instance.
 */
typedef struct FileWriterContext {
    void*         private_data;         ///< Pointer to implementation-specific data (e.g., FILE* or SNDFILE*).
    FileWriterOps api;                  ///< The set of function pointers for the selected writer implementation.
    long long     total_bytes_written;  ///< A running total of bytes written through this context.
} FileWriterContext;


// --- Function Declarations ---

/**
 * @brief Initializes the file writer context based on user configuration.
 *
 * This function acts as a factory, selecting the correct set of operations
 * (raw or WAV) and attaching them to the context struct. It does not open
 * the file; that is handled by the `open` function pointer within the ops struct.
 *
 * @param ctx Pointer to the FileWriterContext to initialize.
 * @param config The application configuration, used to determine which writer to use.
 * @return true on success, false on failure (e.g., unknown output type).
 */
bool file_writer_init(struct FileWriterContext* ctx, const struct AppConfig* config);

#endif // FILE_WRITER_H_
