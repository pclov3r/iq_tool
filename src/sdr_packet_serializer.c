#include "sdr_packet_serializer.h"
#include "constants.h"
#include "log.h"
#include "app_context.h"
#include "pipeline_types.h"
#include "ring_buffer.h"
#include "sample_convert.h"
#include <string.h>
#include <stdlib.h>

// --- INTERNAL HELPER ---
static bool _has_space_for(RingBuffer* buffer, size_t bytes_needed) {
    size_t cap = ring_buffer_get_capacity(buffer);
    size_t size = ring_buffer_get_size(buffer);
    // Ring buffer implementation typically reserves 1 byte to distinguish full from empty
    size_t free_space = (cap > size) ? (cap - size - 1) : 0;
    return (free_space >= bytes_needed);
}

// --- WRITE IMPLEMENTATION ---

bool sdr_packet_serializer_write_block(RingBuffer* buffer, uint32_t num_samples, const void* sample_data, format_t format) {
    size_t bytes_per_sample = get_bytes_per_sample(format);
    size_t data_size = num_samples * bytes_per_sample;
    size_t total_needed = sizeof(SdrInputChunkHeader) + data_size;

    // CRITICAL FIX: Atomic Check. If we can't write EVERYTHING, we write NOTHING.
    if (!_has_space_for(buffer, total_needed)) {
        return false;
    }

    SdrInputChunkHeader header;
    header.magic = IQPK_MAGIC;
    header.num_samples = num_samples;
    header.flags = 0; // Data is implicitly interleaved now
    header.format_id = (uint8_t)format;

    // Ensure padding bytes are zeroed for deterministic behavior and future compatibility
    memset(header.reserved, 0, sizeof(header.reserved));

    ring_buffer_write(buffer, &header, sizeof(header));
    ring_buffer_write(buffer, sample_data, data_size);

    return true;
}

bool sdr_packet_serializer_write_reset_event(RingBuffer* buffer) {
    if (!_has_space_for(buffer, sizeof(SdrInputChunkHeader))) return false;

    SdrInputChunkHeader header;
    header.magic = IQPK_MAGIC;
    header.num_samples = 0;
    header.flags = SDR_CHUNK_FLAG_STREAM_RESET;
    header.format_id = (uint8_t)FORMAT_UNKNOWN;

    // Ensure padding bytes are zeroed
    memset(header.reserved, 0, sizeof(header.reserved));

    ring_buffer_write(buffer, &header, sizeof(header));
    return true;
}

// --- READ IMPLEMENTATION ---

int64_t sdr_packet_serializer_read_packet(RingBuffer* buffer,
                                          SampleChunk* target_chunk,
                                          SerializerState* state,
                                          bool* is_reset_event,
                                          size_t request_size_samples) 
{
    *is_reset_event = false;

    // 1. Fetch Header if needed
    if (state->samples_remaining_in_packet == 0) {
        uint32_t current_word = 0;

        // Sync Loop: Find the Magic Number
        while (true) {
            size_t bytes_read = ring_buffer_read(buffer, &current_word, sizeof(uint32_t));
            if (bytes_read == 0) return 0; // EOS
            if (bytes_read < sizeof(uint32_t)) return -1; // Error

            if (current_word == IQPK_MAGIC) break; // Found it

            // Shift 1 byte and retry (sliding window)
            unsigned char byte;
            current_word = (current_word >> 8);
            if (ring_buffer_read(buffer, &byte, 1) < 1) return 0;
            current_word |= ((uint32_t)byte << 24);
        }

        // Read rest of header
        // Note: sizeof(SdrInputChunkHeader) now includes the 16-byte padding defined in the header.
        SdrInputChunkHeader header;
        header.magic = current_word;
        size_t rest_size = sizeof(SdrInputChunkHeader) - sizeof(uint32_t);

        if (ring_buffer_read(buffer, ((char*)&header) + sizeof(uint32_t), rest_size) < rest_size) return -1;

        if (header.flags & SDR_CHUNK_FLAG_STREAM_RESET) {
            *is_reset_event = true;
            return 0;
        }

        state->samples_remaining_in_packet = header.num_samples;
        state->current_packet_format = (format_t)header.format_id;
    }

    // 2. Read Payload
    size_t samples_to_read = request_size_samples;
    if (samples_to_read > state->samples_remaining_in_packet) {
        samples_to_read = state->samples_remaining_in_packet;
    }

    // Validate format
    size_t bpp = get_bytes_per_sample(state->current_packet_format);
    if (bpp == 0) return -1;

    // Validate capacity
    size_t capacity_samples = target_chunk->raw_input_capacity_bytes / bpp;
    if (samples_to_read > capacity_samples) samples_to_read = capacity_samples;

    if (samples_to_read == 0) return 0;

    // Read
    size_t bytes_to_read = samples_to_read * bpp;
    if (ring_buffer_read(buffer, target_chunk->raw_input_data, bytes_to_read) < bytes_to_read) return -1;

    state->samples_remaining_in_packet -= samples_to_read;
    target_chunk->packet_sample_format = state->current_packet_format;
    target_chunk->input_bytes_per_sample_pair = bpp;

    return (int64_t)samples_to_read;
}
