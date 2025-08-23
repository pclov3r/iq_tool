#include "sdr_packet_serializer.h"
#include "constants.h" // <-- MODIFIED
#include "log.h"
#include "types.h"
#include <string.h>
#include <stdlib.h>

// --- Private Packet Definition ---
#pragma pack(push, 1)
typedef struct {
    uint32_t num_samples;
    uint8_t  flags;
} SdrInputChunkHeader;
#pragma pack(pop)

#define SDR_CHUNK_FLAG_INTERLEAVED  (1 << 0)
#define SDR_CHUNK_FLAG_STREAM_RESET (1 << 1)

// --- Serialization Functions ---

bool sdr_packet_serializer_write_deinterleaved_chunk(FileWriteBuffer* buffer, uint32_t num_samples, const short* i_data, const short* q_data) {
    SdrInputChunkHeader header;
    header.num_samples = num_samples;
    header.flags = 0; // De-interleaved

    size_t bytes_per_plane = num_samples * sizeof(short);
    
    size_t header_written = file_write_buffer_write(buffer, &header, sizeof(header));
    if (header_written < sizeof(header)) return false;

    size_t i_written = file_write_buffer_write(buffer, i_data, bytes_per_plane);
    if (i_written < bytes_per_plane) return false;

    size_t q_written = file_write_buffer_write(buffer, q_data, bytes_per_plane);
    if (q_written < bytes_per_plane) return false;

    return true;
}

bool sdr_packet_serializer_write_interleaved_chunk(FileWriteBuffer* buffer, uint32_t num_samples, const void* sample_data, size_t bytes_per_sample_pair) {
    SdrInputChunkHeader header;
    header.num_samples = num_samples;
    header.flags = SDR_CHUNK_FLAG_INTERLEAVED;

    size_t data_bytes = num_samples * bytes_per_sample_pair;

    size_t header_written = file_write_buffer_write(buffer, &header, sizeof(header));
    if (header_written < sizeof(header)) return false;

    size_t data_written = file_write_buffer_write(buffer, sample_data, data_bytes);
    if (data_written < data_bytes) return false;

    return true;
}

bool sdr_packet_serializer_write_reset_event(FileWriteBuffer* buffer) {
    SdrInputChunkHeader header;
    header.num_samples = 0;
    header.flags = SDR_CHUNK_FLAG_STREAM_RESET;

    size_t written = file_write_buffer_write(buffer, &header, sizeof(header));
    return (written == sizeof(header));
}

// --- Deserialization Function ---

// --- MODIFIED: Signature updated and malloc removed ---
int64_t sdr_packet_serializer_read_packet(FileWriteBuffer* buffer,
                                          SampleChunk* target_chunk,
                                          bool* is_reset_event,
                                          void* temp_buffer,
                                          size_t temp_buffer_size) {
    *is_reset_event = false;
    SdrInputChunkHeader header;

    // 1. Read the header.
    size_t header_bytes_read = file_write_buffer_read(buffer, &header, sizeof(header));
    if (header_bytes_read == 0) {
        return 0; // Normal end of stream
    }
    if (header_bytes_read < sizeof(header)) {
        log_error("Incomplete header read from SDR buffer. Stream corrupted.");
        return -1; // Fatal error
    }

    // 2. Check for special event packets.
    if (header.num_samples == 0) {
        if (header.flags & SDR_CHUNK_FLAG_STREAM_RESET) {
            *is_reset_event = true;
        }
        return 0; // 0 frames, but check is_reset_event
    }

    uint32_t samples_in_chunk = header.num_samples;
    if (samples_in_chunk > PIPELINE_CHUNK_BASE_SAMPLES) {
        log_warn("SDR chunk (%u samples) exceeds buffer capacity (%d). Truncating.",
                 samples_in_chunk, PIPELINE_CHUNK_BASE_SAMPLES);
        samples_in_chunk = PIPELINE_CHUNK_BASE_SAMPLES;
    }

    // 3. Read the sample data based on the header flags.
    if (header.flags & SDR_CHUNK_FLAG_INTERLEAVED) {
        // Case for RTL-SDR, HackRF, etc.
        size_t bytes_to_read = samples_in_chunk * target_chunk->input_bytes_per_sample_pair;
        size_t data_bytes_read = file_write_buffer_read(buffer, target_chunk->raw_input_data, bytes_to_read);
        if (data_bytes_read < bytes_to_read) {
            log_error("Incomplete data read for interleaved chunk. Stream corrupted.");
            return -1;
        }
    } else {
        // Case for SDRplay (de-interleaved cs16).
        size_t bytes_per_plane = samples_in_chunk * sizeof(short);
        
        // Sanity check the provided temporary buffer
        if (bytes_per_plane > temp_buffer_size) {
            log_fatal("SDR deserializer buffer is too small for this packet. Required: %zu, Available: %zu.",
                      bytes_per_plane, temp_buffer_size);
            return -1;
        }

        int16_t* raw_output = (int16_t*)target_chunk->raw_input_data;
        
        // --- MODIFIED: Use the pre-allocated temp_buffer instead of malloc ---
        short* temp_i = (short*)temp_buffer;
        
        size_t i_bytes_read = file_write_buffer_read(buffer, temp_i, bytes_per_plane);
        if (i_bytes_read < bytes_per_plane) {
            log_error("Incomplete I-plane read for de-interleaved chunk. Stream corrupted.");
            return -1;
        }

        // We can reuse the same buffer for the Q plane
        short* temp_q = (short*)temp_buffer;
        size_t q_bytes_read = file_write_buffer_read(buffer, temp_q, bytes_per_plane);
        if (q_bytes_read < bytes_per_plane) {
            log_error("Incomplete Q-plane read for de-interleaved chunk. Stream corrupted.");
            return -1;
        }

        // Re-interleave the data into the final destination
        for (uint32_t i = 0; i < samples_in_chunk; i++) {
            raw_output[i * 2]     = temp_i[i];
            raw_output[i * 2 + 1] = temp_q[i];
        }
        // --- MODIFIED: No free() is needed ---
    }

    return samples_in_chunk;
}
