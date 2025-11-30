#ifndef SDR_PACKET_SERIALIZER_H_
#define SDR_PACKET_SERIALIZER_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "common_types.h"

// Forward Declarations
struct RingBuffer;
struct SampleChunk;

// State for the Reader thread to track where it is in the stream
typedef struct {
    uint32_t samples_remaining_in_packet;
    format_t current_packet_format;
} SerializerState;

// --- WRITER FUNCTIONS ---

// Writes interleaved data (I, Q, I, Q) with a header.
bool sdr_packet_serializer_write_block(struct RingBuffer* buffer, uint32_t num_samples, const void* sample_data, format_t format);

// Writes de-interleaved data (IIII... QQQQ...) with a header.
bool sdr_packet_serializer_write_deinterleaved_chunk(struct RingBuffer* buffer, uint32_t num_samples, const short* i_data, const short* q_data, format_t format);

// Writes a reset marker (e.g., frequency change).
bool sdr_packet_serializer_write_reset_event(struct RingBuffer* buffer);

// --- READER FUNCTION ---

int64_t sdr_packet_serializer_read_packet(struct RingBuffer* buffer,
                                          struct SampleChunk* target_chunk,
                                          SerializerState* state,
                                          bool* is_reset_event,
                                          size_t request_size_samples);

#endif // SDR_PACKET_SERIALIZER_H_
