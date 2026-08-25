# Squiggly Pala — device firmware

Custom firmware for the **Waveshare ESP32-S3-Touch-ePaper-1.54** (touch variant, 16MB flash / 8MB PSRAM).
Voice-only note device: record to SD, transcribe through your Jetson tunnel, transfer files over Wi-Fi, deep sleep between uses.

Built on Waveshare's own BSP code (e-paper driver, FT6336 touch, ES8311 codec stack, power rails) with a PlatformIO/Arduino glue layer written for this project.

## Flash it

1. Install **VS Code** → Extensions → install **PlatformIO IDE**
2. File → Open Folder → this `firmware/` folder
3. Connect the board with a **data** USB-C cable
4. Click the **→ (Upload)** in the bottom PlatformIO toolbar
5. When it finishes, the device boots into the menu

If upload fails: hold the **BOOT** button while it says "Connecting...", or try another cable/port.

## Controls (all touch)

| Gesture | Action |
|---|---|
| Tap **REC** | start recording (16 kHz mono WAV → `/recordings/`) |
| Tap anywhere while recording | stop & save |
| Tap **SYNC** | join Wi-Fi, upload every WAV without a transcript to `POST {api}/transcribe`, save `.txt` next to it |
| Tap **TRANSFER** | start hotspot `PALA-XXXX` (key `record123`) → browse to `http://192.168.4.1` |
| **Hold** anywhere | deep sleep (weeks of standby) |
| Press **BOOT** | wake up |

## First-time setup

1. TRANSFER mode → join the hotspot → open the page
2. Fill in **SSID / password** and your **API base** (e.g. `https://your-tunnel.ngrok-free.dev`) → Save & reboot
3. Now SYNC works: every new recording gets transcribed and stored beside it as `.txt`

## Notes

- Recordings: `/sdcard/recordings/*.wav` (16 kHz, 16-bit, mono) with matching `.txt` transcripts
- Max clip length: 120 seconds
- The web app (`index.html` at the repo root) imports these WAVs; transcripts can be pasted in or synced via GitHub
- Battery % isn't shown yet (ADC pin mapping TBD) — everything else runs from the battery

## Troubleshooting

- **"no SD card"** → card must be FAT32, inserted before power-on
- **"wifi failed"** → check SSID/password in TRANSFER page; ngrok URL must be live
- **Upload fails** → data cable, hold BOOT during connect, try USB-A ports on the back of the PC
