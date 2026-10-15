# Architecture and data flow

This document describes the production firmware and Python transcription service. The application is deliberately separated into small C modules so that audio capture, utterance ownership, network delivery, and AFE processing can be reviewed independently.

## System overview

```text
INMP441 microphone (I2S RX, 32-bit slots)
                  |
                  v
       voice_afe.c / afe_feed_task
   convert microphone to PCM16; pair with
    speaker-reference PCM (or zeros when idle)
                  |
                  v
       ESP-SR AFE: AEC -> WebRTC NS -> VAD -> WakeNet
                  |
                  v
       voice_afe.c / afe_fetch_task
     wake word -> command speech -> record
       VAD silence / max duration ends capture
                  |
                  v
       audio_recorder.c / audio_queue
          fixed-pool buffer ownership
                  |
                  v
       stt_client.c / http_sender_task
     mono PCM16 -> WAV header + samples -> HTTP POST
                  |
                  v
       server/server.py (FastAPI + Faster-Whisper)
                  |
                  v
       JSON transcript and detected language
```

## Firmware modules

### `main/main.c` — startup orchestration

Initializes NVS, the LED, recording buffers, I2S interfaces, Wi-Fi and the ESP-SR AFE. It creates the playback-reference queue and starts the AFE producer, AFE consumer and HTTP sender tasks. Keep slow network processing out of the AFE tasks.

### `main/voice_aiot_internal.h` and `main/voice_aiot_state.c` — internal contract/state

The internal header declares shared types, configuration constants and cross-module functions. `voice_aiot_state.c` provides each shared variable's single definition. Avoid defining shared globals in multiple translation units.

### `main/wifi_manager.c` — Wi-Fi station lifecycle

Registers ESP-IDF Wi-Fi/IP event handlers, starts station mode and waits for an assigned IP address. The connection bit is cleared on disconnect and restored only when the station receives an IP address. The host server URL must use the PC's reachable LAN address, not loopback.

### `main/audio_io.c` — I2S and PCM conversion

- `init_i2s_microphone()` configures I2S RX for the INMP441's 32-bit slots.
- `inmp441_32_to_pcm16()` converts the significant sample bits to signed PCM16.
- `init_i2s_speaker()` configures I2S TX for the MAX98357A.
- `speaker_play_pcm16()` outputs mono PCM16 in stereo slots and queues matching mono frames for the AFE playback-reference channel. It uses a sequence index and checksum to detect reference-frame corruption/order errors.

The playback function is synchronous and can wait on I2S and queue operations. Call it from a separate task, not from the AFE feed/fetch task.

### `main/audio_recorder.c` — recording buffers and ownership

Allocates a fixed pool of maximum-duration PCM buffers in PSRAM. Its functions manage the active recording buffer, recording indicator, and transfer of completed packets to the HTTP sender.

Buffer ownership follows this sequence:

1. `acquire_record_buffer()` removes an available index from `free_buffer_queue`.
2. `append_samples()` copies enhanced PCM samples into the active buffer without exceeding `PCM_CAPACITY`.
3. `finish_recording()` sends an `audio_packet_t` descriptor to `audio_queue`; it does not copy the audio again.
4. The sender calls `release_packet_buffer()` only after its HTTP attempt is finished.
5. If queuing fails, the recorder immediately returns the buffer to the pool.

Do not return a sender-owned buffer while an HTTP request is still reading it.

### `main/voice_afe.c` — ESP-SR AFE tasks and state machine

`afe_init()` loads ESP-SR models from the `model` partition and creates the AFE in high-performance mode with AEC, WebRTC noise suppression, WakeNet and VAD enabled.

`afe_feed_task()` reads one AFE-sized frame from I2S RX, handling partial reads. It obtains the corresponding reference frame if the application is playing PCM through `speaker_play_pcm16()`; otherwise it supplies a zero reference. It converts the microphone to PCM16 and interleaves the channels as `M0, R0, M1, R1, ...` before calling ESP-SR `feed()`.

`afe_fetch_task()` consumes the AFE result and enforces one utterance per wake-word detection. It separates the wake phrase from command speech, appends enhanced PCM to the active recording buffer, ends the utterance after the configured VAD silence period or maximum duration, and hands the recording to the queue.

Keep both tasks real-time friendly: avoid HTTP operations, file I/O, and large per-frame allocations in these loops.

### `main/stt_client.c` — WAV assembly and HTTP client

`http_sender_task()` blocks on `audio_queue`. The sender builds a standard WAV header for mono, 16 kHz, signed PCM16, posts the header and samples to `APP_SERVER_URL`, logs the server response, and prints the transcript/language. The buffer index returns to the pool after the request attempt regardless of success.

### Configuration and component files

- `main/Kconfig.projbuild` exposes application settings in `idf.py menuconfig`.
- `main/CMakeLists.txt` registers source files and ESP-IDF component dependencies.
- `main/idf_component.yml` declares the managed `esp-sr` and `cjson` dependencies.
- `sdkconfig.defaults` provides non-secret target, flash, PSRAM, CPU and model defaults.
- `partitions.csv` reserves flash for the firmware and ESP-SR models.

## Python service

`server/server.py` starts a FastAPI application and loads the Faster-Whisper `base` model once per process.

- `GET /health` returns readiness and model metadata.
- `POST /transcribe` accepts raw WAV bytes and responds with `status`, `text`, `language`, `duration`, and `processing_time`.

The service uses CPU inference with `int8` compute and automatic language detection. `vad_filter=False` is intentional because ESP-SR VAD already delimits the utterance. The project returns transcription in the spoken language; translation, command execution, answer generation, and TTS are outside its current scope.

## AEC reference-channel constraint

AEC estimates the device speaker signal in microphone input and subtracts its echo. This requires the same PCM sent to the physical speaker to be provided to AFE channel R with correct timing. The application uses the `MR` channel layout: microphone on M and playback reference on R. During ordinary STT-only operation the speaker is silent and R is zero-filled. If speaker playback is added to the application, route that playback through `speaker_play_pcm16()` or otherwise guarantee that the exact playback PCM is supplied to the reference channel.
