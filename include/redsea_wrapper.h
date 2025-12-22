/*
 * Redsea C Wrapper Interface
 *
 * This header defines a pure C API to access the Redsea C++ RDS decoder.
 * It is designed to be included by C99 projects.
 */

#ifndef REDSEA_WRAPPER_H
#define REDSEA_WRAPPER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle to the C++ object. 
// The C compiler knows this pointer exists but cannot see inside it.
typedef struct RDSContext_T* RedseaHandle;

// Structure to hold decoded RDS data.
// This is a "Snapshot" of the current station state.
typedef struct {
    // Basic Identification
    uint16_t pi_code;       // Program Identification (e.g. 0x54A1)
    char callsign[5];       // RBDS Callsign (e.g. "KEXP", "WNYC") - 4 chars + null
    char ps_name[9];        // Program Service Name (e.g. "BBC R1  ") - 8 chars + null
    char radiotext[65];     // RadioText (e.g. "Song Title...") - 64 chars + null
    char program_type[17];  // PTY Category (e.g. "Sport") - 16 chars + null
    char pty_name[9];       // NEW: PTY Name (e.g. "Football") - 8 chars + null
    
    // Flags
    bool is_music;          // True = Music, False = Speech
    bool tp;                // Traffic Program (Station carries traffic info)
    bool ta;                // Traffic Announcement (Traffic info is currently broadcasting)
    
    // Decoder Identification (DI) Flags
    bool stereo;            // Station broadcasting in Stereo
    bool compressed;        // Audio is compressed
    bool dynamic;           // PTY is dynamic (changes)
    bool binaural;          // Artificial Head recording

    // Extended Info
    char clock_time[32];    // ISO8601 Time string (if received)
    int alt_freqs[32];      // List of Alternative Frequencies (kHz)
    int alt_freq_count;     // Number of valid AFs in the list
    char country_code[3];   // Country Code (e.g. "US", "DE") - 2 chars + null

    // Validity Check
    bool valid;             // True if PI code is acquired (Decoder is synced)

} RdsState;

/**
 * @brief Initialize a new Redsea decoder instance.
 * 
 * @param sample_rate The sample rate of the MPX input buffer (e.g. 240000.0f)
 * @param is_rbds Set to true for North American RBDS (Callsigns, US PTYs). False for World RDS.
 * @param show_partial Set to true to display PS/RT text even if incomplete/noisy.
 * @return RedseaHandle Opaque pointer to the decoder instance. NULL on failure.
 */
RedseaHandle redsea_init(float sample_rate, bool is_rbds, bool show_partial);

/**
 * @brief Free the Redsea decoder instance.
 * 
 * @param ctx The handle to free. Safe to call with NULL.
 */
void redsea_free(RedseaHandle ctx);

/**
 * @brief Process a chunk of raw MPX audio and retrieve the latest state.
 * 
 * @param ctx The decoder handle.
 * @param mpx_data Pointer to the float array containing raw MPX samples.
 * @param num_samples Number of samples in the buffer.
 * @param out_state Pointer to a struct where the decoded state will be written.
 */
void redsea_process_mpx(RedseaHandle ctx, const float* mpx_data, int num_samples, RdsState* out_state);

#ifdef __cplusplus
}
#endif

#endif // REDSEA_WRAPPER_H
