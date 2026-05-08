/**
 * @file audio_output_functions.c
 * @brief Implements the unified audio output using Miniaudio and a lock-free RingBuffer.
 */

#include "audio_output_functions.h"
#include "miniaudio.h"
#include "ring_buffer.h"
#include "log.h"
#include "utils.h"
#include "mem_arena.h"
#include "signal_handler.h"
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

struct AudioOutputContext {
    ma_device audio_device;
    RingBuffer* audio_ring_buffer;
    bool audio_device_initialized;
    size_t buffer_size;
    int channels;
};

// --- Miniaudio Callback ---
static void miniaudio_data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    (void)pInput;
    AudioOutputContext* ctx = (AudioOutputContext*)pDevice->pUserData;
    if (frameCount == 0) return;

    size_t bytes_needed = frameCount * ctx->channels * sizeof(int16_t);
    size_t available = ring_buffer_get_size(ctx->audio_ring_buffer);

    if (available < bytes_needed) {
        // Underrun: Play whatever is left, pad the rest with silence.
        if (available > 0) ring_buffer_read(ctx->audio_ring_buffer, pOutput, available);
        memset((uint8_t*)pOutput + available, 0, bytes_needed - available);
        return;
    }
    
    // Normal: Pull the exact requested amount of data.
    ring_buffer_read(ctx->audio_ring_buffer, pOutput, bytes_needed);
}

// --- Public API ---

AudioOutputContext* audio_output_create(struct MemoryArena* arena, int sample_rate, int channels, size_t buffer_size_bytes) {
    AudioOutputContext* ctx = (AudioOutputContext*)mem_arena_alloc(arena, sizeof(AudioOutputContext), true);
    if (!ctx) return NULL;

    ctx->buffer_size = buffer_size_bytes;
    ctx->channels = channels;

    // 1. Setup Audio Ring Buffer
    ctx->audio_ring_buffer = ring_buffer_create(buffer_size_bytes);
    if (!ctx->audio_ring_buffer) {
        log_fatal("AudioOutput: Failed to create ring buffer.");
        return NULL;
    }

    double bytes_per_sec = (double)sample_rate * channels * sizeof(int16_t);
    double duration = (double)buffer_size_bytes / bytes_per_sec;
    log_info("AudioOutput: Ring Buffer created: %zu bytes (%.2f seconds)", buffer_size_bytes, duration);

    // 2. Setup Miniaudio
    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format   = ma_format_s16; // Mandate 16-bit PCM
    deviceConfig.playback.channels = channels;
    deviceConfig.sampleRate        = sample_rate;
    deviceConfig.dataCallback      = miniaudio_data_callback;
    deviceConfig.pUserData         = ctx;

    if (ma_device_init(NULL, &deviceConfig, &ctx->audio_device) != MA_SUCCESS) {
        log_fatal("AudioOutput: Failed to initialize OS audio device.");
        ring_buffer_destroy(ctx->audio_ring_buffer);
        return NULL;
    }
    ctx->audio_device_initialized = true;

    // 3. Start Audio Hardware
    if (ma_device_start(&ctx->audio_device) != MA_SUCCESS) {
        log_fatal("AudioOutput: Failed to start OS audio device.");
        ma_device_uninit(&ctx->audio_device);
        ring_buffer_destroy(ctx->audio_ring_buffer);
        return NULL;
    }

    return ctx;
}

size_t audio_output_write(AudioOutputContext* ctx, const void* pcm_data, size_t bytes, PipelineMode mode) {
    if (!ctx || !ctx->audio_ring_buffer || bytes == 0) return 0;

    // --- CONDITIONAL BACKPRESSURE ---
    if (mode == PIPELINE_MODE_FILE_PROCESSING) {
        const size_t THROTTLE_THRESHOLD = (size_t)(ctx->buffer_size * 0.8);
        ring_buffer_wait_for_threshold(ctx->audio_ring_buffer, THROTTLE_THRESHOLD);
        if (is_shutdown_requested()) return 0;
    }

    ring_buffer_write(ctx->audio_ring_buffer, pcm_data, bytes);
    return bytes;
}

void audio_output_flush(AudioOutputContext* ctx) {
    if (!ctx || !ctx->audio_ring_buffer) return;

    // --- Audio Drain Parameters ---
    const int poll_interval_ms = 10;
    const int stall_timeout_ms = 200;
    const int hardware_padding_ms = 200;

    size_t last_size = (size_t)-1;
    int stall_count = 0;
    int max_stall_iterations = stall_timeout_ms / poll_interval_ms;
    if (max_stall_iterations < 1) max_stall_iterations = 1;

    while (true) {
        size_t curr_size = ring_buffer_get_size(ctx->audio_ring_buffer);

        if (curr_size == 0) break; // Success
        if (is_shutdown_requested()) break; // Global Abort

        // Stall Detection
        if (curr_size == last_size) {
            stall_count++;
            if (stall_count > max_stall_iterations) break;
        } else {
            stall_count = 0;
            last_size = curr_size;
        }

        #ifdef _WIN32
        Sleep(poll_interval_ms);
        #else
        usleep(poll_interval_ms * 1000);
        #endif
    }

    // Hardware Padding
    if (hardware_padding_ms > 0) {
        #ifdef _WIN32
        Sleep(hardware_padding_ms);
        #else
        usleep(hardware_padding_ms * 1000);
        #endif
    }
}

void audio_output_destroy(AudioOutputContext* ctx) {
    if (!ctx) return;
    if (ctx->audio_device_initialized) {
        ma_device_uninit(&ctx->audio_device);
        ctx->audio_device_initialized = false;
    }
    if (ctx->audio_ring_buffer) {
        ring_buffer_destroy(ctx->audio_ring_buffer);
        ctx->audio_ring_buffer = NULL;
    }
}
