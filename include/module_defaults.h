/**
 * @file module_defaults.h
 * @brief Policy layer mapping for hardware-specific defaults and constraints.
 *
 * This file acts as the configuration manifest for SDR input hardware. It isolates
 * physical constraints, such as sample rates, filter attenuations, and buffer
 * sizes, from the central DSP pipeline and the Module Registry.
 */

#ifndef MODULE_DEFAULTS_H_
#define MODULE_DEFAULTS_H_

// =============================================================================
// == SDR Hardware Interaction & Tuning
// =============================================================================
// Default values and tuning parameters specific to each SDR device.

// --- WAV Constants ---
#define WAV_DEMOD_AUDIO_BUFFER_SIZE                     (128 * 1024)

// --- RAWFILE Constants ---
#define RAWFILE_DEMOD_AUDIO_BUFFER_SIZE                 (128 * 1024)

// --- STDIN Constants ---
#define STDIN_DEMOD_AUDIO_BUFFER_SIZE                   (128 * 1024)

// --- RTLSDR Constants ---
#define RTLSDR_DEFAULT_FILTER_ATTENUATION_DB            60.0f
#define RTLSDR_DEFAULT_SAMPLE_RATE                      2400000.0
#define RTLSDR_PASSTHROUGH_BUFFER_SIZE                  16384
#define RTLSDR_DEMOD_AUDIO_BUFFER_SIZE                  (128 * 1024)

// --- SDRPLAY Constants ---
#define SDRPLAY_DEFAULT_FILTER_ATTENUATION_DB           80.0f
#define SDRPLAY_DEFAULT_SAMPLE_RATE_HZ                  2000000.0
#define SDRPLAY_DEFAULT_BANDWIDTH_HZ                    1536000.0
#define SDRPLAY_DEFAULT_IF_GAIN_DB                      -50
#define MAX_SDRPLAY_CONVERSION_SAMPLES                  16384
#define SDRPLAY_DEMOD_AUDIO_BUFFER_SIZE                 (128 * 1024)

// --- HACKRF Constants ---
#define HACKRF_DEFAULT_FILTER_ATTENUATION_DB            60.0f
#define HACKRF_DEFAULT_SAMPLE_RATE                      8000000.0
#define HACKRF_DEFAULT_LNA_GAIN                         16
#define HACKRF_DEFAULT_VGA_GAIN                         0
#define HACKRF_DEMOD_AUDIO_BUFFER_SIZE                  (128 * 1024)

// --- BLADERF Constants ---
#define BLADERF_DEFAULT_FILTER_ATTENUATION_DB           80.0f
#define BLADERF_DEFAULT_SAMPLE_RATE_HZ                  2000000
#define BLADERF_DEFAULT_BANDWIDTH_HZ                    1500000
#define BLADERF_TRANSFER_SIZE_SECONDS                   0.020
#define BLADERF_PROFILE_LOWLATENCY_NUM_BUFFERS          32
#define BLADERF_PROFILE_LOWLATENCY_NUM_TRANSFERS        16
#define BLADERF_PROFILE_BALANCED_NUM_BUFFERS            64
#define BLADERF_PROFILE_BALANCED_NUM_TRANSFERS          32
#define BLADERF_PROFILE_HIGHTHROUGHPUT_NUM_BUFFERS      96
#define BLADERF_PROFILE_HIGHTHROUGHPUT_NUM_TRANSFERS    48
#define BLADERF_DEMOD_AUDIO_BUFFER_SIZE                 (128 * 1024)

// --- AIRSPY Constants ---
#define AIRSPY_DEFAULT_FILTER_ATTENUATION_DB            80.0f
#define AIRSPY_DEFAULT_SAMPLE_RATE                      2500000.0
#define AIRSPY_DEFAULT_SAMPLE_FORMAT                    CS16
#define AIRSPY_DEFAULT_GAIN_VALUE                       10
#define AIRSPY_DEFAULT_LNA_GAIN                         5
#define AIRSPY_DEFAULT_MIXER_GAIN                       5
#define AIRSPY_DEFAULT_VGA_GAIN                         5
#define AIRSPY_DEMOD_AUDIO_BUFFER_SIZE                  (128 * 1024)

// --- AIRSPY HF+ Constants ---
#define AIRSPYHF_DEFAULT_FILTER_ATTENUATION_DB          100.0f
#define AIRSPYHF_DEFAULT_SAMPLE_RATE                    768000
#define AIRSPYHF_DEMOD_AUDIO_BUFFER_SIZE                (128 * 1024)

// --- SPYSERVER Constants --
#define SPYSERVER_DEFAULT_SAMPLE_RATE_HZ                600000.0
#define SPYSERVER_MAX_BUFFER_BYTES                      (256 * 1024 * 1024)
#define SPYSERVER_PREBUFFER_TARGET_SECONDS              5.0f
#define SPYSERVER_PREBUFFER_MIN_BYTES                   65536
#define SPYSERVER_BUFFER_HEADROOM_FACTOR                4.0f
#define SPYSERVER_RING_BUFFER_MIN_BYTES                 (1024 * 1024)
#define SPYSERVER_PREBUFFER_MAX_FILL_RATIO              0.8f
#define SPYSERVER_DEMOD_AUDIO_BUFFER_SIZE               (1536 * 1024)

#endif // MODULE_DEFAULTS_H_
