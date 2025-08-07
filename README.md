# iq_resample_tool

A fast, multi-threaded command-line tool for resampling, shifting, and correcting I/Q data streams.

I originally built this tool for a very specific need: processing NRSC-5 (HD Radio) captures for use with the awesome `nrsc5` decoder. This means it's particularly good at handling the frequency shifts and metadata found in WAV files from SDR software, but it has grown into a more general-purpose utility for prepping I/Q data for any downstream tool.

---

### ⚠️  A Quick Word of Warning: This is an AI-Assisted Project ⚠️

Let's be upfront: a large language model (AI) helped write a significant portion of this code. *I guided it, reviewed its output, and tested the result, but this project didn't evolve through the typical trial-and-error of a human-only endeavor. Even this README you're reading was drafted by the AI based on the source code, then edited and refined by me.*

*What does this mean for you?*

*   **It's still experimental.** While it works, it hasn't been battle-tested across a wide variety of systems and edge cases.
*   **Bugs are likely lurking.** The logic likely has  quirks that haven't been discovered yet.
*   **Use with caution.** I wouldn't use this for anything mission-critical without a thorough personal review of the code. *For serious work, a mature framework like* [GNU Radio](https://github.com/gnuradio/gnuradio) *is always a better bet.*

---

### What It Can Do

*   **Fast, Multi-Threaded Pipeline:** Designed to chew through data on multi-core systems using a reader -> processor -> writer model.
*   **Flexible Inputs:**
    *   **WAV Files:** Reads standard 8-bit and 16-bit complex (I/Q) WAV files.
    *   **Raw I/Q Files:** Just point it at a headerless file, but you have to tell it the sample rate and format.
    *   **SDR Hardware:** Streams directly from **SDRplay** and **HackRF** devices.
*   **Intelligent Metadata Parsing:** Automatically reads metadata from various sources to make your life easier, especially for frequency correction.
    *   `auxi` chunks from **SDR Console, SDRconnect,** and **SDRuno**.
    *   SDR# style filenames (e.g., `..._20240520_181030Z_97300000Hz_...`).
*   **Powerful Processing:**
    *   **High-Quality Resampling:** Uses `liquid-dsp` under the hood.
    *   **Precise Frequency Shifting:** Apply shifts before or after resampling—handy for those weird, narrow I/Q captures.
    *   **Automatic I/Q Correction:** Can optionally find and fix I/Q imbalance on the fly. *This is very experimental and being refined. It may make things worse currently.* 
    *   **DC Blocking:** A simple filter to remove the pesky DC offset.
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
*   **(Optional) SDRplay API Library:** To build with SDRplay support, you must first download and install the official API from the **[SDRplay website](https://www.sdrplay.com/downloads/)**.
*   **(Optional) HackRF Library (libhackrf) and libusb:** To build with HackRF support, you'll need the host libraries (e.g., `libhackrf-dev` and `libusb-1.0-0-dev` on Debian/Ubuntu).

#### On Linux (Debian/Ubuntu Example)

1.  **Install the boring stuff:**
    ```bash
    sudo apt-get update
    sudo apt-get install build-essential cmake libsndfile1-dev libliquid-dev libexpat1-dev libhackrf-dev libusb-1.0-0-dev
    ```

2.  **Build the tool:**
    ```bash
    git clone https://github.com/pclov3r/iq_resample_tool.git
    cd iq_resample_tool
    mkdir build && cd build

    # Build without SDR support
    cmake ..

    # Or, build with everything enabled
    cmake -DWITH_SDRPLAY=ON -DWITH_HACKRF=ON ..

    make
    ```
You'll find the `iq_resample_tool` executable in the `build` directory.

#### On Windows

Cross-compiling with MinGW-w64 or building in MSYS2 is your best bet. The repo includes a `support/win-cross-compile` script to help with this, but **you will need to edit the hardcoded paths** in that script to match your environment.

The script builds different versions optimized for specific CPU features (AVX, AVX2). Make sure you run the one that matches your hardware, or it will crash!

### How to Use It

The best way to see all options is to run `iq_resample_tool --help`.

#### Command-Line Options

```text
Usage: iq_resample_tool -i {wav <file_path> | raw-file <file_path> | sdrplay | hackrf} {--file <path> | --stdout} [options]

Description:
  Resamples an I/Q file or a stream from an SDR device to a specified format and sample rate.

Required Input:
  -i, --input <type>                 Specifies the input type. Must be one of:
                                       wav:      Input from a WAV file specified by <file_path>.
                                       raw-file: Input from a headerless file of raw I/Q samples specified by <file_path>.
                                       sdrplay:  Input from a SDRplay device.
                                       hackrf:   Input from a HackRF device.

Output Destination (Required, choose one):
  -f, --file <file>                  Output to a file.
  -o, --stdout                       Output binary data for piping to another program.

Output Options:
  --output-container <type>          Specifies the output file container format.
                                       Defaults to 'wav-rf64' for file output, 'raw' for stdout.
                                       raw:      Headerless, raw I/Q sample data.
                                       wav:      Standard WAV format (max 4GB, limited sample rates).
                                       wav-rf64: RF64/BW64 format for large files and high sample rates.

  --output-sample-format <format>    Sample format for output data. (Defaults to cs16 for file output).
                                       cs8, cu8, cs16, cu16, cs32, cu32, cf32
                                       (Not all formats are compatible with WAV containers).

WAV Input Specific Options (Only valid with '--input wav'):
  --wav-center-target-frequency <hz> Shift signal to a new target center frequency (e.g., 97.3e6).
                                       (Recommended for WAV captures with frequency metadata).
  --wav-shift-frequency <hz>         Apply a direct frequency shift in Hz.
                                       (Use if WAV input lacks metadata or for manual correction).
  --wav-shift-after-resample         Apply frequency shift AFTER resampling (default is before).
                                       (A workaround for narrow I/Q WAV recordings where only a single
                                        HD sideband is present).

Raw File Input Options (Only valid with '--input raw-file'):
  --raw-file-input-rate <hz>         (Required) The sample rate of the raw input file.
  --raw-file-input-sample-format <format> (Required) The sample format of the raw input file.
                                       Valid formats: cs8, cu8, cs16, cu16, cs32, cu32, cf32
                                       (File is assumed to be 2-channel interleaved I/Q data).

SDR Options (Only valid when using an SDR input):
  --rf-freq <hz>                     (Required) Tuner center frequency in Hz (e.g., 97.3e6).
  --bias-t                           (Optional) Enable Bias-T power.

SDRplay-Specific Options (Only valid with '--input sdrplay'):
  --sdrplay-sample-rate <hz>         Set sample rate in Hz. (Optional, Default: 2e6).
                                       Valid range: 2e6 to 10e6.
  --sdrplay-bandwidth <hz>           Set analog bandwidth in Hz. (Optional, Default: 1.536e6).
                                       Valid values: 200e3, 300e3, 600e3, 1.536e6, 5e6, 6e6, 7e6, 8e6.
  --sdrplay-device-idx <IDX>         Select specific SDRplay device by index (0-indexed). (Default: 0).
  --sdrplay-gain-level <LEVEL>       Set manual gain level (0=min gain). Disables AGC.
                                       Max level varies by device/freq (e.g., RSP1A: 0-9, RSPdx @100MHz: 0-27).
  --sdrplay-antenna <PORT>           Select antenna port (device-specific).
                                       RSPdx/R2: A, B, C | RSP2: A, B, HIZ | RSPduo: A, HIZ
                                       (Not applicable for RSP1, RSP1A, RSP1B).
  --sdrplay-hdr-mode                 (Optional) Enable HDR mode on RSPdx/RSPdxR2.
  --sdrplay-hdr-bw <BW_MHZ>          Set bandwidth for HDR mode. Requires --sdrplay-hdr-mode. (Default: 1.7).
                                       Valid values: 0.2, 0.5, 1.2, 1.7.

HackRF-Specific Options (Only valid with '--input hackrf'):
  --hackrf-sample-rate <hz>          Set sample rate in Hz. (Optional, Default: 8e6).
                                       Valid range is 2-20 Msps (e.g., 2e6, 10e6, 20e6).
                                       Automatically selects a suitable baseband filter.
  --hackrf-lna-gain <db>             Set LNA (IF) gain in dB. (Optional, Default: 16).
                                       Valid values: 0-40 in 8 dB steps (e.g., 0, 8, 16, 24, 32, 40).
  --hackrf-vga-gain <db>             Set VGA (Baseband) gain in dB. (Optional, Default: 0).
                                       Valid values: 0-62 in 2 dB steps (e.g., 0, 2, 4, ... 62).
  --hackrf-amp-enable                Enable the front-end RF amplifier (+14 dB).

Processing Options:
  --output-rate <hz>                 Output sample rate in Hz. (Required if no preset is used).
                                       (Cannot be used with --preset or --no-resample).

  --gain <multiplier>                Apply a linear gain multiplier to the samples (Default: 1.0).

  --no-resample                      Disable the resampler (passthrough mode).
                                       Output sample rate will be the same as the input rate.

  --iq-correction                    (Optional) Enable automatic I/Q imbalance correction.

  --dc-block                         (Optional) Enable DC offset removal (high-pass filter).

  --preset <name>                    Use a preset for a common target.
                                       (Cannot be used with --no-resample).
                                       Loaded presets:
                                         cu8-nrsc5:        Sets sample type to cu8, rate to 1488375.0 Hz for FM/AM NRSC5 decoding (produces headerless raw output).
                                         cs16-fm-nrsc5:    Sets sample type to cs16, rate to 744187.5 Hz for FM NRSC5 decoding (produces headerless raw output).
                                         cs16-am-nrsc5:    Sets sample type to cs16, rate to 46511.71875 Hz for AM NRSC5 decoding (produces headerless raw output).
```

#### Examples

**Example 1: Basic File Resampling**
Resample a WAV file to a 16-bit RF64 (large WAV) file with a custom output rate.
```bash
iq_resample_tool --input wav --file my_capture_resampled.wav --output-container wav-rf64 --output-sample-format cs16 --output-rate 240000
```

**Example 2: Piping to a Decoder with a Preset (WAV Input)**
Use the `cu8-nrsc5` preset to resample and automatically correct the frequency, then pipe it to `nrsc5`. (Assumes the WAV has frequency metadata).
```bash
iq_resample_tool --input wav --wav-center-target-frequency 97.3e6 --preset cu8-nrsc5 --stdout | nrsc5 -r - 0
```

**Example 3: Streaming from a HackRF Device with Preset**
Tune a HackRF to 98.5 MHz, set LNA and VGA gain, and pipe the output to nrsc5 using the `cu8-nrsc5` preset.
```bash
iq_resample_tool -i hackrf --rf-freq 98.5e6 --hackrf-lna-gain 24 --hackrf-vga-gain 16 --preset cu8-nrsc5 --stdout | nrsc5 -r - 0
```

**Example 4: Streaming from an SDRplay Device with Preset**
Tune an SDRplay RSPdx to 102.5 MHz, set a manual gain level and select a specific antenna port before piping to nrsc5 using the `cu8-nrsc5` preset.
```bash
iq_resample_tool -i sdrplay --rf-freq 102.5e6 --sdrplay-gain-level 20 --sdrplay-antenna B --preset cu8-nrsc5 --stdout | nrsc5 -r - 0
```

**Example 5: Manual Frequency Correction (WAV Input)**
Apply a direct -400 kHz frequency shift to a file that lacks metadata, using the `cu8-nrsc5` preset.
```bash
iq_resample_tool --input wav capture_no_meta.wav --wav-shift-frequency -400e3 --preset cu8-nrsc5 --stdout | nrsc5 -r - 0
```

**Example 6: Workaround for Narrow Sideband Recordings (WAV Input)**
Use the `--wav-shift-after-resample` flag to process a narrow HD I/Q recording where the desired signal is off-center after resampling, using the `cu8-nrsc5` preset.
```bash
iq_resample_tool --input wav narrow_capture.wav --wav-center-target-frequency 97.3e6 --preset cu8-nrsc5 --wav-shift-after-resample --stdout | nrsc5 -r - 0
```

### Configuration via Presets

`iq_resample_tool` supports presets to save you from repeatedly typing the same output formatting options. A default `iq_resample_tool_presets.conf` is included in the repository, which you can use as a starting point for your own configurations. A preset bundles common settings like `target_rate`, `sample_format_name`, and `output_type` into a single flag (`--preset <name>`), which is perfect for common piping scenarios.

**Pro Tip:** If the tool finds config files in multiple locations, it will print a warning and load **none** of them to avoid confusion. Just delete the duplicates to fix it.

**Search Locations:**
*   **Windows:** The executable's directory, `%APPDATA%\iq_resample_tool\`, `%PROGRAMDATA%\iq_resample_tool\`
*   **Linux:** The current directory, `$XDG_CONFIG_HOME/iq_resample_tool/`, `/etc/iq_resample_tool/`

### Current State & Future Plans

This tool is a work in progress.

*   **Known Issues:**
    *   It's experimental. Expect bugs.
    *   Windows builds are 64-bit only. I see no reason to post 32-bit ones given Windows 10 is end of life soon and Windows 11 is 64-bit only. If I'm wrong and it's required open an issue.
    *   As mentioned, IQ correction may not be functioning correctly. 

*   **Roadmap:**
    *   [ ] Add RTL-SDR support.
    *   [ ] Add Airspy support and HydraSDR (if I can get my hands on the hardware).
    *   [ ] Add BladeRF support.
    *   [ ] General code cleanup and comment refactoring.

### Contributing

Contributions are highly welcome! Whether you've found a bug or have a cool idea for a feature, feel free to open an issue or send a pull request.

Since an AI had a heavy hand in writing this tool, AI-assisted pull requests are totally fair game. Just a heads-up: all PRs, whether from a human or a bot, will be carefully reviewed to make sure they fit the project's goals and quality standards. I look forward to seeing what you come up with!
