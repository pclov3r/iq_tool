/**
 * @file sdr_packet_serializer.h
 * @brief Defines the data protocol for the ring buffer between the SDR Capture and Reader threads.
 *
 * This module provides a standardized data protocol for writing different types
 * of SDR data (interleaved, de-interleaved) and events (stream reset) into the
 * single, contiguous byte stream managed by the `sdr_input_buffer` ring buffer.
 *
 * Its primary role is to decouple the SDR hardware callback (running in the
 * `sdr_capture_thread`) from the main processing pipeline. The capture thread
 * uses the `sdr_packet_serializer_write_*` functions to serialize data into
 * framed packets. The `reader_thread` then uses `sdr_packet_serializer_read_packet`
 * to deserialize these packets, ensuring a robust and orderly flow of data and
 * events, even if the SDR hardware provides data in inconvenient chunks or formats.
 */

#ifndef SDR_PACKET_SERIALIZER_H_
#define SDR_PACKET_SERIALIZER_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// --- Forward Declarations ---
// We only use pointers to these structs, so we don't need their full definitions.
struct FileWriteBuffer;
struct SampleChunk;
struct AppResources;

// --- Serialization Functions (Writing to the Stream) ---

/**
 * @brief Writes a packet of DE-INTERLEAVED samples (e.g., from SDRplay) to the buffer.
 * This function serializes the header and data into a single packet in the ring buffer.
 *
 * @param buffer The target ring buffer.
 * @param num_samples The number of I/Q pairs to write.
 * @param i_data A pointer to the buffer of I samples.
 * @param q_data A pointer to the buffer of Q samples.
 * @return true on success, false if the buffer did not have enough space.
 */
bool sdr_packet_serializer_write_deinterleaved_chunk(struct FileWriteBuffer* buffer, uint32_t num_samples, const short* i_data, const short* q_data);

/**
 * @brief Writes a packet of INTERLEAVED samples (e.g., from RTL-SDR) to the buffer.
 * This function serializes the header and data into a single packet in the ring buffer.
 *
 * @param buffer The target ring buffer.
 * @param num_samples The number of I/Q pairs to write.
 * @param sample_data A pointer to the interleaved sample data ([I, Q, I, Q, ...]).
 * @param bytes_per_sample_pair The size in bytes of one I/Q pair (e.g., 2 for cu8, 4 for cs16).
 * @return true on success, false if the buffer did not have enough space.
 */
bool sdr_packet_serializer_write_interleaved_chunk(struct FileWriteBuffer* buffer, uint32_t num_samples, const void* sample_data, size_t bytes_per_sample_pair);

/**
 * @brief Writes a special "stream reset" event packet to the buffer.
 * This packet contains no sample data and is used to signal a discontinuity.
 *
 * @param buffer The target ring buffer.
 * @return true on success, false on failure.
 */
bool sdr_packet_serializer_write_reset_event(struct FileWriteBuffer* buffer);


// --- Deserialization Function (Reading from the Stream) ---

/**
 * @brief Reads and parses the next complete packet from the ring buffer.
 *
 * This is a blocking call. It handles the logic of reading the header, determining
 * the packet type and size, and reading the correct amount of data. It will
 * always return a correctly interleaved block of samples in the target_chunk.
 *
 * @param buffer The source ring buffer.
 * @param target_chunk A pointer to a pre-allocated SampleChunk to be filled.
 * @param[out] is_reset_event A pointer to a boolean that will be set to true if the
 *                            packet was a stream reset event.
 * @param temp_buffer A pre-allocated buffer for de-interleaving.
 * @param temp_buffer_size The size of the temp_buffer in bytes.
 * @return The number of frames read and placed in the target_chunk. Returns 0 for
 *         a normal end-of-stream or a non-data event, and a negative value for a fatal parsing error.
 */
int64_t sdr_packet_serializer_read_packet(struct FileWriteBuffer* buffer,
                                          struct SampleChunk* target_chunk,
                                          bool* is_reset_event,
                                          void* temp_buffer,
                                          size_t temp_buffer_size);

/**
 * @brief A reusable utility to take a large, interleaved buffer from an SDR,
 *        break it into pipeline-sized chunks, and write each as a packet to the ring buffer.
 *
 * @param resources Pointer to the application resources (needed for the buffer handle).
 * @param data Pointer to the start of the large interleaved data block from the SDR.
 * @param length_bytes The total size in bytes of the data block.
 * @param bytes_per_sample_pair The size in bytes of one I/Q pair for this data.
 */
void sdr_write_interleaved_chunks(struct AppResources* resources, const unsigned char* data, uint32_t length_bytes, size_t bytes_per_sample_pair);

#endif // SDR_PACKET_SERIALIZER_H_
