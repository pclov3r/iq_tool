# iq_tool

A multi-threaded command-line tool for resampling, filtering, shifting, and correcting I/Q data streams.

I originally built this tool for a very specific need: processing I/Q files with NRSC-5 (HD Radio) captures for use with the awesome [NRSC5](https://github.com/theori-io/nrsc5) decoder. This means it's particularly good at handling the frequency shifts and metadata found in WAV files from SDR software, but it has grown into a more general-purpose utility. 

The goal of this tool is to provide an easy, fast, and lightweight command-line utility for converting I/Q data from files or SDRs to a specific sample rate and format, and then either piping it to other programs or writing it to a file.

---

### ⚠️ A Quick Word of Warning: This is a fast moving AI-Assisted Project ⚠️

**If you're looking for a stable platform, look at [GNU Radio](https://github.com/gnuradio/gnuradio). This project is a playground and should not be relied upon. At least not yet.**

Let's be upfront: a large language model (AI) helped write a significant portion of this code, *if not most.* I guided it, reviewed its output the best I could, and tested the result, but this project didn't evolve through the typical trial-and-error of a human-only endeavor. Even this README you're reading was drafted by the AI based on the source code, then edited and refined by me.

Second, it's worth knowing that this was a learning project for me. I chose to use C for its simplicity and how it keeps you close to the metal; I also figured it would be a better target for AI code and DSP versus a more complex language like C++. Since I was learning the language as I went, what you'll see in the code is my journey of tackling C, threading, and DSP all at once. The focus was always on getting a practical, working result, which means some of the solutions are probably not what you'd find in a textbook. However, I have made a effort to clean up and refactor the code to the best of my knowledge and research.

*What does this mean?*

*   **It's Experimental.** While it works, it hasn't been battle-tested across a wide variety of systems and edge cases nor has it had a security review.
*   **Design choices not stable.** You may see features, command line options, etc. suddenly appear and disappear. You may also see large commits of lots of changes. **The mainline codebase may also be broken at times due to fast moving code and changes. Releases may be more stable.** 
*   **Bugs are expected.** The logic very likely has quirks that haven't been discovered yet. Other issues causing crashes likely exist too. 
*   **Use with caution!** I wouldn't use this for anything mission-critical without a thorough personal review.

---

### What It Can Do

*   **Multi-Threaded Pipeline:** Uses a Reader -> Pre-Processor -> Resampler -> Post-Processor -> Writer model.
*   **Flexible Inputs:**
    *   **WAV Files:** Reads standard 8-bit and 16-bit complex (I/Q) WAV files.
    *   **Raw I/Q Files:** Just point it at a headerless file, but you have to tell it the sample rate and format.
    *   **SDR Hardware:** Streams directly from **RTL-SDR**, **SDRplay**, **HackRF**, **Airspy**, **Airspy HF+**, and **BladeRF** devices.
    *   **SpyServer Support:** Connect to networked SpyServer instances.
*   **WAV Metadata Parsing:** Automatically reads metadata from SDR I/Q captures to make your life easier, especially for frequency correction.
    *   `auxi` chunks from **SDR Console, SDRconnect,** and **SDRuno**.
    *   SDR# style filenames (e.g., `..._20240520_181030Z_97300000Hz_...`).
*   **Processing Features:**
    *   **Resampling** to a new sample rate.
    *   **Frequency Shifting:** Apply shifts before or after resampling.
    *   **Demodulation & Decoding:**
        *   **Wideband FM (WFM):** Demodulates FM Broadcast signals to audio with optional Redsea RDS/RBDS decoding.
        *   **Narrowband FM (NFM):** Demodulates voice signals with adjustable squelch and standard/narrow bandwidths
        *   **AM:** Demodulates AM signals using a Synchronous (PLL) detector, or standard Envelope detection.
        *   **NRSC5 (HD Radio):** Integrated support for demodulating and playing HD Radio streams (via `libnrsc5`).
*   **Automatic Gain Control (AGC):** A universal Harris/LMS block-level tracker that provides smooth gain control for all signal types. It features a crucial **deadband** that allows strong, stable signals to pass through untouched—preserving Modulation Error Ratio (MER) for digital signals and audio transparency for analog ones—while still capable of automatically rescuing weak signals from deep fades.
    *   **Filtering:**
        *   Apply low-pass, high-pass, band-pass, or notch FIR filters.
        *   Offers two processing methods: a `FIR` (time-domain) method and an `FFT` (frequency-domain) method and will attempt to automatically default to the most suitable method.
    *   **Automatic I/Q Correction:** Can optionally find and fix I/Q imbalance on the fly. *This is very experimental and possibly could make it worse.*
    *   **DC Blocking:** A simple high-pass filter to remove the pesky DC offset.
*   **Versatile Outputs:**
    *   **Container Formats:** `stdout` (for piping), `raw-file` (headerless files), standard `wav`, and `wav-rf64` (for files >4GB).
    *   **Sample Formats:** Supports a variety of complex sample formats including `cs16`, `cu8`, `cs8`, and more.
    *   **Presets:** Define your favorite settings in a config file for quick access.

### Getting Started: Building from Source

**Dependencies:**
*   **A C11 compiler:** (GCC, Clang)
*   **CMake:** (version 3.10 or higher)
*   **libsndfile**
*   **liquid-dsp**
*   **libexpat**
*   **pthreads:** This is a standard system component on Linux/macOS. On Windows, a compatible version is typically included with the MinGW-w64 toolchain.
*   **(Optional) C++17 Compliant Compiler:** (G++, Clang). For enabling Redsea RDS decoding within the WFM module.
*   **(Optional) libiconv:** For enabling Redsea RDS decoding within the WFM module.
*   **(Optional) libfftw3:** For a performance boost with FFT-based filtering, install (`libfftw3-dev`) **before** building or installing `liquid-dsp`.
*   **(Optional) RTL-SDR Library (librtlsdr):** For RTL-SDR support (e.g., `librtlsdr-dev`).
*   **(Optional) BladeRF Library (libbladeRF):** For BladeRF support (e.g., `libbladerf-dev`). Windows installers found **[here](https://github.com/Nuand/bladeRF/releases)**.
*   **(Optional) HackRF Library (libhackrf):** For HackRF support (e.g., `libhackrf-dev`).
*   **(Optional) Airspy Library (libairspy):** For Airspy support (e.g., `libairspy-dev`).
*   **(Optional) Airspy HF+ Library (libairspyhf):** For Airspy HF+ support (e.g., `libairspyhf-dev`).
*   **(Optional) SDRplay API Library:** To build with SDRplay support, you must first download and install the official API from the **[SDRplay website](https://www.sdrplay.com/downloads/)**.
*   **(Optional) NRSC5 Library (libnrsc5):** For NRSC5 (HD Radio) playback support. GitHub repo found **[here](https://github.com/theori-io/nrsc5)**.

#### On Linux (Debian/Ubuntu Example for all modules)

1.  **Install the dependencies:**
    ```bash
    sudo apt-get update
    sudo apt-get install build-essential cmake libsndfile1-dev libliquid-dev libexpat1-dev librtlsdr-dev libhackrf-dev libairspy-dev libairspyhf-dev libbladerf-dev libusb-1.0-0-dev libfftw3-dev
    ```

2.  **Build NRSC5 if enabling support:**
    ```bash
    git clone https://github.com/theori-io/nrsc5
    cd nrsc5
    mkdir build
    cd build
    cmake .. (Add -DUSE_SSE=ON for x86 or -DUSE_NEON=ON for ARM for higher performance)
    make or make -j N (Replace N with number of threads)
    make install
    ldconfig
    ```

3.  **Build the tool:**
    ```bash
    git clone https://github.com/pclov3r/iq_tool.git
    cd iq_tool
    mkdir build
    cd build

    # Build without any optional SDR support besides Spyserver support and no RDS decoding support in the WFM module
    cmake ..

    # Or, build with everything enabled
    cmake -DWITH_RTLSDR=ON -DWITH_SDRPLAY=ON -DWITH_HACKRF=ON -DWITH_AIRSPY=ON -DWITH_AIRSPYHF=ON -DWITH_BLADERF=ON -DWITH_NRSC5=ON -DWITH_REDSEA=ON ..

    make or make -j N (Replace N with number of threads)
    make install 
    ```
You'll find the `iq_tool` executable in the `build` directory.

### On Windows

#### Using Pre-compiled Binaries

Pre-compiled binaries for Windows without NRSC5 output module support are available on the project's **[releases page](https://github.com/pclov3r/iq_tool/releases)**.

> **Note:** Not every commit will result in a new release and Windows binary. Releases are typically made after significant changes.

1.  **Download and Extract:** Download the `.zip` archive. You must extract **all files** (the `.exe` and all `.dll` files) into the same folder for the program to work.
2.  **Choose the Correct Build:** Pay close attention to the AVX vs. AVX2 builds available.
    *   **⚠️ Important:** Attempting to run an AVX2-optimized build on a CPU that does not support the AVX2 instruction set **will cause the program to crash.** 
    *   **Try using a AVX build if the AVX2 build crashes but you should attempt to use a AVX2 build first for better performance.**

#### Building from Source

1.  **Environment:** Your best bet is to use a **MinGW-w64** toolchain. Building within an **MSYS2** environment may also work but has not been tested.
2.  **Build Script:** The repository includes a Windows build script (`support/win-cross-compile`) that can be used as a starting point.
    *   **Note:** The script contains hard-coded paths to dependencies. You will need to edit this script and adjust the paths to match your own build environment or build the dependencies matching the layout.

### How to Use It

The best way to see all options is to run `iq_tool --help`.

**Note on Command-Line Options:** The help message is dynamic. It intelligently shows only the options relevant to the features you enabled during compilation. For example, if you build the tool without SDRplay support, you won't see the `--sdrplay-*` options. The example below shows the output with all SDR modules enabled.

#### Command-Line Options

```text
Usage: iq_tool -i <in_type> [in_file] -o <out_type> [out_file] [options]

Required Input & Output
    -i, --input=<str>                         Specifies the input module.
    -o, --output=<str>                        Specifies the output module and optional file path

Output Options
    --output-sample-format=<str>              Sample format for output data {cs8|cu8|cs16|...}

Processing Options
    --output-rate=<flt>                       Output sample rate in Hz.
    --input-gain-multiplier=<flt>             Apply a linear gain multiplier to INPUT samples (before processing).
    --output-gain-multiplier=<flt>            Apply a linear gain multiplier to OUTPUT samples (after processing).
    --dbm-offset=<flt>                        Override the dBFS-to-dBm calibration offset.
    --freq-shift=<flt>                        Apply a direct frequency shift in Hz (e.g., -100e3)
    --shift-after-resample                    Apply frequency shift AFTER resampling (default is before)
    --raw-passthrough                         Bypass all processing. Copies raw input bytes directly to output.
    --iq-correction                           (Optional) Enable automatic I/Q imbalance correction.
    --dc-block                                (Optional) Enable DC offset removal (high-pass filter).
    --preset=<str>                            Use a preset for a common target.

Output Automatic Gain Control
    --output-agc                              Enable automatic gain control on the output.
    --agc-target=<flt>                        AGC target magnitude (0.0 - 1.0). (Default: 0.12)

Filtering Options (Chain up to 5 by combining options or adding suffixes -2, -3, etc. e.g., --lowpass --stopband --lowpass-2 --pass-range --pass-range-2)
    --lowpass=<flt>                           Isolate signal at DC. Keeps freqs from -<hz> to +<hz>.
    --highpass=<flt>                          Remove signal at DC. Rejects freqs from -<hz> to +<hz>.
    --pass-range=<str>                        Isolate a specific band. Format: 'start_freq:end_freq'.
    --stopband=<str>                          Remove a specific band (notch). Format: 'start_freq:end_freq'.

Filter Quality Options
    --transition-width=<flt>                  Set filter sharpness by transition width in Hz. (Default: Auto).
    --filter-taps=<int>                       Set exact filter length. Overrides --transition-width.
    --attenuation=<flt>                       Set global filter & resampler stop-band attenuation in dB. (Default: Auto).

Filter Implementation Options
    --filter-type=<str>                       Set filter implementation {fir|fft}. (Default: auto).
    --filter-fft-size=<int>                   Set FFT size for 'fft' filter type. Must be a power of 2.

SDR General Options
    --sdr-rf-freq=<flt>                       (Required for SDR) Tuner center frequency in Hz
    --sdr-freq-offset=<flt>                   Frequency offset in Hz (e.g. 125e6 for HamItUp).
    --sdr-sample-rate=<flt>                   Set sample rate in Hz. (Device-specific default)
    --sdr-bias-t                              (Optional) Enable Bias-T power.

WAV Input (wav)
    --wav-center-target-freq=<flt>            Shift signal to a new target center frequency (e.g., 97.3e6)

Raw File Input (rawfile)
    --raw-file-input-rate=<flt>               (Required) The sample rate of the RAW input file.
    --raw-file-input-sample-format=<str>      (Required) The sample format of the RAW input file.

Standard Input (stdin)
    --stdin-input-rate=<flt>                  (Required) The sample rate of the stdin stream in Hz.
    --stdin-input-sample-format=<str>         (Required) The sample format of the stdin stream.

RTL-SDR Input (rtlsdr)
    --rtlsdr-device-idx=<int>                 Select specific RTL-SDR device by index (0-indexed). (Default: 0)
    --rtlsdr-gain=<flt>                       Set manual tuner gain in dB (e.g., 28.0, 49.6). Disables AGC.
    --rtlsdr-ppm=<int>                        Set frequency correction in parts-per-million. (Optional, Default: 0)
    --rtlsdr-direct-sampling=<int>            Enable direct sampling mode for HF reception (1=I-branch, 2=Q-branch)

SDRplay Input (sdrplay)
    --sdrplay-bandwidth=<flt>                 Set analog bandwidth in Hz. (Optional, Default: 1.536e6)
    --sdrplay-device-idx=<int>                Select specific SDRplay device by index (0-indexed). (Default: 0)
    --sdrplay-lna-state=<int>                 Set LNA state (0=min gain). Disables AGC.
    --sdrplay-if-gain=<int>                   Set IF gain in dB (fine gain, e.g., -20, -35, -59). (Default: -50 if --sdrplay-lna-state is specified.) Disables AGC.
    --sdrplay-antenna=<str>                   Select antenna port (device-specific).
    --sdrplay-hdr-mode                        (Optional) Enable HDR mode on RSPdx/RSPdxR2.
    --sdrplay-hdr-bw=<flt>                    Set bandwidth for HDR mode. Requires --sdrplay-hdr-mode.
    --sdrplay-notch-fm                        Enable FM Broadcast Notch Filter.
    --sdrplay-notch-dab                       Enable DAB Broadcast Notch Filter.
    --sdrplay-notch-am                        Enable MW/AM Notch Filter (RSPduo Tuner A only).

HackRF Input (hackrf)
    --hackrf-lna-gain=<int>                   Set LNA (IF) gain in dB. (Optional, Default: 16)
    --hackrf-vga-gain=<int>                   Set VGA (Baseband) gain in dB. (Optional, Default: 0)
    --hackrf-amp-enable                       Enable the front-end RF amplifier (+14 dB).

Airspy Input (airspy)
    --airspy-gain-mode=<str>                  Gain mode: 'linearity', 'sensitivity', or 'manual'. (Default: AGC)
    --airspy-gain-value=<int>                 Gain value for linearity/sensitivity modes (0-21). (Default: 10)
    --airspy-lna-gain=<int>                   Manual LNA gain (0-14). Only with manual mode. (Default: 5)
    --airspy-mixer-gain=<int>                 Manual Mixer gain (0-15). Only with manual mode. (Default: 5)
    --airspy-vga-gain=<int>                   Manual VGA gain (0-15). Only with manual mode. (Default: 5)
    --airspy-sample-format=<str>              Sample format: 'cf32' or 'cs16'. (Default: cs16)
    --airspy-serial=<int>                     Select device by serial number (hex, e.g., 0x123456789ABCDEF0).
    --airspy-packing                          Enable bit-packing mode (12-bit samples).

Airspy HF+ Input (airspyhf)
    --airspyhf-agc=<str>                      AGC mode: 'off', 'low', or 'high'. (Default: high)
    --airspyhf-attn=<flt>                     Attenuation in dB (0.0 to 48.0). (Default: 0.0)
    --airspyhf-preamp                         Enable LNA/PreAmp.
    --airspyhf-serial=<int>                   Select device by serial number (hex, e.g., 0x123456789ABCDEF0).
    --airspyhf-no-lib-dsp                     Disable library DSP processing (IQ correction, DC removal, etc).

BladeRF Input (bladerf)
    --bladerf-device-idx=<int>                Select specific BladeRF device by index (0-indexed). (Default: 0)
    --bladerf-load-fpga=<str>                 Load an FPGA bitstream from the specified file.
    --bladerf-bandwidth=<flt>                 Set analog bandwidth in Hz. (Not applicable in 8-bit high-speed mode)
    --bladerf-gain=<int>                      Set overall manual gain in dB. Disables AGC.
    --bladerf-channel=<int>                   For BladeRF 2.0: Select RX channel 0 (RXA) or 1 (RXB). (Default: 0)
    --bladerf-bit-depth=<int>                 Set capture bit depth {8|12}. 8-bit mode is for BladeRF 2.0 only. (Default: 12, auto-switches to 8 for rates > 61.44 MHz on BladeRF 2.0)

SpyServer Client Input (spyserver-client)
    --spyserver-client-host=<str>             Hostname or IP of the spyserver instance (Required).
    --spyserver-client-port=<int>             Port number of the spyserver instance (Required).
    --spyserver-client-gain=<int>             Set manual gain. Disables AGC. (Ignored on servers without gain control)
    --spyserver-client-sample-format=<str>    Select sample format {cu8|cs16|cs24|cf32}. Default is cu8.

RAW File Output (rawfile)

    (No module-specific options)

WAV Output (wav)

    (No module-specific options)

WAV RF64 Output (wav-rf64)

    (No module-specific options)

Standard Output (stdout)

    (No module-specific options)

NRSC5 Output (nrsc5)
    --nrsc5-mode=<str>                        Set decoder mode {cs16-fm|cs16-am|cu8-fm|cu8-am}. (Default: cs16-fm)
    --nrsc5-program=<int>                     Select HD program/subchannel (0-7). (Required)

WFM Output (wfm)
    --wfm-de-emphasis-time=<flt>              Set FM de-emphasis time constant in microseconds (default: 75.0).
    --wfm-gain=<flt>                          Set audio output gain (linear).
    --wfm-force-stereo                        Force stereo decoding regardless of signal quality.
    --wfm-force-mono                          Force mono output.
    --wfm-raw-mpx-stdout                      Pipe raw MPX data (S16 Mono) to stdout while playing audio.
    --wfm-no-rds                              Disable RDS decoding (Enabled by default).
    --wfm-rds                                 Use World RDS standard (Default is US RBDS).
    --wfm-rds-partial                         Show partial/noisy RDS text (PS/RT).

NFM Output (nfm)
    --nfm-gain=<flt>                          Audio gain (default: 1.0)
    --nfm-squelch=<flt>                       Squelch threshold in dB (default: -50.0)
    --nfm-narrow                              Enable Narrow mode (2.5kHz dev). Default is Standard (5kHz dev).
    --nfm-no-squelch                          Disable squelch (force open audio)
    --nfm-no-discriminator-filter             Disable the discriminator filter.

AM Output (am)
    --am-gain=<flt>                           Set audio output gain (linear).
    --am-cutoff=<flt>                         Set audio lowpass filter cutoff in Hz (default: 10000).
    --am-envelope                             Disable Synchronous AM (PLL) and use Magnitude Envelope Detection.

Available Presets
    cu8-nrsc5                                 Sets sample type to cu8, rate to 1488375.0 Hz for piping to external nrsc5 (FM/AM). Use --output stdout
    cu8-nrsc5-usb                             Sets sample type to cu8, rate to 1488375.0 Hz, isolates USB sideband (102-215kHz) (Hack) for piping to external nrsc5 (FM). Use --output stdout
    cu8-nrsc5-lsb                             Sets sample type to cu8, rate to 1488375.0 Hz, isolates LSB sideband (-215 to -102kHz) (Hack) for piping to external nrsc5 (FM). Use --output stdout
    cs16-fm-nrsc5                             Sets sample type to cs16, rate to 744187.5 Hz for piping to external nrsc5 (FM). Use --output stdout
    cs16-fm-nrsc5-usb                         Sets sample type to cs16, rate to 744187.5 Hz, isolates USB sideband (102-215kHz) (Hack) for piping to external nrsc5 (FM). Use --output stdout
    cs16-fm-nrsc5-lsb                         Sets sample type to cs16, rate to 744187.5 Hz, isolates LSB sideband (-215 to -102kHz) (Hack) for piping to external nrsc5 (FM). Use --output stdout
    cs16-am-nrsc5                             Sets sample type to cs16, rate to 46511.71875 Hz for piping to external nrsc5 (AM). Use --output stdout

Help & Version
    -v, --version                             show program's version number and exit
    -h, --help                                show this help message and exit
```

#### Examples

**Example 1: Basic File Resampling**
Resample a WAV file to a 16-bit RF64 (large WAV) file with a custom output rate.
```bash
iq_tool --input wav my_capture.wav --output wav-rf64 processed.wav --output-sample-format cs16 --output-rate 240000
```

**Example 2: Channel Selection (FFT Filter)**
Isolate a specific range of frequencies from a live SDR stream. The tool will automatically select the `fft` filter because this is an asymmetric (offset) filter.
```bash
iq_tool --input rtlsdr --sdr-rf-freq 98.5e6 --pass-range 50e3:250e3 --output stdout --output-rate 240000 --output-sample-format cs16 | ...
```

**Example 3: Piping to a Decoder with a Preset (WAV Input)**
Use the `cu8-nrsc5` preset to resample and automatically correct the frequency, then pipe it to `nrsc5`. (Assumes the WAV has frequency metadata).
```bash
iq_tool --input wav my_capture.wav --wav-center-target-freq 97.3e6 --preset cu8-nrsc5 --output stdout | nrsc5 -r - 0
```

**Example 4: Streaming from an SDRplay Device with Preset**
Tune an SDRplay RSPdx to 102.5 MHz, set a manual gain level (using LNA state) and select an antenna port before piping to nrsc5.
```bash
iq_tool --input sdrplay --sdr-rf-freq 102.5e6 --sdrplay-lna-state 4 --sdrplay-antenna B --preset cu8-nrsc5 --output stdout | nrsc5 -r - 0
```

### Configuration via Presets

`iq_tool` supports presets to save you from repeatedly typing the same output formatting options. A default `iq_tool_presets.conf` is included in the repository, which you can use as a starting point for your own configurations. A preset bundles common settings like `target_rate`, `sample_format_name`, and `output_type` into a single flag (`--preset <name>`), which is perfect for common piping scenarios.

**Pro Tip:** If the tool finds config files in multiple locations, it will print a warning and load **none** of them to avoid confusion. Just delete the duplicates to fix it.

**Search Locations:**
*   **Windows:** The executable's directory, `%APPDATA%\\iq_tool\\`, `%PROGRAMDATA%\\iq_tool\\`
*   **Linux:** The current directory, `$XDG_CONFIG_HOME/iq_tool/`, `/etc/iq_tool/` , `/usr/local/etc/iq_tool`

### Current State & Future Plans

This tool is a work in progress.

*   **Known Issues:**
    *   It's experimental. Expect bugs. 
    *   Windows builds are 64-bit only. I see no reason to post 32-bit ones given Windows 10 is end of life soon and Windows 11 is 64-bit only. If I'm wrong and it's required open an issue.
    *   As mentioned, IQ correction may not be functioning correctly.
    *   Log verbosity levels are not yet refined. The output is noisy, and many messages are not be at the appropriate level (e.g., ERROR vs. FATAL).

*   **Roadmap:**
    *   [x] Add RTL-SDR support.
    *   [x] Add BladeRF support.
    *   [x] Refactor configuration system for full modularity.
    *   [x] Implement FFT-based filtering option.
    *   [x] SpyServer Support
    *   [x] Implement Output AGC (DX, Local, and Digital profiles).
    *   [x] Airspy Support
    *   [x] Airspy HF+ support
    *   [x] SpyServer Support
    *   [ ] Add HydraSDR support
    *   [ ] Improve I/Q correction algorithm stability.
    *   [ ] Refine and standardize log levels throughout the application.
    *   [ ] Add unit tests. 
    *   [ ] General code cleanup and comment refactoring.
    *   [ ] Improve the README.

### For Developers: Architecture & Design

This section provides a high-level overview of the tool's internal design for those looking to understand, modify, or extend the codebase.

#### The Data Flow Pipeline

`iq_tool` processes data through a high-performance pipeline of concurrent, decoupled stages. Each major task runs in its own dedicated thread, using thread-safe blocking queues to hand off data from one stage to the next. To maximize throughput and minimize latency, the pipeline utilizes a pre-allocated memory pool of `SampleChunk` structures, allowing for high-speed data flow without the overhead of dynamic memory allocation.

The sequence of threads and their responsibilities are as follows:

1.  **Source Thread (Optional / Live Mode Only):**
    This thread is active when using live inputs (SDR hardware, network streams, or OS pipes). Its sole responsibility is to acquire data from the hardware or protocol and write it into a high-capacity, lock-free ring buffer. It uses a structured packet format (Header + Payload) which allows it to communicate stream events, such as hardware overruns or resets, to the rest of the pipeline. This thread ensures that real-time data acquisition is never delayed by downstream DSP processing.

2.  **Reader Thread:**
    The bridge between raw data and the DSP chain. 
    *   **In Live Mode:** It "sips" data from the Source Ring Buffer, parses the packet headers, and populates `SampleChunk` structures.
    *   **In File Mode:** It reads raw data directly from the disk.
    Once a `SampleChunk` is filled with raw bytes, the Reader thread pushes it into the Pre-Processor queue.

3.  **Pre-Processor Thread:**
    The first DSP stage. It dequeues `SampleChunk` structures and performs operations on the raw data before it is resampled. This includes:
    *   Converting raw integer samples into 32-bit complex floats (CF32).
    *   Applying initial input gain.
    *   Executing DC-offset removal and automatic I/Q imbalance correction.
    *   Performing pre-resample frequency shifting (NCO) and filtering.

4.  **Resampler Thread:**
    Handles sample rate conversion. It pulls processed CF32 buffers and uses a polyphase filter (via `liquid-dsp`) to transition the data to the user-specified target rate. It utilizes a "ping-pong" buffer strategy within the `SampleChunk` to ensure high-speed, zero-copy data flow.

5.  **Post-Processor Thread:**
    The final DSP stage. It operates on the resampled complex float data to prepare it for the final output. Responsibilities include:
    *   Applying secondary filtering or Automatic Gain Control (AGC).
    *   Performing post-resample frequency shifting.
    *   Converting the internal CF32 data into the final user-selected byte format (e.g., `cs16`, `cu8`).

6.  **Writer Thread:**
    The final stage in the pipeline. It dequeues formatted buffers and writes them to the destination.
    *   **Synchronous Output (File/Stdout):** It manages backpressure; if the destination is slow, the writer will block the pipeline to ensure no data is lost.
    *   **Real-time Output (Audio/Live):** For audio modules (AM, NFM, WFM), the writer interacts with the OS sound driver. If the pipeline runs faster than real-time, the writer manages the timing to ensure perfectly paced playback.

#### The Modular I/O System

The tool features a fully modular architecture designed for the rapid extension of both data sources and data sinks. By decoupling hardware and format-specific logic from the core DSP pipeline, `iq_tool` can easily be adapted to new SDR hardware, network protocols, or demodulation schemes.

*   **The Core Interfaces (`module.h`):** All modules are built upon two primary virtual method tables (v-tables) defined in `module.h`:
    *   **`InputModuleInterface`**: Standardizes how the pipeline initializes hardware, starts data acquisition, and cleans up resources for any data source.
    *   **`OutputModuleInterface`**: Standardizes how data is delivered to a destination, allowing the tool to switch seamlessly between writing raw files, generating WAVs, or streaming real-time audio.
*   **The Module Headers:** Every module provides its own header file (e.g., `input_rtlsdr.h` or `output_am.h`). These headers act as the bridge to the registry, exporting two critical functions: one to return the module's API v-table and another to provide its unique command-line options.
*   **The Registry (`module_registry.c` / `module_registry.h`):** This serves as the system's central "factory." It maintains a comprehensive catalog of every compiled-in module. When a user selects a mode via the `--input` or `--output` arguments, the registry performs a lookup and injects the appropriate logic into the pipeline at runtime.
*   **Adding a New Module:**
    1.  **Define the Header:** Create a new header file (e.g., `input_new_sdr.h`) that declares the functions needed to retrieve the module's API and CLI options.
    2.  **Implement the Interface:** Create the corresponding implementation file (e.g., `input_new_sdr.c`) that fulfills the required function pointers in the `InputModuleInterface` or `OutputModuleInterface`.
    3.  **Define CLI Options:** Use the built-in `argparse` integration within your module to define any unique command-line flags (e.g., gain settings for a specific SDR or cutoff frequencies for a demodulator).
    4.  **Register the Module:** Include your new header in `module_registry.c` and add a new entry to the `temp_modules[]` array to make it visible to the command-line parser and the factory.
    5.  **Update the Build System:** Add the new source file to `CMakeLists.txt` and link any necessary external libraries.

This design ensures that hardware-specific quirks and complex output formats remain isolated from the high-speed processing core, resulting in a codebase that is clean, maintainable, and highly portable.

### Contributing

Contributions are highly welcome! Whether you've found a bug or have a cool idea for a feature, feel free to open an issue or send a pull request.

Since an AI had a heavy hand in writing this tool, AI-assisted pull requests are totally fair game. Just a heads-up: all PRs, whether from a human or a bot, will be carefully reviewed to make sure they fit the project's goals and quality standards. I look forward to seeing what you come with.
