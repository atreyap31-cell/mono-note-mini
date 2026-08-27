# Squiggly Pala - device firmware

Custom firmware for the **Waveshare ESP32-S3-Touch-ePaper-1.54** (touch variant, 16MB flash / 8MB PSRAM).

A notebook you talk to: record to the SD card, tag it, play it back on the speaker, transcribe it through your own server (no OpenAI required - any endpoint that takes a WAV upload and returns JSON text), sync on a schedule you choose, deep sleep between uses.

Built on Waveshare's own BSP code (e-paper driver, FT6336 touch, ES8311 codec stack, power rails) with a PlatformIO/Arduino glue layer written for this project.

## Flash it

1. Install **VS Code** -> Extensions -> install **PlatformIO IDE**
2. File -> Open Folder -> this `firmware/` folder
3. Connect the board with a **data** USB-C cable
4. Click **-> (Upload)** in the bottom PlatformIO toolbar
5. When it finishes, the device boots into the guided tour

If upload fails: hold the **BOOT** button while it says "Connecting...", or try another cable/port.

## First boot

A brand-new device (or one that was just factory reset) runs a **17-step guided tour** covering every feature. Each screen has a **NEXT** button that has to be pressed to move on - taps anywhere else are ignored, so nothing gets skipped by accident. **BACK** returns to the previous step. One step asks you to choose button sounds on or off before it will continue.

Re-run it any time from **Settings > Extra > Redo tutorial**. Re-running changes nothing on the card.

## Controls (all touch)

| Gesture | Action |
|---|---|
| **TAP** | choose |
| **HOLD 0.9s** | go back (sleeps from the menu) |
| **SWIPE up/down** | scroll lists, page through transcripts |

## Recording

**Hold MAKE NOTE** and it records while your finger is down; let go and it saves. **Or tap it once** and it keeps recording until you tap the timer - so a long note doesn't mean holding a finger on the glass for two minutes.

Either way it captures 16 kHz mono WAV to `/recordings/`, up to **120 seconds**, with a live level meter and a countdown. At the cap it stops and saves by itself.

The moment it saves, the **tag picker** appears: `Work`, `Projects`, `Ideas`, `Quotes`, `Random`, or tap the bottom line to skip.

## Menu

| Row | What |
|---|---|
| **VIEW NOTES** | browse by tag (each tag shows its count) -> note list -> play, read, delete |
| **MAKE NOTE** | record, as above |
| **TO-DO** | checklist stored as `/todo.txt`; tap a row to check it off |
| **SETTINGS** | everything else |

In the note view, `PLAY` plays on the speaker, swipes page through the transcript, and `DELETE` asks twice before removing the `.wav`, `.txt` and `.tag` together.

## Settings

| Row | What |
|---|---|
| **1 Wi-Fi** | starts the hotspot `MonoNote-XXXX` (key `record123`), serves at `192.168.4.1` |
| **2 Sync now** | manual sync - uploads every clip with no transcript yet |
| **3 Storage** | space used, plus **FREE SPACE** (below) |
| **4 IP** | joins your home Wi-Fi and shows the address to type in a browser |
| **5 How to & Credits** | quick reference |
| **6 Extra** | redo tutorial, auto-sync rate, factory reset |

### Storage and FREE SPACE

The storage page shows total/used/free and how many clips are already transcribed. **FREE SPACE** (two taps to confirm) deletes the **audio** of every clip that already has a transcript, and keeps the text and the tag. The words are the point; the audio is the bulky part.

### Extra

- **Redo tutorial** - runs the 17-step tour again. Deletes nothing.
- **Auto-sync rate** - `Off`, `1h`, `2h`, `4h`, `8h`, `24h`. **Default is every 4 hours.** Syncing more often means waking the radio more often, which drains the battery faster.
- **Factory reset** - erases the **entire SD card** (notes, transcripts, tags, to-do list) and every stored setting (Wi-Fi, server address, sound, sync rate), then reboots into the first-boot tour. Asks twice. Cannot be undone.

## Auto-sync

If Wi-Fi and an API address are both set, the device syncs on its own at the rate you picked, checking every 5 minutes to see whether it is due. Manual **Sync now** still works any time. Tap the screen mid-sync to stop early.

The clock is only set by NTP, and only a sync reaches NTP - so after a cold boot with no clock, the device makes one attempt to get itself started rather than waiting forever.

## Your own web interface

The device serves **`/www/index.html` from the SD card at the root URL** - design any site you like, drop it in `/www/`, done (no reflash). Endpoints:

| Route | What |
|---|---|
| `GET /api/list` | JSON `[{name, size}]` of `.wav`, `.txt` and `.tag` in `/recordings` |
| `GET /file?n=rec_x.wav` | download any recording/transcript/tag |
| `POST /up` | multipart upload of a `.wav`, `.txt` or `.tag` into `/recordings/` |
| `GET /api/todo` | the to-do list as plain text |
| `POST /api/todo` | replace the to-do list |
| `POST /save` | form fields: `ssid`, `pass`, `api`, `devpass`, `sound`, `synchrs` |

If `/www/index.html` doesn't exist, `/` redirects to the built-in manager at `/app` (browse, filter, download, upload, to-do, settings).

**Every route is behind HTTP Basic auth.** The device password defaults to `record123` and is changed on the settings form - so others on your Wi-Fi can't read your notes.

## Transcription backend

Sync uploads each WAV as multipart field `audio` to `{api}/transcribe` and reads `text`, `content`, `transcript`, or `result` from the JSON reply. Point `api` at anything that speaks that contract - your Jetson tunnel, a local Whisper server, anything. No OpenAI keys involved.

Note that TLS certificates are **not** verified on this upload (`setInsecure`), which is fine for a box on your own network. Don't point it somewhere sensitive over the open internet.

## Notes

- Files on SD: `rec_YYYYMMDD_HHMMSS.wav` + matching `.txt` (transcript) + `.tag` (category)
- Sleeps after 30 s idle on the home screen, 60 s in the menu; wakes on **BOOT**, **PWR**, or a touch
- Button sounds are a soft 1 kHz tick, **off by default**
- The companion web app (`index.html` at the repo root) imports these WAVs for reading aloud, tagging, and GitHub sync

## Troubleshooting

- **"no SD card"** -> card must be FAT32, inserted before power-on
- **"wifi failed"** -> check SSID/password on the settings page; API URL must be live
- **Upload fails** -> data cable, hold BOOT during connect, try another USB port
