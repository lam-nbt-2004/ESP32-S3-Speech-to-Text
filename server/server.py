"""HTTP speech-to-text service used by the ESP32-S3 firmware.

POST /transcribe accepts raw PCM WAV bytes and returns JSON containing the
recognized text and language detected by Whisper. GET /health is a lightweight
startup/connectivity check.

This service transcribes in the spoken language; it does not translate,
generate answers, or synthesize speech in this project stage.
"""

import io
import time
from typing import Any

from fastapi import FastAPI, HTTPException, Request
from faster_whisper import WhisperModel

app = FastAPI(title="ESP32-S3 Speech-to-Text Server")

MODEL_NAME = "base"
print(f"Loading faster-whisper model: {MODEL_NAME}", flush=True)
model = WhisperModel(MODEL_NAME, device="cpu", compute_type="int8")
print("Speech-to-text model is ready.", flush=True)


@app.get("/health")
def health() -> dict[str, Any]:
    """Return a small readiness response for local/server connectivity tests."""
    return {"status": "ok", "model": MODEL_NAME, "task": "transcribe", "language": "auto"}


@app.post("/transcribe")
async def transcribe_audio(request: Request) -> dict[str, Any]:
    """Transcribe the raw WAV request body and return a compact JSON result."""
    audio_bytes = await request.body()
    if not audio_bytes:
        raise HTTPException(status_code=400, detail="Empty audio body")

    content_type = request.headers.get("content-type", "")
    print(
        f"[STT] Received {len(audio_bytes)} bytes, Content-Type={content_type}",
        flush=True,
    )

    # Faster-Whisper segment output is a generator; consuming it inside this
    # try block ensures inference errors are converted into an HTTP 500 response.
    started = time.perf_counter()
    try:
        segments, info = model.transcribe(
            io.BytesIO(audio_bytes),
            task="transcribe",
            language=None,       # automatic language detection (English/Vietnamese)
            beam_size=5,
            vad_filter=False,    # ESP-SR VAD already segments the utterance
            temperature=0.0,
        )
        text = "".join(segment.text for segment in segments).strip()
    except Exception as exc:
        print(f"[STT] Error: {type(exc).__name__}: {exc}", flush=True)
        raise HTTPException(status_code=500, detail="Speech recognition failed") from exc

    elapsed = time.perf_counter() - started
    language = getattr(info, "language", None) or "unknown"
    print(
        f"[STT] language={language}, duration={getattr(info, 'duration', None)}, "
        f"elapsed={elapsed:.2f}s",
        flush=True,
    )
    print(f'[STT] transcript: {text!r}', flush=True)

    return {
        "status": "success",
        "text": text,
        "language": language,
        "duration": getattr(info, "duration", None),
        "processing_time": elapsed,
    }


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host="0.0.0.0", port=8000)
