#include "file_write_buffer.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdbool.h>
#include "log.h"

// The full definition of the opaque FileWriteBuffer struct from the header file.
struct FileWriteBuffer {
    unsigned char* buffer;
    size_t capacity;
    
    // C99 version: Use standard variables. All access must be protected by the mutex.
    size_t write_pos;
    size_t read_pos;
    
    pthread_mutex_t mutex;
    pthread_cond_t data_available_cond;
    
    bool end_of_stream;
    bool shutting_down;
};

FileWriteBuffer* file_write_buffer_create(size_t capacity) {
    FileWriteBuffer* iob = (FileWriteBuffer*)malloc(sizeof(FileWriteBuffer));
    if (!iob) {
        log_fatal("Failed to allocate memory for FileWriteBuffer struct.");
        return NULL;
    }

    iob->buffer = (unsigned char*)malloc(capacity);
    if (!iob->buffer) {
        log_fatal("Failed to allocate memory for FileWriteBuffer data buffer of size %zu bytes.", capacity);
        free(iob);
        return NULL;
    }

    iob->capacity = capacity;
    
    // Initialize standard variables.
    iob->write_pos = 0;
    iob->read_pos = 0;
    
    pthread_mutex_init(&iob->mutex, NULL);
    pthread_cond_init(&iob->data_available_cond, NULL);
    
    iob->end_of_stream = false;
    iob->shutting_down = false;

    log_debug("I/O buffer created with %zu bytes capacity.", capacity);
    return iob;
}

void file_write_buffer_destroy(FileWriteBuffer* iob) {
    if (!iob) return;
    
    pthread_mutex_destroy(&iob->mutex);
    pthread_cond_destroy(&iob->data_available_cond);
    free(iob->buffer);
    free(iob);
}

size_t file_write_buffer_write(FileWriteBuffer* iob, const void* data, size_t bytes) {
    if (!iob || !data || bytes == 0) return 0;

    // The entire write operation is now protected by a single lock.
    pthread_mutex_lock(&iob->mutex);

    size_t available_space;
    if (iob->write_pos >= iob->read_pos) {
        available_space = iob->capacity - (iob->write_pos - iob->read_pos) - 1;
    } else {
        available_space = (iob->read_pos - iob->write_pos) - 1;
    }

    size_t bytes_to_write = (bytes > available_space) ? available_space : bytes;
    if (bytes_to_write > 0) {
        size_t first_chunk_size = (iob->write_pos + bytes_to_write > iob->capacity) ? (iob->capacity - iob->write_pos) : bytes_to_write;
        memcpy(iob->buffer + iob->write_pos, data, first_chunk_size);

        size_t second_chunk_size = bytes_to_write - first_chunk_size;
        if (second_chunk_size > 0) {
            memcpy(iob->buffer, (const unsigned char*)data + first_chunk_size, second_chunk_size);
        }

        iob->write_pos = (iob->write_pos + bytes_to_write) % iob->capacity;
        
        // Signal the consumer thread that new data is available.
        pthread_cond_signal(&iob->data_available_cond);
    }

    pthread_mutex_unlock(&iob->mutex);

    return bytes_to_write;
}

size_t file_write_buffer_read(FileWriteBuffer* iob, void* buffer, size_t max_bytes) {
    if (!iob || !buffer || max_bytes == 0) return 0;

    // The entire read operation is now protected by a single lock.
    pthread_mutex_lock(&iob->mutex);

    size_t available_data;

    while (true) {
        available_data = (iob->write_pos >= iob->read_pos) ? (iob->write_pos - iob->read_pos) : (iob->capacity - (iob->read_pos - iob->write_pos));

        if (available_data > 0 || iob->shutting_down || iob->end_of_stream) {
            break;
        }
        
        pthread_cond_wait(&iob->data_available_cond, &iob->mutex);
    }

    if (iob->shutting_down) {
        pthread_mutex_unlock(&iob->mutex);
        return 0;
    }

    if (available_data == 0 && iob->end_of_stream) {
        pthread_mutex_unlock(&iob->mutex);
        return 0;
    }
    
    size_t bytes_to_read = (max_bytes > available_data) ? available_data : max_bytes;

    if (bytes_to_read > 0) {
        size_t first_chunk_size = (iob->read_pos + bytes_to_read > iob->capacity) ? (iob->capacity - iob->read_pos) : bytes_to_read;
        memcpy(buffer, iob->buffer + iob->read_pos, first_chunk_size);

        size_t second_chunk_size = bytes_to_read - first_chunk_size;
        if (second_chunk_size > 0) {
            memcpy((unsigned char*)buffer + first_chunk_size, iob->buffer, second_chunk_size);
        }

        iob->read_pos = (iob->read_pos + bytes_to_read) % iob->capacity;
    }

    pthread_mutex_unlock(&iob->mutex);

    return bytes_to_read;
}

void file_write_buffer_signal_end_of_stream(FileWriteBuffer* iob) {
    if (!iob) return;
    pthread_mutex_lock(&iob->mutex);
    iob->end_of_stream = true;
    pthread_cond_signal(&iob->data_available_cond);
    pthread_mutex_unlock(&iob->mutex);
}

void file_write_buffer_signal_shutdown(FileWriteBuffer* iob) {
    if (!iob) return;
    pthread_mutex_lock(&iob->mutex);
    iob->shutting_down = true;
    pthread_cond_signal(&iob->data_available_cond);
    pthread_mutex_unlock(&iob->mutex);
}

size_t file_write_buffer_get_size(FileWriteBuffer* iob) {
    if (!iob) return 0;

    pthread_mutex_lock(&iob->mutex);
    size_t available_data = (iob->write_pos >= iob->read_pos) 
                          ? (iob->write_pos - iob->read_pos) 
                          : (iob->capacity - (iob->read_pos - iob->write_pos));
    pthread_mutex_unlock(&iob->mutex);
    
    return available_data;
}

size_t file_write_buffer_get_capacity(FileWriteBuffer* iob) {
    if (!iob) return 0;
    // Capacity is immutable, so a mutex isn't strictly necessary,
    // but it's good practice for consistency.
    return iob->capacity;
}
