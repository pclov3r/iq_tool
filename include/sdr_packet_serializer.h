/**
 * @file sdr_packet_serializer.h
 * @brief Defines the data protocol for the ring buffer between SDR Capture and Reader threads.
 *
 * This module provides a standardized binary protocol for transmitting I/Q data
 * from hardware drivers (Producers) to the processing pipeline (Consumer) via
 * a lock-free Ring Buffer.
 *
 * Key Features:
 * 1. **Atomic Writes:** Packets are either written entirely or dropped to preserve stream integrity.
 * 2. **Alignment:** The 16-byte header ensures the payload is aligned for SIMD operations.
 * 3. **Stateful Reading:** The reader can "sip" small chunks of data from a large
 *    packet in the buffer, decoupling the hardware transfer size from the DSP block size.
 * 4. **Self-Healing:** Uses a Magic Number to resynchronize if the stream is corrupted.
 *
 * Data Invariant:
 * All data payloads written to this stream MUST be Interleaved (I, Q, I, Q...).
 * Any de-interleaving required by hardware (e.g. SDRplay) must occur before
 * writing to this buffer.
 */

#ifndef SDR_PACKET_SERIALIZER_H_
#define SDR_PACKET_SERIALIZER_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "common_types.h"

// --- Forward Declarations ---
struct RingBuffer;
struct SampleChunk;

// --- Protocol Constants ---

/**
 * @brief The synchronization marker ("IQPK") used to identify the start of a packet.
 */
#define IQPK_MAGIC 0x4B505149

/**
 * @brief Flag indicating a stream discontinuity (e.g., buffer overrun).
 *        Payload length is usually 0 when this is set.
 */
#define SDR_CHUNK_FLAG_STREAM_RESET (1 << 0)


// --- Data Structures ---

/**
 * @brief The packet header placed before every data payload in the ring buffer.
 *
 * We use explicit padding to ensure the total size is 32 bytes. This guarantees that
 * the payload immediately following this header starts on a 32-byte aligned boundary
 * (assuming the ring buffer itself is aligned), which is critical for SIMD/AVX performance.
 */
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;        ///< Synchronization marker (IQPK_MAGIC).
    uint32_t num_samples;  ///< The number of I/Q pairs in the following payload.
    uint8_t  flags;        ///< Bitmask of stream status flags (e.g. RESET).
    uint8_t  format_id;    ///< The SampleFormat enum value of the sample data.
    uint8_t  reserved[22];
} SdrInputChunkHeader;
#pragma pack(pop)

/**
 * @brief Tracks the state of the current packet being read by the consumer.
 *
 * Since the Reader thread may request data in smaller chunks than the hardware
 * provides, this struct tracks how much of the current packet in the ring buffer
 * remains to be read.
 */
typedef struct {
    uint32_t samples_remaining_in_packet; ///< How many samples are left in the current ring buffer packet.
    SampleFormat current_packet_format;       ///< The sample format of the current packet.
} SerializerState;


// --- Serialization Functions (Writing to the Stream) ---

/**
 * @brief Writes a packet of INTERLEAVED samples to the ring buffer.
 *
 * This function creates the header and writes it, followed immediately
 * by the raw sample data. The operation is atomic: if the buffer cannot hold
 * both the header and the full payload, nothing is written, and false is returned.
 *
 * @param buffer The target ring buffer.
 * @param num_samples The number of I/Q pairs to write.
 * @param sample_data Pointer to the interleaved data ([I, Q, I, Q...]).
 * @param format The format of the samples (e.g., CU8, CS16).
 * @return true if written successfully, false if dropped due to lack of space.
 */
bool sdr_packet_serializer_write_block(struct RingBuffer* buffer, uint32_t num_samples, const void* sample_data, SampleFormat format);

/**
 * @brief Writes a "Stream Reset" event packet to the buffer.
 *
 * This packet has 0 payload bytes and the SDR_CHUNK_FLAG_STREAM_RESET flag set.
 * It tells the downstream pipeline to clear filters/buffers to avoid smearing glitches.
 *
 * @param buffer The target ring buffer.
 * @return true if written, false if buffer full.
 */
bool sdr_packet_serializer_write_reset_event(struct RingBuffer* buffer);


// --- Deserialization Function (Reading from the Stream) ---

/**
 * @brief Reads data from the ring buffer into a SampleChunk.
 *
 * This function acts as a state machine. It handles reading the Packet Header
 * and sipping the Payload in chunks.
 *
 * @param buffer The source ring buffer.
 * @param target_chunk The SampleChunk to fill with data. The `packet_sample_format` field will be updated.
 * @param state Pointer to the persistent state tracker for this stream.
 * @param[out] is_reset_event Set to true if a Reset Event packet was encountered.
 * @param request_size_samples The maximum number of samples to read into the chunk.
 *
 * @return The actual number of samples read (may be less than requested if the
 *         packet ends), 0 if end-of-stream or reset event, or -1 on fatal error.
 */
int64_t sdr_packet_serializer_read_packet(struct RingBuffer* buffer,
                                          struct SampleChunk* target_chunk,
                                          SerializerState* state,
                                          bool* is_reset_event,
                                          size_t request_size_samples);

#endif // SDR_PACKET_SERIALIZER_H_
