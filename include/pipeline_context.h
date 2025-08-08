// pipeline_context.h

#ifndef PIPELINE_CONTEXT_H_
#define PIPELINE_CONTEXT_H_

// Forward declarations for the main application structs
struct AppConfig;
struct AppResources;

// The definitive declaration of the PipelineContext struct,
// which is passed to all thread functions.
typedef struct {
    struct AppConfig* config;
    struct AppResources* resources;
} PipelineContext;

#endif // PIPELINE_CONTEXT_H_
