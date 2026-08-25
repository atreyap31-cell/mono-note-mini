# Squiggly Pala â€” device firmware

Custom firmware for the **Waveshare ESP32-S3-Touch-ePaper-1.54** (touch variant, 16MB flash / 8MB PSRAM).
Voice-only note device: hold-to-record to SD, tag, play back on the speaker, transcribe through your own server (no OpenAI required â€” any endpoint that accepts a WAV upload and returns JSON text), auto-sync daily, deep sleep between uses.

Built on Waveshare's own BSP code (e-paper driver, FT6336 touch, ES8311 codec stack, power rails) with a PlatformIO/Arduino glue layer written for this project.

## Flash it

1. Install **VS Code** â†’ Extensions â†’ install **PlatformIO IDE**
2. File â†’ Open Folder â†’ this `firmware/` folder
3. Connect the board with a **data** USB-C cable
4. Click the **â†’ (Upload)** in the bottom PlatformIO toolbar
5. When it finishes, the device boots into the menu

If upload fails: hold the **BOOT** button while it says "Connecting...", or try another cable/port.

## Controls (all touch)

| Gesture | Action |
|---|---|
| **Hold REC** | records while held (16 kHz mono WAV â†’ `/recordings/`) |
| Release | saves, then prompts the **tag picker** (idea / task / reminder / project, or skip) |
| **SYNC** | joins Wi-Fi, uploads every WAV without a transcript to `POST {api}/transcribe`, saves `.txt` beside it |
| **PLAY** | recording browser: tap a title to play on the speaker, tap again to stop, swipe to scroll, hold to exit |
| **SEND / SETTINGS** | hotspot `MonoNote-XXXX` (key `record123`) â†’ `http://192.168.4.1` |
| **Hold anywhere (menu)** | deep sleep â€” wakes on **BOOT** or **PWR** button; auto-sleeps after 60 s idle |
| Button sounds | confirmation blips on taps; toggle on the settings page |

## Daily auto-sync

After the first successful SYNC (which sets the clock via NTP), the device checks on every boot and once per minute: if **24 h** have passed and Wi-Fi + API are configured, it syncs by itself. Manual SYNC still works anytime.

## Your own web interface

The TRANSFER hotspot serves **`/www/index.html` from the SD card at the root URL** â€” design any site you like, drop it in `/www/`, done (no reflash). It talks to these endpoints:

| Route | What |
|---|---|
| `GET /api/list` | JSON `[{name, size}]` of everything in `/recordings` |
| `GET /file?n=rec_x.wav` | download any recording/transcript/tag file |
| `POST /up` | multipart upload, saves to `/recordings/<filename>` |
| `POST /save` | form fields: `ssid`, `pass`, `api`, `sound` |

If `/www/index.html` doesn't exist, `/` redirects to the built-in manager at `/app` (browse, filter, download, upload, settings).

## Transcription backend

SYNC uploads each WAV as multipart field `audio` to `{api}/transcribe` and reads `text`, `content`, `transcript`, or `result` from the JSON reply. Point `api` at anything that speaks that contract â€” your Jetson tunnel, a local Whisper server, anything. No OpenAI keys involved.

## Notes

- Files on SD: `rec_YYYYMMDD_HHMMSS.wav` + matching `.txt` (transcript) + `.tag` (category)
- Max clip length: 120 seconds
- The companion web app (`index.html` at the repo root) imports these WAVs for reading aloud, tagging, and GitHub sync

## Troubleshooting

- **"no SD card"** â†’ card must be FAT32, inserted before power-on
- **"wifi failed"** â†’ check SSID/password on the settings page; API URL must be live
- **Upload fails** â†’ data cable, hold BOOT during connect, try another USB port

