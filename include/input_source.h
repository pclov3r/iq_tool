/**
 * @file input_source.h
 * @brief Defines the abstract interface for all I/Q data input sources.
 *
 * This file is the cornerstone of the modular input system. It defines a generic
 * interface (`ModuleApi`) using a structure of function pointers. Any
 * concrete input source, whether a file reader (WAV, raw) or a live SDR device
 * (RTL-SDR, SDRplay), must provide an implementation of this interface.
 *
 * It also defines the common data structures used to describe an input source's
 * properties and metadata.
 */

#ifndef INPUT_SOURCE_H_
#define INPUT_SOURCE_H_

#include <stdbool.h>
#include <time.h>
#include <stdint.h>
#include "constants.h" // For MAX_SUMMARY_ITEMS

// --- Forward Declarations ---
// These break circular dependencies and allow us to use pointers to these
// structs without needing their full definitions in this header.
struct AppConfig;
struct AppResources;
struct InputSummaryInfo;
struct InputSourceContext;


// --- Data Structures ---

/**
 * @struct InputSourceInfo
 * @brief Holds basic, essential information about the input source.
 */
typedef struct InputSourceInfo {
    int64_t frames;     ///< Total number of I/Q frames in the source. -1 for a live stream.
    int     samplerate; ///< The native sample rate of the source in Hz.
} InputSourceInfo;

/**
 * @struct SummaryItem
 * @brief A single key-value pair for displaying in the configuration summary.
 */
typedef struct SummaryItem {
    char label[64];
    char value[128];
} SummaryItem;

/**
 * @struct InputSummaryInfo
 * @brief A collection of SummaryItem objects to be displayed.
 */
typedef struct InputSummaryInfo {
    SummaryItem items[MAX_SUMMARY_ITEMS];
    int         count;
} InputSummaryInfo;

/**
 * @struct InputSourceContext
 * @brief A container passing the main application state to input source functions.
 */
typedef struct InputSourceContext {
    const struct AppConfig* config;
    struct AppResources*    resources;
} InputSourceContext;


// --- The Core Interface Definition ---

/**
 * @struct ModuleApi
 * @brief The "vtable" of function pointers that defines the input source interface.
 */
typedef struct ModuleApi {
    /**
     * @brief Performs initial setup (e.g., open file, select SDR device, set SDR parameters).
     * @param ctx The application context.
     * @return true on success, false on failure.
     */
    bool (*initialize)(struct InputSourceContext* ctx);

    /**
     * @brief Starts the data stream. This is a blocking call that runs in the reader thread.
     * @param ctx The application context.
     * @return NULL on normal exit.
     */
    void* (*start_stream)(struct InputSourceContext* ctx);

    /**
     * @brief Gracefully stops the data stream (e.g., cancels an async SDR read).
     * @param ctx The application context.
     */
    void (*stop_stream)(struct InputSourceContext* ctx);

    /**
     * @brief Releases all resources allocated by the input source.
     * @param ctx The application context.
     */
    void (*cleanup)(struct InputSourceContext* ctx);

    /**
     * @brief Populates a summary struct with details specific to this input source.
     * @param ctx A read-only pointer to the application context.
     * @param info A pointer to the summary struct to be populated.
     */
    void (*get_summary_info)(const struct InputSourceContext* ctx, struct InputSummaryInfo* info);

    /**
     * @brief Validates and post-processes command-line options specific to this module.
     * @param config A pointer to the application configuration, which can be modified.
     * @return true if the options are valid, false otherwise.
     */
    bool (*validate_options)(struct AppConfig* config);

    /**
     * @brief Validates generic options (e.g., --sdr-rf-freq) in the context of this module.
     * @param config A read-only pointer to the application configuration.
     * @return true if the generic options are valid for this module, false otherwise.
     */
    bool (*validate_generic_options)(const struct AppConfig* config);

    /**
     * @brief Reports whether the input source has a known, finite length (file vs. stream).
     * @return true if the source has a known length, false otherwise.
     */
    bool (*has_known_length)(void);

    // Optional function for file-based sources to perform initial I/Q correction.
    bool (*pre_stream_iq_correction)(struct InputSourceContext* ctx);

} ModuleApi;

#endif // INPUT_SOURCE_H_
