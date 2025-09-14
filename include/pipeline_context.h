/**
 * @file pipeline_context.h
 * @brief Defines the context structure passed to all pipeline thread functions.
 *
 * This is a simple container struct that bundles pointers to the main application
 * configuration (`AppConfig`) and resources (`AppResources`). It provides a clean
 * and consistent way to pass the entire application state to the entry point of
 * each processing thread.
 */

#ifndef PIPELINE_CONTEXT_H_
#define PIPELINE_CONTEXT_H_

// --- Forward Declarations ---
// We use forward declarations here to avoid including the full, large headers
// for AppConfig and AppResources. This keeps this header lightweight and
// dependency-free.
struct AppConfig;
struct AppResources;

// --- Struct Definition ---

/**
 * @struct PipelineContext
 * @brief A container for passing the application's primary state objects to threads.
 */
typedef struct PipelineContext {
    struct AppConfig*   config;     ///< Pointer to the application's configuration settings.
    struct AppResources* resources;   ///< Pointer to the application's allocated resources.
} PipelineContext;

#endif // PIPELINE_CONTEXT_H_
