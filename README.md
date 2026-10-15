# ESP32-S3 AIoT Voice Control

An ESP-IDF application for an **ESP32-S3-N16R8** that listens for the **“Hi ESP”** wake word, captures one spoken command, and sends the recorded audio to a Python speech-to-text service. The firmware uses Espressif's ESP-SR Audio Front End (AFE) with Acoustic Echo Cancellation (AEC), WebRTC noise suppression, Voice Activity Detection (VAD), and WakeNet.

> **Current scope:** speech recognition only. The server returns recognized text and language; this version does not interpret commands, generate answers, or speak a response. The MAX98357A output and synchronized AEC reference path are available for application playback, but normal startup does not play audio.

## Features

- Wake-word activation using the configured ESP-SR WakeNet model (`wn9_hiesp` / “Hi ESP”).
- INMP441 I2S microphone capture at 16 kHz.
- ESP-SR AFE with AEC, WebRTC noise suppression, VAD, and WakeNet.
- One utterance per wake word, ending after VAD-detected silence or the configured duration limit.
- WAV upload to a FastAPI endpoint running Faster-Whisper on the host computer.
- Optional MAX98357A playback API that queues the same PCM samples as the AEC playback reference.

## Hardware

The documented defaults target an ESP32-S3-N16R8 board with 16 MB flash and 8 MB PSRAM.

| Module / signal | ESP32-S3 GPIO | Notes |
|---|---:|---|
| INMP441 BCLK | GPIO 14 | I2S bit clock |
| INMP441 WS / LRCLK | GPIO 15 | I2S word select |
| INMP441 DOUT | GPIO 16 | Microphone data into ESP32 |
| MAX98357A BCLK | GPIO 17 | Optional speaker playback |
| MAX98357A LRC / WS | GPIO 18 | Optional speaker playback |
| MAX98357A DIN | GPIO 8 | Optional speaker playback |
| Status LED | GPIO 2 | High while recording a command |

Check the pinout of your exact development board and amplifier module before wiring. Connect the INMP441 L/R pin to the level that matches the selected I2S slot; the default is **LEFT**. Speaker wiring is optional for wake-word/STT operation, but is required when using the speaker/AEC-reference playback path. Never connect a speaker output directly to an ESP32 GPIO.

## Prerequisites

### Firmware tools

- **ESP-IDF v6.1** with its toolchain and Python environment installed and exported for your terminal. Use the [official ESP-IDF installation guide for ESP32-S3](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/index.html).
- Git, if cloning the repository.
- A USB data cable and the correct serial/COM port for your ESP32-S3.
- A 2.4 GHz Wi-Fi network that the board can join.

ESP-IDF Component Manager resolves the firmware dependencies declared in `main/idf_component.yml`, including `espressif/esp-sr` and `espressif/cjson`, during configuration/build.

### Python speech-to-text server

- Python **3.10 or newer** (64-bit recommended).
- Internet access on first setup to install Python packages and download the Faster-Whisper model.
- A PC reachable from the ESP32 over the local network. Allow inbound TCP port `8000` through the host firewall on a trusted/private network if required.

## 1. Get the source

Clone the repository or download its ZIP, then open an ESP-IDF terminal in the project root (the folder containing the top-level `CMakeLists.txt`). For example:

```powershell
git clone <YOUR_REPOSITORY_URL>
cd rec
```

If you downloaded a ZIP, extract it and `cd` into the extracted project folder instead.

## 2. Configure the firmware

Set the ESP32-S3 target, open menuconfig, and configure the board locally:

```powershell
idf.py set-target esp32s3
idf.py menuconfig
```

Under **AIoT Voice Control**, configure at least:

- **Wi-Fi SSID** and **Wi-Fi password** for your network.
- **Speech-to-text server URL**, for example `http://192.168.1.50:8000/transcribe`.
- Microphone/speaker GPIOs if your wiring differs from the table.
- Optional recording duration, silence endpoint, and HTTP timeout settings.

Use the host computer's LAN IPv4 address in `APP_SERVER_URL`, not `127.0.0.1`: from the ESP32, loopback refers to the ESP32 itself. On Windows, `ipconfig` shows the IPv4 address of the active Wi-Fi/Ethernet adapter. The server and ESP32 must be able to reach one another over the network.

`sdkconfig.defaults` contains non-secret defaults for the ESP32-S3, 16 MB flash, octal PSRAM, 240 MHz CPU, custom partition table, and selected ESP-SR models. ESP-IDF generates a local `sdkconfig` after configuration. That generated file is intentionally not included in this repository because it may contain credentials and machine-specific choices.

## 3. Build and flash the firmware

With ESP-IDF v6.1 active and your board connected:

```powershell
idf.py reconfigure
idf.py build
idf.py -p COM6 flash monitor
```

Replace `COM6` with the serial port on your computer. On Linux/macOS, use the appropriate serial device path, such as `/dev/ttyACM0` or `/dev/ttyUSB0`.

The custom `partitions.csv` reserves a dedicated `model` partition for ESP-SR assets. The ESP-IDF/ESP-SR build process must populate that partition with the selected models; the project flash arguments generated during a successful build include the model image. If AFE model loading fails at boot, verify that the model partition is present and that `wn9_hiesp` is selected and included by your installed ESP-SR component.

## 4. Install and run the speech-to-text service

Open a **second terminal** in the project root. Create a virtual environment and install the server dependencies:

```powershell
python -m venv server\.venv
server\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install -r server\requirements.txt
python server\server.py
```

On Linux/macOS, activate the environment with:

```bash
source server/.venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -r server/requirements.txt
python server/server.py
```

The first start loads the Faster-Whisper `base` model and may download model files. The current service uses CPU inference with `int8` compute. Keep this terminal open while testing the board. Uvicorn listens on `0.0.0.0:8000`, allowing a LAN client to connect when the host firewall permits it.

Check the service from another terminal:

```powershell
Invoke-RestMethod http://127.0.0.1:8000/health | ConvertTo-Json
```

The response should contain `"status": "ok"`. You can also open `http://127.0.0.1:8000/docs` in a browser to view the FastAPI-generated API documentation.

## 5. Use the voice-control pipeline

1. Start the Python server and wait for `Speech-to-text model is ready.`
2. Flash the firmware and open the ESP-IDF serial monitor.
3. Say **“Hi ESP”** clearly toward the INMP441 microphone.
4. After the wake word is accepted, speak one short command or sentence.
5. Pause and wait for the VAD silence timeout. The device should queue the captured audio and POST it to `/transcribe`.
6. Confirm the serial log shows an HTTP `200` response and an `STT transcript (...)` line. The server terminal logs the recognized language and transcript as well.

Each new utterance requires the wake word again. Language detection is automatic, and the output is transcription in the spoken language; translation and command execution are not implemented in this stage.

## Configuration reference

These settings are available through `idf.py menuconfig` under **AIoT Voice Control**.

| Setting | Purpose | Default |
|---|---|---:|
| `APP_SERVER_URL` | Python `/transcribe` endpoint | `http://192.168.1.100:8000/transcribe` |
| `APP_MIC_BCLK_GPIO` | INMP441 bit clock | `14` |
| `APP_MIC_WS_GPIO` | INMP441 word select | `15` |
| `APP_MIC_DOUT_GPIO` | INMP441 data | `16` |
| `APP_SPK_BCLK_GPIO` | MAX98357A bit clock | `17` |
| `APP_SPK_WS_GPIO` | MAX98357A word select | `18` |
| `APP_SPK_DOUT_GPIO` | MAX98357A data | `8` |
| `APP_LED_GPIO` | Recording indicator LED | `2` |
| `APP_MAX_RECORD_SEC` | Maximum utterance duration | `10 s` |
| `APP_SILENCE_MS` | Silence required to end a recording | `1200 ms` |
| `APP_COMMAND_START_TIMEOUT_MS` | Allowed delay to begin a command after wake word | `4000 ms` |
| `APP_VAD_MIN_SPEECH_MS` | AFE VAD minimum speech duration | `128 ms` |
| `APP_VAD_MIN_NOISE_MS` | AFE VAD minimum noise duration | `1000 ms` |
| `APP_VAD_DELAY_MS` | AFE VAD delay/cache | `256 ms` |
| `APP_HTTP_TIMEOUT_MS` | HTTP request timeout | `15000 ms` |

To apply menuconfig changes, save and exit, then run `idf.py reconfigure` and `idf.py build`.

## Project structure

```text
rec/
├── CMakeLists.txt              # ESP-IDF project entry point
├── dependencies.lock           # Resolved ESP-IDF component versions
├── partitions.csv              # Application and ESP-SR model partitions
├── sdkconfig.defaults          # Non-secret ESP32-S3 defaults
├── main/
│   ├── main.c                  # Startup and task creation
│   ├── Kconfig.projbuild       # Project settings shown in menuconfig
│   ├── CMakeLists.txt          # Firmware source/component registration
│   ├── idf_component.yml       # ESP-SR and cJSON dependencies
│   ├── voice_aiot_internal.h   # Private types, constants, and interfaces
│   ├── voice_aiot_state.c      # Shared cross-task state definitions
│   ├── wifi_manager.c          # Wi-Fi station and reconnect handling
│   ├── audio_io.c              # I2S microphone/speaker and reference PCM
│   ├── audio_recorder.c        # Recording buffers, LED, and queue handoff
│   ├── voice_afe.c             # AEC/NS, WakeNet/VAD, feed/fetch tasks
│   ├── stt_client.c             # WAV formatting and HTTP STT client
│   └── speaker.h               # Speaker playback API
├── server/
│   ├── server.py               # FastAPI + Faster-Whisper endpoint
│   └── requirements.txt        # Python server dependencies
└── docs/
    └── ARCHITECTURE.md         # Module responsibilities and data flow
```

Generated output such as `build/`, `managed_components/`, `sdkconfig`, local virtual environments, bytecode, and private audio captures should not be committed. `.gitignore` excludes these local artifacts.

## Troubleshooting

| Symptom | Checks |
|---|---|
| Wi-Fi never connects | Confirm the SSID/password in menuconfig, network availability, and 2.4 GHz support. |
| HTTP connection fails | Confirm the PC's LAN IP in `APP_SERVER_URL`, that the server is running on port 8000, and that firewall rules allow the connection. |
| `/health` works on the PC but not from ESP32 | Use the PC's LAN IP rather than `127.0.0.1`; verify both devices are on a reachable network. |
| Wake word is not detected | Confirm `wn9_hiesp` is available in the model partition, the microphone pinout and LEFT/RIGHT slot selection are correct, and the room is quiet enough. |
| Transcript is empty or inaccurate | Check microphone placement and input levels, reduce background noise, speak after the wake-word prompt, and review the server's `[STT]` logs. |
| AFE fails to initialize | Review the serial boot log, ESP-SR model selection, partition table, PSRAM settings, and resolved ESP-SR component version. |
| Speaker playback reports reference faults | Use `speaker_play_pcm16()` so the same PCM is queued as the AEC reference; don't play unrelated audio through the speaker without providing that audio to the reference channel. |

## Architecture notes

AEC is configured in the ESP-SR AFE, with input format `MR`: channel 0 is the microphone and channel 1 is the speaker playback reference. During ordinary STT-only operation there is no speaker playback, so the reference channel is zero-filled. When an application calls `speaker_play_pcm16()`, it queues the corresponding mono PCM frames for the AFE along with I2S speaker output. Accurate cancellation depends on feeding the actual playback PCM with appropriate timing.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the module-level data flow and buffer-ownership rules.
