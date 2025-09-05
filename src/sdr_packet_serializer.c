#include "sdr_packet_serializer.h"
#include "constants.h"
#include "log.h"
#include "app_context.h"
#include "pipeline_types.h"
#include "file_write_buffer.h"
#include <string.h>
#include <stdlib.h>

// Magic number "IQPK" (I=0x49, Q=0x51, P=0x50, K=0x4B) as a little-endian 32-bit integer.
// Used to synchronize the reader to the start of a valid packet.
#define IQPK_MAGIC 0x4B505149

// --- PRIVATE DEFINITIONS ---
#pragma pack(push, 1)
typedef struct {
    // --- MODIFIED ---
    uint32_t magic; // MUST be the first field
    uint32_t num_samples;
    uint8_t  flags;
    uint8_t  format_id;
} SdrInputChunkHeader;
#pragma pack(pop)

#define SDR_CHUNK_FLAG_INTERLEAVED  (1 << 0)
#define SDR_CHUNK_FLAG_STREAM_RESET (1 << 1)
// --- END OF PRIVATE DEFINITIONS ---


// --- PRIVATE HELPER FUNCTION DECLARATIONS (PROTOTYPES) ---
static int64_t _read_interleaved_payload(FileWriteBuffer* buffer, SampleChunk* target_chunk, uint32_t num_samples);
static int64_t _read_and_reinterleave_payload(FileWriteBuffer* buffer, SampleChunk* target_chunk, uint32_t num_samples, void* temp_buffer, size_t temp_buffer_size);

/**
 * @brief Checks if a raw format ID from a packet header is a valid, defined member of the format_t enum.
 * @param format_id The integer ID to check.
 * @return true if the ID is a valid enum member, false otherwise.
 */
static bool _is_packet_format_id_valid(uint8_t format_id) {
    switch ((format_t)format_id) {
        case U8:
        case S8:
        case U16:
        case S16:
        case U32:
        case S32:
        case F32:
        case CU8:
        case CS8:
        case CU16:
        case CS16:
        case CU32:
        case CS32:
        case CF32:
        case SC16Q11:
        case FORMAT_UNKNOWN: // FORMAT_UNKNOWN is valid for non-data events (e.g., reset)
            return true;
        default:
            return false;
    }
}


// --- PUBLIC API: SERIALIZATION FUNCTIONS ---

bool sdr_packet_serializer_write_deinterleaved_chunk(FileWriteBuffer* buffer, uint32_t num_samples, const short* i_data, const short* q_data, format_t format) {
    SdrInputChunkHeader header;
    header.magic = IQPK_MAGIC; // --- MODIFIED ---
    header.num_samples = num_samples;
    header.flags = 0; // De-interleaved (interleaved flag is NOT set)
    header.format_id = (uint8_t)format;

    size_t bytes_per_plane = num_samples * sizeof(short);
    
    if (file_write_buffer_write(buffer, &header, sizeof(header)) < sizeof(header)) return false;
    if (file_write_buffer_write(buffer, i_data, bytes_per_plane) < bytes_per_plane) return false;
    if (file_write_buffer_write(buffer, q_data, bytes_per_plane) < bytes_per_plane) return false;

    return true;
}

bool sdr_packet_serializer_write_interleaved_chunk(FileWriteBuffer* buffer, uint32_t num_samples, const void* sample_data, size_t bytes_per_sample_pair, format_t format) {
    SdrInputChunkHeader header;
    header.magic = IQPK_MAGIC; // --- MODIFIED ---
    header.num_samples = num_samples;
    header.flags = SDR_CHUNK_FLAG_INTERLEAVED;
    header.format_id = (uint8_t)format;

    size_t data_bytes = num_samples * bytes_per_sample_pair;

    if (file_write_buffer_write(buffer, &header, sizeof(header)) < sizeof(header)) return false;
    if (file_write_buffer_write(buffer, sample_data, data_bytes) < data_bytes) return false;

    return true;
}

bool sdr_packet_serializer_write_reset_event(FileWriteBuffer* buffer) {
    SdrInputChunkHeader header;
    header.magic = IQPK_MAGIC; // --- MODIFIED ---
    header.num_samples = 0;
    header.flags = SDR_CHUNK_FLAG_STREAM_RESET;
    header.format_id = (uint8_t)FORMAT_UNKNOWN;

    return (file_write_buffer_write(buffer, &header, sizeof(header)) == sizeof(header));
}


// --- PUBLIC API: REFACTORED DESERIALIZATION FUNCTION ---

// --- NEW: This function has been completely replaced with the robust, re-synchronizing version. ---
int64_t sdr_packet_serializer_read_packet(FileWriteBuffer* buffer,
                                          SampleChunk* target_chunk,
                                          bool* is_reset_event,
                                          void* temp_buffer,
                                          size_t temp_buffer_size) {
    *is_reset_event = false;
    SdrInputChunkHeader header;
    uint32_t current_word = 0;
    bool resync_in_progress = false;
    uint64_t discarded_bytes = 0;
    unsigned char single_byte;

    // This loop is the core of the re-synchronization logic.
    while (true) {
        // Step 1: Read exactly 4 bytes to check for the magic number.
        size_t bytes_read = file_write_buffer_read(buffer, &current_word, sizeof(uint32_t));

        // Handle end-of-stream conditions
        if (bytes_read == 0) {
            if (resync_in_progress) {
                log_warn("Stream ended during re-sync after discarding %llu bytes.", discarded_bytes);
            }
            return 0; // Clean end of stream
        }
        if (bytes_read < sizeof(uint32_t)) {
            log_error("SDR stream corrupted at the very end. Incomplete magic number read.");
            return -1; // Fatal error
        }

        // Step 2: Check if we found the magic number.
        if (current_word == IQPK_MAGIC) {
            if (resync_in_progress) {
                log_info("Stream re-synchronized successfully after discarding %llu bytes.", discarded_bytes);
            }
            break; // Success! Exit the re-sync loop and proceed.
        }

        // Step 3: If no match, start re-sync mode and discard one byte at a time.
        if (!resync_in_progress) {
            log_warn("SDR stream de-synchronized! Scanning for next valid packet...");
            resync_in_progress = true;
        }

        // To discard a single byte, we shift our current word and read one new byte.
        current_word = (current_word >> 8); // Discard the oldest byte (from the beginning of the 4-byte read)
        if (file_write_buffer_read(buffer, &single_byte, 1) < 1) {
             log_warn("Stream ended during re-sync after discarding %llu bytes.", discarded_bytes);
             return 0; // End of stream
        }
        current_word |= ((uint32_t)single_byte << 24); // Add the new byte at the end (high bits)
        discarded_bytes++;
    }

    // At this point, we have successfully read and validated the magic number.
    header.magic = current_word;

    // Now read the rest of the header (num_samples, flags, format_id).
    size_t rest_of_header_size = sizeof(SdrInputChunkHeader) - sizeof(uint32_t);
    size_t header_bytes_read = file_write_buffer_read(buffer, ((char*)&header) + sizeof(uint32_t), rest_of_header_size);
    
    if (header_bytes_read < rest_of_header_size) {
        log_error("SDR stream corrupted: Found magic number but header was incomplete.");
        return -1; // Fatal error
    }

    // --- The rest of the function is the original validation and payload reading logic ---
    
    if (!_is_packet_format_id_valid(header.format_id)) {
        log_error("SDR stream corrupted: received invalid sample format ID (%u).", header.format_id);
        return -1;
    }
    
    target_chunk->packet_sample_format = (format_t)header.format_id;
    if (header.flags & SDR_CHUNK_FLAG_STREAM_RESET) {
        *is_reset_event = true;
    }
    if (header.num_samples > 0 && target_chunk->packet_sample_format == FORMAT_UNKNOWN) {
        log_error("SDR stream corrupted: received data packet with FORMAT_UNKNOWN.");
        return -1;
    }
    if (header.num_samples > (PIPELINE_CHUNK_BASE_SAMPLES * 2)) {
        log_error("SDR stream corrupted: received impossibly large packet length (%u).", header.num_samples);
        return -1;
    }
    if (header.num_samples == 0) {
        return 0; // This is a non-data event packet (like a reset).
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
void sdr_write_interleaved_chunks(AppResources* resources, const unsigned char* data, uint32_t length_bytes, size_t bytes_per_sample_pair, format_t format) {
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
                bytes_per_sample_pair,
                format))
        {
            log_warn("SDR input buffer overrun! Dropped %u samples.", total_samples_in_transfer - samples_processed);
            break;
        }

        samples_processed += samples_this_chunk;
        current_buffer_pos += (samples_this_chunk * bytes_per_sample_pair);
    }
}
