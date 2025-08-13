// src/input_source.h

#ifndef INPUT_SOURCE_H_
#define INPUT_SOURCE_H_

#include <stdbool.h>
#include "types.h" // Include types.h to get the full definitions

/**
 * @brief Context struct to pass common data to input source functions.
 *        This avoids global variables and makes functions more testable.
 */
typedef struct InputSourceContext {
    const AppConfig* config;     // READ-ONLY: Input sources should not change the app config.
    AppResources* resources;     // READ-WRITE: Input sources must populate and update resource fields.
} InputSourceContext;


/**
 * @brief The interface definition: a struct of function pointers.
 *        Any concrete input source (WAV, SDRplay, HackRF) will provide its own
 *        set of functions that match these pointers.
 */
typedef struct InputSourceOps {
    /**
     * @brief Performs initial setup (e.g., open file, select SDR device, set SDR parameters).
     * @return true on success, false on failure.
     */
    bool (*initialize)(InputSourceContext* ctx);

    /**
     * @brief Starts the actual streaming/reading of data. This function will be called
     *        in the reader thread and should block until shutdown or an error occurs.
     * @return NULL on normal exit/shutdown.
     */
    void* (*start_stream)(InputSourceContext* ctx);

    /**
     * @brief Gracefully stops the data stream (e.g., stop RX on SDR).
     */
    void (*stop_stream)(InputSourceContext* ctx);

    /**
     * @brief Releases all resources allocated by the input source (e.g., close handles, free memory).
     */
    void (*cleanup)(InputSourceContext* ctx);

    /**
     * @brief Populates a generic summary struct with details specific to this input source.
     *        The context is const because this function should only read data, not modify it.
     * @param ctx A read-only pointer to the application context.
     * @param info A pointer to the summary struct to be populated.
     */
    void (*get_summary_info)(const InputSourceContext* ctx, InputSummaryInfo* info);

    /**
     * @brief Validates and post-processes the command-line options specific to this input source.
     * @param config A pointer to the application configuration, which can be modified.
     * @return true if the options are valid, false otherwise.
     */
    bool (*validate_options)(AppConfig* config); // <<< MODIFIED: config is now non-const

    /**
     * @brief Reports whether the input source has a known, finite length (like a file).
     *        This helps generic functions determine if the source is a live stream
     *        versus a finite file.
     * @return true if the source has a known length, false otherwise (e.g., for a live SDR stream).
     */
    bool (*has_known_length)(void);

} InputSourceOps;

#endif // INPUT_SOURCE_H_
