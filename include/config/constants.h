/**
 * @file constants.h
 * @brief Central configuration for application resource usage and performance
 * tuning.
 *
 * This file defines the key parameters that govern the memory footprint,
 * latency, and processing quality of the iq_tool. Values here represent a
 * balance between performance, memory usage, and stability. Adjust these values
 * to tune the application for specific hardware or use cases.
 */

#ifndef CONSTANTS_H_
#define CONSTANTS_H_

// =============================================================================
// == Tier 1: High-Level Application Behavior
// =============================================================================

#define APP_NAME "iq_tool"
#define PRESETS_FILENAME "iq_tool_presets.conf"

// Defines the interval in seconds for console status updates and warning
// throttles to the console.
#define CONSOLE_UPDATE_INTERVAL_SEC 1.0

// =============================================================================
// == Tier 2: Core Memory & ProcessChain Architecture
// =============================================================================
// These are the most critical parameters for controlling the application's
// memory footprint and real-time performance.

/**
 * @def MEM_ARENA_ALIGNMENT
 * @brief The memory alignment boundary for all allocations within the memory
 * arena.
 *
 * Purpose: To ensure that all pointers returned by the arena are aligned to a
 * boundary suitable for high-performance SIMD (SSE/AVX) instructions, which
 * are heavily used by DSP libraries like liquid-dsp.
 */
#define MEM_ARENA_ALIGNMENT 128

/**
 * @def CACHE_LINE_PADDING
 * @brief Byte boundary used to isolate atomic variables across cache lines.
 *
 * Purpose: Pushes shared variables (e.g. ring buffer read/write indices) into
 * separate cache lines and accounts for adjacent cache line prefetchers (128
 * bytes total) to eliminate false sharing between CPU cores.
 */
#define CACHE_LINE_PADDING 128

/**
 * @def DSP_MAX_MODULES
 * @brief Hard cap for the DspContext states[] array. Must always be >= chain
 * length.
 */
#define DSP_MAX_MODULES 16

/**
 * @def MAX_MANAGED_THREADS
 * @brief Maximum number of concurrent threads managed by ThreadManager.
 * Must accommodate input, chunker, output, watchdog, background, and DSP
 * stages.
 */
#define MAX_MANAGED_THREADS 32

/**
 * @def MEM_ARENA_CHUNK_SIZE_BYTES
 * @brief Default chunk size for dynamic memory arena allocations.
 */
#define MEM_ARENA_CHUNK_SIZE_BYTES (32 * 1024 * 1024) // 32 MB chunk

/**
 * @def MEM_ARENA_RAM_PERCENT_CAP
 * @brief Safety cap: maximum percentage of detected system RAM the arena can
 * grow to.
 */
#define MEM_ARENA_RAM_PERCENT_CAP 80 // 80% of total system RAM

/**
 * @def MEM_ARENA_32BIT_CAP
 * @brief Upper bound for 32-bit platforms to prevent virtual address space
 * exhaustion.
 */
#define MEM_ARENA_32BIT_CAP (1536ULL * 1024 * 1024) // 1.5 GB

/**
 * @def INPUT_BUFFER_DURATION_SEC
 * @brief Target duration (in seconds) for the input ring buffer.
 *
 * Purpose: Provides consistent latency tolerance across all sample rates.
 * The actual buffer size is calculated as: sample_rate × duration ×
 * bytes_per_sample
 *
 * Trade-off: 5 seconds provides good protection against driver hiccups while
 * remaining reasonable in memory usage. At 2.4 MHz this is ~48 MB, at 20 MHz
 * ~400 MB.
 */
#define INPUT_BUFFER_DURATION_SEC 5.0f

/**
 * @def INPUT_BUFFER_MIN_BYTES
 * @brief Minimum size for the input buffer (safety floor).
 *
 * Prevents excessively small buffers on very low sample rate signals.
 */
#define INPUT_BUFFER_MIN_BYTES (4 * 1024 * 1024) // 4 MB

/**
 * @def INPUT_BUFFER_MAX_BYTES
 * @brief Maximum size for the input buffer (safety ceiling).
 *
 * Prevents excessive RAM usage on very high sample rate signals.
 */
#define INPUT_BUFFER_MAX_BYTES (512 * 1024 * 1024) // 512 MB

/**
 * @def PROCESS_CHAIN_TARGET_BLOCK_SAMPLES
 * @brief The target number of samples for a processing block.
 *
 * This is calculated to ensure that the working set (Input Buffer + Output
 * Buffer) fits comfortably inside a standard CPU L2 Cache (256KB).
 *
 * Calculation: 12,288 samples * 8 bytes/sample (complex float) * 2 buffers
 * (Ping/Pong) = ~192 KB. This leaves ~64KB for instructions, stack, and OS
 * overhead.
 */
#define PROCESS_CHAIN_TARGET_BLOCK_SAMPLES 12288

/**
 * @def PROCESS_CHAIN_MIN_READ_SAMPLES
 * @brief The minimum number of samples to read from the input per cycle.
 *
 * Prevents excessive mutex locking overhead during extreme upsampling scenarios
 * (e.g. where the calculated input requirement might be < 10 samples).
 */
#define PROCESS_CHAIN_MIN_READ_SAMPLES 256

/**
 * @def PROCESS_CHAIN_TARGET_BUFFER_DURATION_SEC
 * @brief Target amount of time to buffer inside the processing chain.
 *
 * Instead of a hardcoded number of chunks, we calculate the depth dynamically
 * to ensure we have enough buffer to survive disk stalls (e.g. 2 seconds).
 */
#define PROCESS_CHAIN_TARGET_BUFFER_DURATION_SEC 2.0f

/**
 * @def PROCESS_CHAIN_MIN_CHUNKS
 * @brief Minimum number of chunks in the process_chain (Sanity Floor).
 */
#define PROCESS_CHAIN_MIN_CHUNKS 64

/**
 * @def PROCESS_CHAIN_MAX_CHUNKS
 * @brief Maximum number of chunks in the process_chain (Sanity Ceiling).
 * Prevents excessive RAM usage on very low sample rate signals.
 */
#define PROCESS_CHAIN_MAX_CHUNKS 16384

/**
 * @def PROCESS_CHAIN_BUFFER_PADDING_SAMPLES
 * @brief Safety padding to absorb SIMD/AVX pre-fetches and floating point
 * rounding jitter. (Resampler bursts and FFT blocks are explicitly calculated).
 */
#define PROCESS_CHAIN_BUFFER_PADDING_SAMPLES 64

// =============================================================================
// == Tier 3: DSP Algorithm Quality & Tuning
// =============================================================================

// Defines the default sharpness of user-defined FIR filters.
#define DEFAULT_FILTER_TRANSITION_FACTOR 0.25f

// --- Dynamic DSP Filter Attenuation ---
#define DEFAULT_FILTER_ATTENUATION_8BIT_DB 60.0f
#define DEFAULT_FILTER_ATTENUATION_16BIT_DB 100.0f
#define DEFAULT_FILTER_ATTENUATION_24BIT_DB 144.0f
#define DEFAULT_FILTER_ATTENUATION_32BIT_DB 150.0f

// =============================================================================
// == Tier 4: Sanity Checks & Hard Limits
// =============================================================================

#define PROCESS_CHAIN_MIN_RATE_SCALAR 0.001f
#define PROCESS_CHAIN_MAX_RATE_SCALAR 1000.0f
#define FILTER_MAX_CHAIN 5
#define PRESETS_MAX_COUNT 128
#define PRESETS_MAX_LINE_LENGTH 1024
#define APP_MAX_SUMMARY_ITEMS 16

// The absolute maximum number of samples allowed in a single process_chain
// chunk. Prevents OOM crashes if user requests extreme upsampling + extreme
// filter taps.
#define PROCESS_CHAIN_MAX_CHUNK_SAMPLES (4 * 1024 * 1024)
#define APP_MAX_PATH_BUFFER 4096

// =============================================================================
// == Tier 5: Application Lifecycle Tuning
// =============================================================================

/**
 * @def SDR_INITIALIZE_TIMEOUT_MS
 * @brief The maximum time to wait for an SDR driver to respond during initial
 * opening.
 */
#define SDR_INITIALIZE_TIMEOUT_MS 10000

// The interval in milliseconds at which the watchdog thread wakes up to check
// the input heartbeat.
#define WATCHDOG_INTERVAL_MS 2000

// The maximum time in milliseconds that can elapse without an input heartbeat
// before the watchdog triggers a shutdown. This must be longer than any
// input module's internal timeouts.
#define WATCHDOG_TIMEOUT_MS 8000

/**
 * @def NETWORK_SOCKET_TIMEOUT_MS
 * @brief The receive/send timeout for network sockets in milliseconds.
 *
 * Prevents the reader thread from hanging indefinitely if the server disappears
 * without closing the TCP connection (half-open socket).
 */
#define NETWORK_SOCKET_TIMEOUT_MS 5000

/**
 * @def NETWORK_MAX_CHUNK_BYTES
 * @brief Maximum chunk size (1 MB) for socket send/recv calls to prevent 32-bit
 * cast overflow.
 */
#define NETWORK_MAX_CHUNK_BYTES (1024 * 1024)

#endif // CONSTANTS_H_
