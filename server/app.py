"""Mono Note Mini - local backend.

Speaks the exact contract the firmware already expects:

    POST {api}/transcribe    multipart, field "audio"  ->  {"text": "..."}

so pointing the device at this machine needs no firmware change - just put
http://<this-pc>:8000 in the device's API field.

Everything runs on your own hardware. Nothing is sent anywhere.

    python app.py                 # http://0.0.0.0:8000
    WHISPER_MODEL=large-v3 python app.py
"""
from __future__ import annotations

import glob
import json
import os
from pathlib import Path
import shutil
import tempfile
import time
from typing import Any

import httpx
from fastapi import FastAPI, File, Form, HTTPException, UploadFile
from fastapi.middleware.cors import CORSMiddleware
from fastapi import Response
from fastapi.responses import FileResponse, JSONResponse
from pydantic import BaseModel

# On Windows the CUDA runtime ships inside the nvidia-* wheels rather than on
# PATH, so CTranslate2 fails with "cublas64_12.dll is not found" unless these
# directories are registered first. Must happen before ctranslate2 is imported.
def _register_cuda_dlls() -> list[str]:
    found = []
    try:
        import nvidia
    except ImportError:
        return found
    # nvidia is a namespace package, so __file__ is None - walk __path__
    roots = list(getattr(nvidia, "__path__", []))
    dlls = [q for root in roots
            for q in glob.glob(os.path.join(root, "**", "bin", "*.dll"), recursive=True)]
    for d in sorted({os.path.dirname(q) for q in dlls}):
        try:
            os.add_dll_directory(d)
            found.append(d)
        except (OSError, AttributeError):
            pass
    if found:
        os.environ["PATH"] = os.pathsep.join(found) + os.pathsep + os.environ.get("PATH", "")
    return found


CUDA_DLL_DIRS = _register_cuda_dlls()

# Keep model downloads off the system drive unless told otherwise - they are
# hundreds of MB each and C: is usually the small one.
os.environ.setdefault("HF_HOME", os.getenv("MNM_CACHE", r"T:\hf-cache"))
os.environ.setdefault("HF_HUB_DISABLE_SYMLINKS_WARNING", "1")

# --------------------------------------------------------------------------- config
WHISPER_MODEL = os.getenv("WHISPER_MODEL", "small.en")
WHISPER_DEVICE = os.getenv("WHISPER_DEVICE", "auto")      # auto | cuda | cpu
OLLAMA_URL = os.getenv("OLLAMA_URL", "http://127.0.0.1:11434")
OLLAMA_MODEL = os.getenv("OLLAMA_MODEL", "gemma3:12b")
API_KEY = os.getenv("API_KEY", "")                         # optional shared secret

# The device's own tag set. The LLM must pick from these and nothing else,
# or the tag will not match anything the firmware can display.
TAGS = ["Work", "Projects", "Ideas", "Quotes", "Random"]

app = FastAPI(title="Mono Note Mini backend", version="1.0")

# The device serves its own web page, so that page lives on a different origin
# to this server and cannot call /enrich without these headers. Open by default
# because this is a LAN tool; set ALLOW_ORIGINS to lock it down.
app.add_middleware(
    CORSMiddleware,
    allow_origins=[o for o in os.getenv("ALLOW_ORIGINS", "*").split(",") if o],
    allow_methods=["GET", "POST"],
    allow_headers=["*"],
    # The web app is hosted on GitHub Pages over HTTPS while this server is on
    # localhost. Chrome's Private Network Access rules block a public origin
    # from reaching a private one unless the server says it expects it, and
    # Starlette refuses those preflights outright without this - answering
    # "Disallowed CORS private-network", which reaches the browser as a plain
    # CORS failure with nothing in it pointing at the real cause.
    allow_private_network=True,
)

_model = None
_model_device = "unloaded"


# --------------------------------------------------------------------------- whisper
def _pick_device() -> tuple[str, str]:
    """Returns (device, compute_type). Falls back to CPU rather than dying:
    an RTX 50-series card is sm_120, and a CTranslate2 built before CUDA 12.8
    has no kernels for it - it raises at load time rather than at install."""
    if WHISPER_DEVICE == "cpu":
        return "cpu", "int8"
    try:
        import ctranslate2

        if ctranslate2.get_cuda_device_count() > 0:
            return "cuda", "float16"
    except Exception:
        pass
    return "cpu", "int8"


def get_model():
    global _model, _model_device
    if _model is not None:
        return _model

    from faster_whisper import WhisperModel

    device, compute = _pick_device()
    try:
        _model = WhisperModel(WHISPER_MODEL, device=device, compute_type=compute)
        _model_device = f"{device}/{compute}"
    except Exception as exc:
        if device == "cpu":
            raise
        # Most likely an unsupported compute capability - say so plainly and
        # carry on rather than leaving the device with a dead endpoint.
        print(f"[whisper] cuda load failed ({exc}); falling back to CPU")
        _model = WhisperModel(WHISPER_MODEL, device="cpu", compute_type="int8")
        _model_device = "cpu/int8 (cuda unavailable)"
    print(f"[whisper] {WHISPER_MODEL} on {_model_device}")
    return _model


def transcribe_file(path: str) -> tuple[str, float]:
    model = get_model()
    segments, info = model.transcribe(path, beam_size=5, vad_filter=True)
    text = " ".join(s.text.strip() for s in segments).strip()
    return text, float(info.duration or 0.0)


# --------------------------------------------------------------------------- routes
def _check_key(key: str | None) -> None:
    if API_KEY and key != API_KEY:
        raise HTTPException(status_code=401, detail="bad or missing key")


@app.get("/", include_in_schema=False)
async def serve_app():
    """Serve the web app from this server.

    Transcription needs this machine, so the page that uses it may as well come
    from this machine. That removes an entire class of problem rather than
    working around it: a page on https://...github.io is forbidden by Chrome
    from calling http://localhost at all - ERR_BLOCKED_BY_CLIENT, decided
    before any CORS or Private Network Access header is even looked at.

    Served from here the page is same-origin with the API, so nothing is
    cross-origin and nothing can be blocked. http://localhost is also a secure
    context in its own right, so Web Bluetooth still works - which is the part
    that would have been lost by dropping to plain HTTP anywhere else.

    The hosted copy on GitHub Pages keeps its own job: reading notes that have
    been synced to a repo, from anywhere, with no server running at all.
    """
    page = Path(__file__).resolve().parent.parent / "index.html"
    if not page.exists():
        raise HTTPException(status_code=404, detail="index.html not found beside the server")
    # Never cache the page. The app is edited constantly, and a browser holding
    # an older copy produces errors from code that no longer exists - which
    # sends you looking for a bug in the current source that is not there.
    return FileResponse(page, media_type="text/html", headers={
        "Cache-Control": "no-store, no-cache, must-revalidate, max-age=0",
        "Pragma": "no-cache",
    })


@app.get("/health")
async def health() -> dict[str, Any]:
    cuda = None
    try:
        import ctranslate2

        cuda = ctranslate2.get_cuda_device_count()
    except Exception:
        cuda = "ctranslate2 not importable"
    return {
        "ok": True,
        "whisper_model": WHISPER_MODEL,
        "whisper_device": _model_device,
        "cuda_devices": cuda,
        "cuda_dll_dirs": len(CUDA_DLL_DIRS),
        "ollama_model": OLLAMA_MODEL,
        "tags": TAGS,
    }


@app.post("/transcribe")
async def transcribe(
    audio: UploadFile | None = File(default=None),
    file: UploadFile | None = File(default=None),
    f: UploadFile | None = File(default=None),
    key: str | None = Form(default=None),
):
    """The firmware sends multipart field "audio"; the other two names are
    accepted so curl and the browser page work without special-casing."""
    _check_key(key)
    up = audio or file or f
    if up is None:
        raise HTTPException(status_code=400, detail="no audio file - use field 'audio'")

    tmp = tempfile.NamedTemporaryFile(suffix=".wav", delete=False)
    try:
        shutil.copyfileobj(up.file, tmp)
        tmp.close()
        started = time.time()
        text, seconds = transcribe_file(tmp.name)
        took = time.time() - started
        print(f"[transcribe] {up.filename} {seconds:.1f}s audio in {took:.1f}s "
              f"({seconds/took if took else 0:.1f}x) -> {len(text)} chars")
        # "text" is the first key the firmware looks for
        return {"text": text, "seconds": round(seconds, 2),
                "took": round(took, 2), "device": _model_device}
    finally:
        try:
            os.unlink(tmp.name)
        except OSError:
            pass


class EnrichIn(BaseModel):
    text: str
    key: str | None = None


@app.post("/enrich")
async def enrich(body: EnrichIn):
    """Optional. Turns a transcript into a tag, a title and any to-dos, using
    whatever model Ollama has loaded. The device does not call this - the web
    page does - so a missing model degrades to a clear error, not a failure."""
    _check_key(body.key)
    text = body.text.strip()
    if not text:
        raise HTTPException(status_code=400, detail="empty text")

    prompt = (
        "You are filing a short spoken note. Reply with JSON only:\n"
        '{"title": "<=6 words", "tag": "one of ' + "|".join(TAGS) + '", '
        '"todos": ["any action items, else empty"]}\n\n'
        f"Note: {text}"
    )
    try:
        async with httpx.AsyncClient(timeout=120) as client:
            r = await client.post(
                f"{OLLAMA_URL}/api/chat",
                json={
                    "model": OLLAMA_MODEL,
                    "messages": [{"role": "user", "content": prompt}],
                    "format": "json",
                    "stream": False,
                },
            )
    except httpx.RequestError as exc:
        raise HTTPException(status_code=503, detail=f"ollama unreachable: {exc}") from exc

    if r.status_code != 200:
        raise HTTPException(status_code=502,
                            detail=f"ollama {r.status_code}: {r.text[:200]}")

    try:
        content = r.json()["message"]["content"]
        data = json.loads(content)
    except Exception:
        raise HTTPException(status_code=502, detail="ollama did not return JSON")

    tag = data.get("tag", "")
    return {
        "title": str(data.get("title", ""))[:60],
        "tag": tag if tag in TAGS else "",
        "todos": [str(t) for t in data.get("todos", []) if str(t).strip()][:10],
    }


@app.exception_handler(HTTPException)
async def http_error(_, exc: HTTPException):
    return JSONResponse(status_code=exc.status_code, content={"error": exc.detail})


if __name__ == "__main__":
    import uvicorn

    port = int(os.getenv("PORT", "8000"))
    print(f"model={WHISPER_MODEL}  ollama={OLLAMA_MODEL}  port={port}")
    uvicorn.run(app, host="0.0.0.0", port=port)
