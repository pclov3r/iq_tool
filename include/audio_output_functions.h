/**
 * @file audio_output_functions.h
 * @brief Defines a unified audio output interface, abstracting OS sound drivers and buffers.
 */

#ifndef AUDIO_OUTPUT_FUNCTIONS_H_
#define AUDIO_OUTPUT_FUNCTIONS_H_

#include <stdbool.h>
#include <stddef.h>
#include "app_context.h"
#include "common_types.h" // For PipelineMode

// Forward declaration of the memory arena
struct MemoryArena;

// Opaque handle for the audio output instance
typedef struct AudioOutputContext AudioOutputContext;

/**
 * @brief Creates and starts an audio output stream.
 *
 * Initializes a lock-free ring buffer and the underlying OS audio driver (via Miniaudio).
 *
 * @param arena The memory arena to allocate the context from.
 * @param sample_rate The audio sample rate (e.g., 48000).
 * @param channels The number of audio channels (e.g., 2 for stereo).
 * @param buffer_size_bytes The size of the internal ring buffer.
 * @return A pointer to the AudioOutputContext handle, or NULL on failure.
 */
AudioOutputContext* audio_output_create(AppContext* app, int sample_rate, int channels, size_t buffer_size_bytes);

/**
 * @brief Writes PCM data to the audio output.
 *
 * Automatically handles conditional backpressure: it will block if processing
 * a file (to maintain 1x playback speed), but will drop samples if processing
 * a live source (to preserve digital RF sync).
 *
 * @param context The audio output handle.
 * @param pcm_data Pointer to the interleaved 16-bit PCM data.
 * @param bytes The number of bytes to write.
 * @param mode The current pipeline mode (File vs. Live SDR).
 * @return The number of bytes successfully written.
 */
size_t audio_output_write(AudioOutputContext* context, const void* pcm_data, size_t bytes, PipelineMode mode);

/**
 * @brief Blocks until the internal audio buffer drains (useful at End-of-Stream).
 */
void audio_output_drain(AudioOutputContext* context);

/**
 * @brief Clears the audio buffer immediately without draining.
 */
void audio_output_clear(AudioOutputContext* context);

/**
 * @brief Stops the audio driver and frees internal buffers.
 */
void audio_output_destroy(AudioOutputContext* context);

#endif // AUDIO_OUTPUT_FUNCTIONS_H_
