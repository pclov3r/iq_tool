/*
 * Redsea C Wrapper Implementation
 *
 * This file acts as a bridge between the C99 application and the C++17 Redsea library.
 * It manually instantiates Redsea classes and copies data into the C-compatible struct.
 */

#include "redsea_wrapper.h"

// Redsea C++ Headers
// We assume 'external/redsea' is in the include path
#include "src/dsp/subcarrier.hh"
#include "src/channel.hh"
#include "src/station.hh"
#include "src/options.hh"
#include "src/tables.hh"

#include <iostream>
#include <cstring>
#include <memory>
#include <vector>
#include <algorithm>
#include <cctype> // For isspace

// A dummy stream buffer to silence Redsea's internal logging
class NullBuffer : public std::streambuf {
public:
    int overflow(int c) { return c; }
};

// The internal C++ context structure
struct RDSContext_T {
    redsea::Options options;
    std::unique_ptr<redsea::SubcarrierSet> subcarrier;
    std::unique_ptr<redsea::Channel> channel;

    // Components to silence standard output
    NullBuffer null_buffer;
    std::ostream null_stream;

    RDSContext_T(float rate, bool is_rbds, bool show_partial) : null_stream(&null_buffer) {
        options.samplerate = rate;
        options.feed_thru = false;
        options.rbds = is_rbds; // Store the RBDS flag for decoding logic
        options.show_partial = show_partial; // Store partial flag

        // Initialize DSP engine (demodulator)
        subcarrier = std::make_unique<redsea::SubcarrierSet>(options.samplerate);

        // Initialize Logic engine (group decoder)
        // Channel 0 is standard for mono/stereo FM
        channel = std::make_unique<redsea::Channel>(options, 0);
    }
};

// Helper function to sanitize RDS strings.
// Trims leading/trailing whitespace but preserves internal spacing.
static void clean_rds_string(char* s) {
    if (!s || *s == '\0') return;

    size_t len = strlen(s);
    if (len == 0) return;

    // 1. Find the first non-space char
    size_t start = 0;
    while (start < len && std::isspace((unsigned char)s[start])) {
        start++;
    }

    // If string is all spaces, empty it
    if (start == len) {
        s[0] = '\0';
        return;
    }

    // 2. Find the last non-space char
    size_t end = len - 1;
    while (end > start && std::isspace((unsigned char)s[end])) {
        end--;
    }

    // 3. Move the trimmed part to the front
    size_t new_len = end - start + 1;
    if (start > 0) {
        std::memmove(s, s + start, new_len);
    }

    // 4. Null terminate
    s[new_len] = '\0';
}

// --- C Interface Implementation ---

extern "C" {

RedseaHandle redsea_init(float sample_rate, bool is_rbds, bool show_partial) {
    try {
        return new RDSContext_T(sample_rate, is_rbds, show_partial);
    } catch (...) {
        return nullptr;
    }
}

void redsea_free(RedseaHandle ctx) {
    if (ctx) delete ctx;
}

void redsea_process_mpx(RedseaHandle ctx, const float* mpx_data, int num_samples, rds_state_t* out) {
    if (!ctx || !mpx_data || !out || num_samples <= 0) return;

    // 1. Wrap C buffer into Redsea's MPXBuffer container
    redsea::MPXBuffer buffer;

    // Clamp to Redsea's internal max buffer size to prevent overflow
    // (kBufferSize is defined in Redsea internals, usually large enough for ~8k samples)
    if (num_samples > (int)buffer.data.size()) {
        num_samples = buffer.data.size();
    }

    buffer.used_size = num_samples;

    // Copy samples from C array to C++ std::array
    // We use memcpy for speed
    std::memcpy(buffer.data.data(), mpx_data, num_samples * sizeof(float));

    buffer.time_received = std::chrono::system_clock::now();

    // 2. DSP: Demodulate MPX audio into raw bits
    // '1' indicates standard RDS (Stream 0). RDS2 would use 4 streams.
    auto bit_buffer = ctx->subcarrier->chunkToBits(buffer, 1);

    // 3. Logic: Decode bits into Groups and update Station state
    // We pass null_stream so Redsea doesn't print JSON to stdout
    ctx->channel->processBits(bit_buffer, ctx->null_stream);

    // 4. Extraction: Pull data from the C++ Station object into the C struct
    const auto& station = ctx->channel->getStation();

    out->pi_code = station.getPI();
    out->valid = (out->pi_code != 0);

    if (out->valid) {
        // --- Strings ---
        std::string ps = station.getPS();
        std::string rt = station.getRT();

        // Partial Support: If Valid string is empty, check Raw
        if (ctx->options.show_partial) {
            if (ps.empty()) {
                ps = station.getRawPS();
            }
            if (rt.empty()) {
                rt = station.getRawRT();
            }
        }

        std::memset(out->ps_name, 0, sizeof(out->ps_name));
        std::strncpy(out->ps_name, ps.c_str(), 8);
        clean_rds_string(out->ps_name);

        std::memset(out->radiotext, 0, sizeof(out->radiotext));
        std::strncpy(out->radiotext, rt.c_str(), 64);
        clean_rds_string(out->radiotext);

        // --- Flags ---
        out->tp = station.getTP();
        out->ta = station.getTA();
        out->is_music = station.getMusic();

        // --- DI Flags ---
        out->stereo = station.getStereo();
        out->compressed = station.getCompressed();
        out->dynamic = station.getDynamicPTY();
        out->binaural = station.getArtificialHead();

        // --- Program Type (PTY) ---
        int pty_idx = station.getPTY();
        std::string pty_str;
        if (ctx->options.rbds) {
            // US RBDS tables
            pty_str = std::string(redsea::getPTYNameStringRBDS(pty_idx));
        } else {
            // EU RDS tables
            pty_str = std::string(redsea::getPTYNameString(pty_idx));
        }
        std::memset(out->program_type, 0, sizeof(out->program_type));
        std::strncpy(out->program_type, pty_str.c_str(), 16);
        clean_rds_string(out->program_type);

        // --- NEW: Program Type Name (PTYN) ---
        // Group 10A specific description (e.g. "Football")
        std::string ptyn = station.getPTYN();
        std::memset(out->pty_name, 0, sizeof(out->pty_name));
        std::strncpy(out->pty_name, ptyn.c_str(), 8);
        clean_rds_string(out->pty_name);

        // --- Callsign (RBDS Only) ---
        std::memset(out->callsign, 0, sizeof(out->callsign));
        if (ctx->options.rbds) {
            // Calculate callsign from PI code (e.g. 0x54A1 -> "KEXP")
            std::string cs = redsea::getCallsignFromPI(out->pi_code);
            std::strncpy(out->callsign, cs.c_str(), 4);
        }

        // --- Clock Time ---
        std::string ct = station.getClockTime();
        std::memset(out->clock_time, 0, sizeof(out->clock_time));
        std::strncpy(out->clock_time, ct.c_str(), 31);

        // --- Country Code ---
        uint16_t cc = station.getCC();
        uint16_t ecc = station.getECC();
        std::string country = std::string(redsea::getCountryString(cc, ecc));
        // Convert to uppercase
        std::transform(country.begin(), country.end(), country.begin(), ::toupper);

        std::memset(out->country_code, 0, sizeof(out->country_code));
        std::strncpy(out->country_code, country.c_str(), 2);

        // --- Alternative Frequencies ---
        std::vector<int> afs = station.getAltFreqs();
        out->alt_freq_count = 0;
        for (size_t i = 0; i < afs.size() && i < 32; i++) {
            out->alt_freqs[i] = afs[i];
            out->alt_freq_count++;
        }
    }
}

} // extern "C"
