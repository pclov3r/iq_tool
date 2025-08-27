#include "sdr_packet_serializer.h"
#include "constants.h"
#include "log.h"
#include "app_context.h"       // Provides AppResources
#include "pipeline_types.h"    // Provides SampleChunk
#include "file_write_buffer.h" // Provides FileWriteBuffer
#include <string.h>
#include <stdlib.h>

// --- PRIVATE DEFINITIONS ---
#pragma pack(push, 1)
typedef struct {
    uint32_t num_samples;
    uint8_t  flags;
} SdrInputChunkHeader;
#pragma pack(pop)

#define SDR_CHUNK_FLAG_INTERLEAVED  (1 << 0)
#define SDR_CHUNK_FLAG_STREAM_RESET (1 << 1)
// --- END OF PRIVATE DEFINITIONS ---


// --- PRIVATE HELPER FUNCTION DECLARATIONS (PROTOTYPES) ---
static int64_t _read_interleaved_payload(FileWriteBuffer* buffer, SampleChunk* target_chunk, uint32_t num_samples);
static int64_t _read_and_reinterleave_payload(FileWriteBuffer* buffer, SampleChunk* target_chunk, uint32_t num_samples, void* temp_buffer, size_t temp_buffer_size);


// --- PUBLIC API: SERIALIZATION FUNCTIONS ---

bool sdr_packet_serializer_write_deinterleaved_chunk(FileWriteBuffer* buffer, uint32_t num_samples, const short* i_data, const short* q_data) {
    SdrInputChunkHeader header;
    header.num_samples = num_samples;
    header.flags = 0; // De-interleaved (interleaved flag is NOT set)

    size_t bytes_per_plane = num_samples * sizeof(short);
    
    if (file_write_buffer_write(buffer, &header, sizeof(header)) < sizeof(header)) return false;
    if (file_write_buffer_write(buffer, i_data, bytes_per_plane) < bytes_per_plane) return false;
    if (file_write_buffer_write(buffer, q_data, bytes_per_plane) < bytes_per_plane) return false;

    return true;
}

bool sdr_packet_serializer_write_interleaved_chunk(FileWriteBuffer* buffer, uint32_t num_samples, const void* sample_data, size_t bytes_per_sample_pair) {
    SdrInputChunkHeader header;
    header.num_samples = num_samples;
    header.flags = SDR_CHUNK_FLAG_INTERLEAVED;

    size_t data_bytes = num_samples * bytes_per_sample_pair;

    if (file_write_buffer_write(buffer, &header, sizeof(header)) < sizeof(header)) return false;
    if (file_write_buffer_write(buffer, sample_data, data_bytes) < data_bytes) return false;

    return true;
}

bool sdr_packet_serializer_write_reset_event(FileWriteBuffer* buffer) {
    SdrInputChunkHeader header;
    header.num_samples = 0;
    header.flags = SDR_CHUNK_FLAG_STREAM_RESET;

    return (file_write_buffer_write(buffer, &header, sizeof(header)) == sizeof(header));
}


// --- PUBLIC API: REFACTORED DESERIALIZATION FUNCTION ---

int64_t sdr_packet_serializer_read_packet(FileWriteBuffer* buffer,
                                          SampleChunk* target_chunk,
                                          bool* is_reset_event,
                                          void* temp_buffer,
                                          size_t temp_buffer_size) {
    *is_reset_event = false;
    SdrInputChunkHeader header;

    size_t header_bytes_read = file_write_buffer_read(buffer, &header, sizeof(header));
    if (header_bytes_read == 0) {
        return 0; // Normal end of stream
    }
    if (header_bytes_read < sizeof(header)) {
        log_error("Incomplete header read from SDR buffer. Stream corrupted.");
        return -1; // Fatal error
    }

    if (header.flags & SDR_CHUNK_FLAG_STREAM_RESET) {
        *is_reset_event = true;
    }
    if (header.num_samples == 0) {
        return 0; // Return 0 frames, but the caller will check is_reset_event.
    }

    if (header.flags & SDR_CHUNK_FLAG_INTERLEAVED) {
        return _read_interleaved_payload(buffer, target_chunk, header.num_samples);
    } else {
        return _read_and_reinterleave_payload(buffer, target_chunk, header.num_samples, temp_buffer, temp_buffer_size);
    }
}


// --- PRIVATE HELPER FUNCTION IMPLEMENTATIONS ---

static int64_t _read_interleaved_payload(FileWriteBuffer* buffer, SampleChunk* target_chunk, uint32_t num_samples) {
    uint32_t samples_to_read = num_samples;
    if (samples_to_read > PIPELINE_CHUNK_BASE_SAMPLES) {
        log_warn("SDR chunk (%u samples) exceeds buffer capacity (%d). Truncating.",
                 samples_to_read, PIPELINE_CHUNK_BASE_SAMPLES);
        samples_to_read = PIPELINE_CHUNK_BASE_SAMPLES;
    }

    size_t bytes_to_read = samples_to_read * target_chunk->input_bytes_per_sample_pair;
    size_t data_bytes_read = file_write_buffer_read(buffer, target_chunk->raw_input_data, bytes_to_read);

    if (data_bytes_read < bytes_to_read) {
        log_error("Incomplete data read for interleaved chunk. Stream corrupted.");
        return -1;
    }
    
    return samples_to_read;
}

static int64_t _read_and_reinterleave_payload(FileWriteBuffer* buffer, SampleChunk* target_chunk, uint32_t num_samples, void* temp_buffer, size_t temp_buffer_size) {
    uint32_t samples_to_read = num_samples;
    if (samples_to_read > PIPELINE_CHUNK_BASE_SAMPLES) {
        log_warn("SDR chunk (%u samples) exceeds buffer capacity (%d). Truncating.",
                 samples_to_read, PIPELINE_CHUNK_BASE_SAMPLES);
        samples_to_read = PIPELINE_CHUNK_BASE_SAMPLES;
    }

    size_t bytes_per_plane = samples_to_read * sizeof(short);
    if ((bytes_per_plane * 2) > temp_buffer_size) {
        log_fatal("SDR deserializer temp buffer is too small for this packet. Required: %zu, Available: %zu.",
                  (bytes_per_plane * 2), temp_buffer_size);
        return -1;
    }

    int16_t* raw_output = (int16_t*)target_chunk->raw_input_data;
    short* temp_i = (short*)temp_buffer;
    short* temp_q = (short*)((char*)temp_buffer + bytes_per_plane);

    if (file_write_buffer_read(buffer, temp_i, bytes_per_plane) < bytes_per_plane) {
        log_error("Incomplete I-plane read for de-interleaved chunk. Stream corrupted.");
        return -1;
    }
    if (file_write_buffer_read(buffer, temp_q, bytes_per_plane) < bytes_per_plane) {
        log_error("Incomplete Q-plane read for de-interleaved chunk. Stream corrupted.");
        return -1;
    }

    for (uint32_t i = 0; i < samples_to_read; i++) {
        raw_output[i * 2]     = temp_i[i];
        raw_output[i * 2 + 1] = temp_q[i];
    }
    
    return samples_to_read;
}

// --- NEW REUSABLE CHUNKING FUNCTION ---
void sdr_write_interleaved_chunks(AppResources* resources, const unsigned char* data, uint32_t length_bytes, size_t bytes_per_sample_pair) {
    if (length_bytes == 0) {
        return;
    }

    uint32_t total_samples_in_transfer = length_bytes / bytes_per_sample_pair;
    uint32_t samples_processed = 0;
    const unsigned char* current_buffer_pos = data;

    while (samples_processed < total_samples_in_transfer) {
        uint32_t samples_this_chunk = total_samples_in_transfer - samples_processed;
        if (samples_this_chunk > PIPELINE_CHUNK_BASE_SAMPLES) {
            samples_this_chunk = PIPELINE_CHUNK_BASE_SAMPLES;
        }

        if (!sdr_packet_serializer_write_interleaved_chunk(
                resources->sdr_input_buffer,
                samples_this_chunk,
                current_buffer_pos,
                bytes_per_sample_pair))
        {
            log_warn("SDR input buffer overrun! Dropped %u samples.", total_samples_in_transfer - samples_processed);
            break;
        }

        samples_processed += samples_this_chunk;
        current_buffer_pos += (samples_this_chunk * bytes_per_sample_pair);
    }
}
