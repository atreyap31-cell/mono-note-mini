# Local backend

Runs the whole thing on your own PC: Whisper for transcription, Ollama for the
optional language-model bits. Nothing leaves the network.

It speaks the contract the firmware already expects, so no reflash is needed —
put `http://<your-pc>:8000` in the device's API field and it works.

| Route | What |
|---|---|
| `POST /transcribe` | multipart, field `audio` → `{"text": "..."}` — the device calls this |
| `POST /enrich` | `{"text": ...}` → `{"title", "tag", "todos"}` — the device's web page calls this |
| `GET /health` | model, device, CUDA status |

## Running it

```powershell
.\run.ps1                    # small.en
.\run.ps1 -Model medium.en   # better, still ~32x realtime on the GPU
```

It prints the address to type into the device.

## Measured on this machine

RTX 5070 (12 GB) + i9-14900K, transcribing 28 s of speech:

| | | |
|---|---|---|
| `small.en` | **cuda/float16** | 0.67 s — **42× realtime** |
| `small.en` | cpu/int8 | 2.30 s — 12× realtime |
| `medium.en` | cuda/float16 | 0.88 s — 32× realtime |

A full 2-minute note comes back in about 3 seconds on the GPU, 10 on the CPU.
The CPU path is genuinely usable, so a GPU problem degrades rather than breaks.

Whisper `small.en` on the GPU uses ~1 GB of VRAM, `medium.en` ~2.5 GB, so there
is plenty left over for an 8B model in Ollama at the same time.

## Setup

Once, to build the environment (kept on T: — C: has no room):

```powershell
T:\python.exe -m venv T:\mnm-server-venv
$env:TMP = "T:\pip-tmp"; $env:TEMP = "T:\pip-tmp"
T:\mnm-server-venv\Scripts\python.exe -m pip install --cache-dir T:\pip-cache -r requirements.txt
T:\mnm-server-venv\Scripts\python.exe -m pip install --cache-dir T:\pip-cache nvidia-cublas-cu12 nvidia-cudnn-cu12
```

Those last two are what make the GPU work. Without them CTranslate2 fails with
`cublas64_12.dll is not found` — on Windows the CUDA runtime ships inside the
pip wheels rather than on PATH, and `app.py` registers those directories at
import time so you do not have to.

## Reading notes aloud

`/tts` speaks a note back using [Piper](https://github.com/rhasspy/piper), which
runs locally on the CPU - no GPU, no API key, no cloud. One voice is installed:

    en_GB-northern_english_male-medium

Download it once (63 MB) into the voice directory:

```powershell
mkdir T:\piper-voices
$b = "https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_GB/northern_english_male/medium"
$v = "en_GB-northern_english_male-medium"
curl.exe -L "$b/$v.onnx"      -o "T:\piper-voices\$v.onnx"
curl.exe -L "$b/$v.onnx.json" -o "T:\piper-voices\$v.onnx.json"
```

The directory is `T:\piper-voices` when it exists and `server/voices` otherwise,
so a checkout on a machine with no T: drive still works. Override with
`PIPER_DIR`, and pick a different installed voice with `PIPER_VOICE`.

Synthesis is about a second per four seconds of speech on the CPU, and the model
is loaded on first use rather than at startup.

`GET /voices` lists what is installed. The web page asks for that rather than
carrying its own list - it used to name a dozen voices from a different engine,
none of which anything here could produce. A voice the server does not have is
ignored in favour of one it does, because serving the wrong voice silently is
more confusing than not honouring the request.

Nothing depends on this: with no server reachable the page falls back to the
browser's own speech synthesis, which is free and needs no setup.

## Letting the device reach it

Windows blocks inbound 8000 by default. Once, from an **admin** PowerShell:

```powershell
New-NetFirewallRule -DisplayName "Mono Note Mini backend" -Direction Inbound `
  -LocalPort 8000 -Protocol TCP -Action Allow -Profile Private
```

`-Profile Private` matters: it opens the port on your home network and not on
public Wi-Fi. Check your network is classified Private, or the rule will not
apply.

Then set the device's API field (Settings → Wi-Fi → the web form) to
`http://<your-pc>:8000`. Sync sends each clip there and writes the transcript
back to the card.

**Give the PC a static DHCP reservation** on your router. The device stores the
API address in NVS, so if the PC's IP moves, sync quietly stops working.

## The language-model half

`/enrich` sends a transcript to Ollama and asks for a title, one of the device's
five tags, and any action items. Ollama is already installed here; it needs a
model that fits:

```powershell
ollama pull llama3.2:3b      # ~2 GB, fast
ollama pull qwen2.5:7b       # ~4.7 GB, noticeably better
```

The 49B Nemotron already in the Ollama folder will not run on this machine —
Q8 at 49B is ~52 GB of weights, more than the VRAM and RAM combined.

`/enrich` returns a clear error if Ollama is not running rather than failing
silently, and the firmware never calls it, so nothing breaks without it.

### Where it shows up

The device's own page (`www/index.html`) grows a **Suggest** button on any note
that has a transcript. It reads the backend address from `/api/info` — the same
one the device already stores for syncing — so there is nothing extra to
configure.

Pressing it files the note under whichever of the five tags the model picks,
and *asks* before adding any action items to the to-do list rather than
appending them silently. A tag outside the five is discarded.

That page is served by the device and this server is a different origin, so the
server sends CORS headers. They are open by default because this is a LAN tool;
set `ALLOW_ORIGINS` to restrict it.

## Away from home

This is LAN-only by design: no certificates, no exposure, nothing to configure.
To reach it from outside, put a Cloudflare Tunnel in front rather than
forwarding a port.

Before doing that, fix the firmware: `transcribeFile()` calls
`client.setInsecure()`, so TLS certificates are not verified. On your own
network that is unremarkable. Pointed at anything across the internet it means
an interceptor can read every note you record, and the device will not notice.
