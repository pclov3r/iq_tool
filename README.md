# iq_resample_tool

A multi-threaded command-line tool for resampling, filtering, shifting, and correcting I/Q data streams.

I originally built this tool for a very specific need: processing NRSC-5 (HD Radio) captures for use with the awesome [NRSC5](https://github.com/theori-io/nrsc5) decoder. This means it's particularly good at handling the frequency shifts and metadata found in WAV files from SDR software, but it has grown into a more general-purpose utility for prepping I/Q data for any downstream tool.

---

### ⚠️ A Quick Word of Warning: This is an AI-Assisted Project ⚠️

Let's be upfront: a large language model (AI) helped write a significant portion of this code, *if not most.* I guided it, reviewed its output the best I could, and tested the result, but this project didn't evolve through the typical trial-and-error of a human-only endeavor. Even this README you're reading was drafted by the AI based on the source code, then edited and refined by me.

Second, it's worth knowing that this was a learning project for me. I chose to use  C for its simplicity and how it keeps you close to the metal; I also figured it would be a better target for AI code and DSP versus a more complex language like C++. Since I was learning the language as I went, what you'll see in the code is my journey of tackling C, threading, and DSP all at once. The focus was always on getting a practical, working result, which means some of the solutions are probably not what you'd find in a textbook. However, I have made a effort to clean up and refactor the code to the best of my knowledge and research.

*What does this mean?*

*   **It's Experimental.** While it works, it hasn't been battle-tested across a wide variety of systems and edge cases nor has it had a security review.
*   **Design choices not stable.** You may see features, command line options, etc. suddenly appear and disappear. You may also see large commits of lots of changes. **The mainline codebase may also be broken at times. Releases may be more stable.** 
*   **Bugs are expected.** The logic very likely has quirks that haven't been discovered yet. Other issues causing crashes likely exist too. 
*   **Use with caution!** I wouldn't use this for anything mission-critical without a thorough personal review of the code. *For serious work, a mature framework like* [GNU Radio](https://github.com/gnuradio/gnuradio) *is always a better bet.*

---

### What It Can Do

*   **Multi-Threaded Pipeline:** Uses a Reader -> Pre-Processor -> Resampler -> Post-Processor -> Writer model.
*   **Flexible Inputs:**
    *   **WAV Files:** Reads standard 8-bit and 16-bit complex (I/Q) WAV files.
    *   **Raw I/Q Files:** Just point it at a headerless file, but you have to tell it the sample rate and format.
    *   **SDR Hardware:** Streams directly from **RTL-SDR**, **SDRplay**, **HackRF**, and **BladeRF** devices.
*   **WAV Metadata Parsing:** Automatically reads metadata from SDR I/Q captures to make your life easier, especially for frequency correction.
    *   `auxi` chunks from **SDR Console, SDRconnect,** and **SDRuno**.
    *   SDR# style filenames (e.g., `..._20240520_181030Z_97300000Hz_...`).
*   **Processing Features:**
    *   **Resampling** to a new sample rate.
    *   **Frequency Shifting:** Apply shifts before or after resampling.
    *   **Filtering:**
        *   Apply low-pass, high-pass, band-pass, or notch FIR filters.
        *   Offers two processing methods: a `FIR` (time-domain) method and an `FFT` (frequency-domain) method and will attempt to automatically default to the most suitable method.
    *   **Automatic I/Q Correction:** Can optionally find and fix I/Q imbalance on the fly. *This is very experimental and possibly could make it worse.*
    *   **DC Blocking:** A simple high-pass filter to remove the pesky DC offset.
*   **Versatile Outputs:**
    *   **Container Formats:** `raw` (for piping), standard `wav`, and `wav-rf64` (for files >4GB).
    *   **Sample Formats:** Supports a variety of complex sample formats including `cs16`, `cu8`, `cs8`, and more.
    *   **Presets:** Define your favorite settings in a config file for quick access.

### Getting Started: Building from Source

You'll need a pretty standard C development environment.

**Dependencies:**
*   A C99 compiler (GCC, Clang, MSVC)
*   **CMake** (version 3.10 or higher)
*   **libsndfile**
*   **liquid-dsp**
*   **libexpat**
*   **pthreads:** This is a standard system component on Linux/macOS. On Windows, a compatible version is typically included with the MinGW-w64 toolchain.
*   **(Optional) libfftw3:** For a performance boost with FFT-based filtering, install (`libfftw3-dev`) **before** building or installing `liquid-dsp`.
*   **(Optional) RTL-SDR Library (librtlsdr):** For RTL-SDR support (e.g., `librtlsdr-dev`).
*   **(Optional) BladeRF Library (libbladeRF):** For BladeRF support (e.g., `libbladerf-dev`). Windows installers found **[here](https://github.com/Nuand/bladeRF/releases)**.
*   **(Optional) HackRF Library (libhackrf):** For HackRF support (e.g., `libhackrf-dev`).
*   **(Optional) SDRplay API Library:** To build with SDRplay support, you must first download and install the official API from the **[SDRplay website](https://www.sdrplay.com/downloads/)**.

#### On Linux (Debian/Ubuntu Example)

1.  **Install the boring stuff:**
    ```bash
    sudo apt-get update
    sudo apt-get install build-essential cmake libsndfile1-dev libliquid-dev libexpat1-dev librtlsdr-dev libhackrf-dev libbladerf-dev libusb-1.0-0-dev libfftw3-dev
    ```

2.  **Build the tool:**
    ```bash
    git clone https://github.com/pclov3r/iq_resample_tool.git
    cd iq_resample_tool
    mkdir build
    cd build

    # Build without any optional SDR support
    cmake ..

    # Or, build with everything enabled
    cmake -DWITH_RTLSDR=ON -DWITH_SDRPLAY=ON -DWITH_HACKRF=ON -DWITH_BLADERF=ON ..

    make
    ```
You'll find the `iq_resample_tool` executable in the `build` directory.

### On Windows

#### Using Pre-compiled Binaries

Pre-compiled binaries for Windows are available on the project's **[releases page](https://github.com/pclov3r/iq_resample_tool/releases)**.

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

The best way to see all options is to run `iq_resample_tool --help`.

**Note on Command-Line Options:** The help message is dynamic. It intelligently shows only the options relevant to the features you enabled during compilation. For example, if you build the tool without SDRplay support, you won't see the `--sdrplay-*` options. The example below shows the output with all SDR modules enabled.

#### Command-Line Options

```text
Required Input & Output
    -i, --input=<str>                     Specifies the input type {wav|raw-file|rtlsdr|sdrplay|hackrf|bladerf}
    -f, --file=<str>                      Output to a file.
    -o, --stdout                          Output binary data for piping to another program.

Output Options
    --output-container=<str>              Specifies the output file container format {raw|wav|wav-rf64}
    --output-sample-format=<str>          Sample format for output data {cs8|cu8|cs16|...}

Processing Options
    --output-rate=<flt>                   Output sample rate in Hz. (Required if no preset or --no-resample is used)
    --gain-multiplier=<flt>               Apply a linear gain multiplier to the samples
    --freq-shift=<flt>                    Apply a direct frequency shift in Hz (e.g., -100e3)
    --shift-after-resample                Apply frequency shift AFTER resampling (default is before)
    --no-resample                         Process at native input rate. Bypasses the resampler but applies all other DSP.
    --raw-passthrough                     Bypass all processing. Copies raw input bytes directly to output.
    --iq-correction                       (Optional) Enable automatic I/Q imbalance correction.
    --dc-block                            (Optional) Enable DC offset removal (high-pass filter).
    --preset=<str>                        Use a preset for a common target.

Filtering Options (Chain up to 5 by combining options or adding suffixes -2, -3, etc. e.g., --lowpass --stopband --lowpass-2 --pass-range --pass-range-2)
    --lowpass=<flt>                       Isolate signal at DC. Keeps freqs from -<hz> to +<hz>.
    --highpass=<flt>                      Remove signal at DC. Rejects freqs from -<hz> to +<hz>.
    --pass-range=<str>                    Isolate a specific band. Format: 'start_freq:end_freq'.
    --stopband=<str>                      Remove a specific band (notch). Format: 'start_freq:end_freq'.

Filter Quality Options
    --transition-width=<flt>              Set filter sharpness by transition width in Hz. (Default: Auto).
    --filter-taps=<int>                   Set exact filter length. Overrides --transition-width.
    --attenuation=<flt>                   Set filter stop-band attenuation in dB. (Default: 60).

Filter Implementation Options (Advanced)
    --filter-type=<str>                   Set filter implementation {fir|fft}. (Default: auto).
    --filter-fft-size=<int>               Set FFT size for 'fft' filter type. Must be a power of 2.

SDR General Options
    --sdr-rf-freq=<flt>                   (Required for SDR) Tuner center frequency in Hz
    --sdr-sample-rate=<flt>               Set sample rate in Hz. (Device-specific default)
    --sdr-bias-t                          (Optional) Enable Bias-T power.

WAV Input Specific Options
    --wav-center-target-freq=<flt>        Shift signal to a new target center frequency (e.g., 97.3e6)

Raw File Input Options
    --raw-file-input-rate=<flt>           (Required) The sample rate of the raw input file.
    --raw-file-input-sample-format=<str>  (Required) The sample format of the raw input file.

RTL-SDR-Specific Options
    --rtlsdr-device-idx=<int>             Select specific RTL-SDR device by index (0-indexed). (Default: 0)
    --rtlsdr-gain=<flt>                   Set manual tuner gain in dB (e.g., 28.0, 49.6). Disables AGC.
    --rtlsdr-ppm=<int>                    Set frequency correction in parts-per-million. (Optional, Default: 0)
    --rtlsdr-direct-sampling=<int>        Enable direct sampling mode for HF reception (1=I-branch, 2=Q-branch)

SDRplay-Specific Options
    --sdrplay-bandwidth=<flt>             Set analog bandwidth in Hz. (Optional, Default: 1.536e6)
    --sdrplay-device-idx=<int>            Select specific SDRplay device by index (0-indexed). (Default: 0)
    --sdrplay-lna-state=<int>             Set LNA state (0=min gain). Disables AGC.
    --sdrplay-if-gain=<int>               Set IF gain in dB (fine gain, e.g., -20, -35, -59). (Default: -50 if --sdrplay-lna-state is specified.) Disables AGC.
    --sdrplay-antenna=<str>               Select antenna port (device-specific).
    --sdrplay-hdr-mode                    (Optional) Enable HDR mode on RSPdx/RSPdxR2.
    --sdrplay-hdr-bw=<flt>                Set bandwidth for HDR mode. Requires --sdrplay-hdr-mode.

HackRF-Specific Options
    --hackrf-lna-gain=<int>               Set LNA (IF) gain in dB. (Optional, Default: 16)
    --hackrf-vga-gain=<int>               Set VGA (Baseband) gain in dB. (Optional, Default: 0)
    --hackrf-amp-enable                   Enable the front-end RF amplifier (+14 dB).

BladeRF-Specific Options
    --bladerf-device-idx=<int>            Select specific BladeRF device by index (0-indexed). (Default: 0)
    --bladerf-load-fpga=<str>             Load an FPGA bitstream from the specified file.
    --bladerf-bandwidth=<flt>             Set analog bandwidth in Hz. (Not applicable in 8-bit high-speed mode)
    --bladerf-gain=<int>                  Set overall manual gain in dB. Disables AGC.
    --bladerf-channel=<int>               For BladeRF 2.0: Select RX channel 0 (RXA) or 1 (RXB). (Default: 0)
    --bladerf-bit-depth=<int>             Set capture bit depth {8|12}. 8-bit mode is for BladeRF 2.0 only. (Default: 12, auto-switches to 8 for rates > 61.44 MHz on BladeRF 2.0)

Available Presets
    cu8-nrsc5                             Sets sample type to cu8, rate to 1488375.0 Hz for FM/AM NRSC5 decoding (produces headerless raw output).
    cs16-fm-nrsc5                         Sets sample type to cs16, rate to 744187.5 Hz for FM NRSC5 decoding (produces headerless raw output).
    cs16-am-nrsc5                         Sets sample type to cs16, rate to 46511.71875 Hz for AM NRSC5 decoding (produces headerless raw output).

Help & Version
    -v, --version                         show program's version number and exit
    -h, --help                            show this help message and exit
```

#### Examples

**Example 1: Basic File Resampling**
Resample a WAV file to a 16-bit RF64 (large WAV) file with a custom output rate.
```bash
iq_resample_tool --input wav my_capture.wav -f my_capture_resampled.wav --output-container wav-rf64 --output-sample-format cs16 --output-rate 240000
```

**Example 2: Channel Selection (FFT Filter)**
Isolate a specific range of frequencies from a live SDR stream. The tool will automatically select the `fft` filter because this is an asymmetric (offset) filter.
```bash
iq_resample_tool --input rtlsdr --sdr-rf-freq 98.5e6 --pass-range 50e3:250e3 --output-rate 240000 --stdout | ...
```

**Example 3: Piping to a Decoder with a Preset (WAV Input)**
Use the `cu8-nrsc5` preset to resample and automatically correct the frequency, then pipe it to `nrsc5`. (Assumes the WAV has frequency metadata).
```bash
iq_resample_tool --input wav my_capture.wav --wav-center-target-freq 97.3e6 --preset cu8-nrsc5 --stdout | nrsc5 -r - 0
```

**Example 4: Streaming from an SDRplay Device with Preset**
Tune an SDRplay RSPdx to 102.5 MHz, set a manual gain level and select an antenna port before piping to nrsc5.
```bash
iq_resample_tool --input sdrplay --sdr-rf-freq 102.5e6 --sdrplay-gain-level 20 --sdrplay-antenna B --preset cu8-nrsc5 --stdout | nrsc5 -r - 0
```

### Configuration via Presets

`iq_resample_tool` supports presets to save you from repeatedly typing the same output formatting options. A default `iq_resample_tool_presets.conf` is included in the repository, which you can use as a starting point for your own configurations. A preset bundles common settings like `target_rate`, `sample_format_name`, and `output_type` into a single flag (`--preset <name>`), which is perfect for common piping scenarios.

**Pro Tip:** If the tool finds config files in multiple locations, it will print a warning and load **none** of them to avoid confusion. Just delete the duplicates to fix it.

**Search Locations:**
*   **Windows:** The executable's directory, `%APPDATA%\\iq_resample_tool\\`, `%PROGRAMDATA%\\iq_resample_tool\\`
*   **Linux:** The current directory, `$XDG_CONFIG_HOME/iq_resample_tool/`, `/etc/iq_resample_tool/` , `/usr/local/etc/iq_resample_tool`

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
    *   [ ] Add Airspy & HydaSDR support (including SpyServer).
    *   [ ] Improve I/Q correction algorithm stability.
    *   [ ] Refine and standardize log levels throughout the application.
    *   [ ] General code cleanup and comment refactoring.
    *   [ ] Improve the README.

### For Developers: Architecture & Design

This section provides a high-level overview of the tool's internal design for those looking to understand, modify, or extend the codebase.

#### The Data Flow Pipeline

The tool processes data using a pipeline of dedicated threads for each major stage. Data is passed between these threads in `SampleChunk` buffers, using queues to hand off a buffer from one stage to the next.

The sequence of threads and their responsibilities are as follows:

1.  **Reader Thread:** The first thread in the pipeline acquires raw samples from the selected input source (a file or SDR). It fills a `SampleChunk` buffer with this raw data and adds it to a queue for the next stage.
    *   **SDR-to-File Mode:** When reading from a live SDR to a file, an additional `sdr_capture_thread` is used. This thread's callback writes data to an intermediate ring buffer in a structured packet format. Each packet consists of a header (containing the number of samples and format flags) followed by the corresponding sample data. This packet structure also allows for non-data events, like stream resets, to be communicated. The main Reader thread's job is to read and parse these packets from the buffer, providing a consistent stream to the rest of the pipeline regardless of the source SDR.

2.  **Pre-Processor Thread:** This thread takes raw sample buffers from the first queue. It converts the data to a 32-bit complex float format and performs any DSP operations scheduled before resampling (e.g., DC blocking).

3.  **Resampler Thread:** This thread takes the complex float buffers and changes the sample rate of the data using a filter from the `liquid-dsp` library.

4.  **Post-Processor Thread:** This thread takes the resampled buffers. It performs any DSP operations scheduled after resampling (e.g., FIR filtering) and then converts the data into the final, user-specified output byte format.

5.  **Writer Thread:** The final thread takes the formatted buffers and writes the data to the output destination.
    *   **File Output:** When writing to a file, the post-processor adds its data to a ring buffer. The writer thread reads from this buffer and writes to the disk.
    *   **Stdout Output:** When piping, data is written directly to the `stdout` stream.

#### The Modular Input System

The tool is designed to be easily extendable for new input sources (like different SDRs or file types). This is handled through a simple but powerful interface in the code.

*   **The Interface (`input_source.h`):** The core of the system is a `struct` of function pointers called `InputSourceOps`. It defines a standard contract that every input source must follow, with functions like `initialize`, `start_stream`, `stop_stream`, `cleanup`, etc.
*   **The Registry (`input_manager.c`):** This file acts as a simple "factory" or registry. It maintains a master list of all available input modules (RTL-SDR, WAV, etc.). When you run the tool with `--input rtlsdr`, the manager looks up the "rtlsdr" entry and provides the main application with the correct `InputSourceOps` struct for that device.
*   **Adding a New Source:**
    1.  Create a new `input_source.c` file that implements the required functions from the `InputSourceOps` interface.
    2.  Define any specific command-line options in that file.
    3.  Add the new module to the master list in `input_manager.c`.
    4.  Update the `CMakeLists.txt` file to compile the new file and link against the library.

This design keeps all the logic for a specific input source contained in its own file, making the code clean and easy to maintain and extend.

### Contributing

Contributions are highly welcome! Whether you've found a bug or have a cool idea for a feature, feel free to open an issue or send a pull request.

Since an AI had a heavy hand in writing this tool, AI-assisted pull requests are totally fair game. Just a heads-up: all PRs, whether from a human or a bot, will be carefully reviewed to make sure they fit the project's goals and quality standards. I look forward to seeing what you come with.
