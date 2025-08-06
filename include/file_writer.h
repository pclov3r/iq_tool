#ifndef FILE_WRITER_H_
#define FILE_WRITER_H_

#include "types.h"
#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Initializes the file writer context based on user configuration.
 *
 * This function acts as a factory, selecting the correct set of operations
 * (raw or WAV) and attaching them to the context struct. It does not open
 * the file; that is handled by the `open` function pointer within the ops struct.
 *
 * @param ctx Pointer to the FileWriterContext to initialize.
 * @param config The application configuration, used to determine which writer to use.
 * @return true on success, false on failure (e.g., unknown type).
 */
bool file_writer_init(FileWriterContext* ctx, const AppConfig* config);

#endif // FILE_WRITER_H_
