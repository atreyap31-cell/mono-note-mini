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
| `GET /api/notes` | **one note per entry**, merged: `[{base, bytes, secs, audio, tag, txt}]` |
| `GET /api/info` | `{totalMB, usedMB, syncHrs, api, ssid}` |
| `GET /api/list` | raw file listing `[{name, size}]` of `/recordings` |
| `GET /file?n=rec_x.wav` | download any recording/transcript/tag |
| `POST /up` | multipart upload of a `.wav`, `.txt` or `.tag` into `/recordings/` |
| `POST /api/delete` | form `n=<base>` — removes audio, transcript and tag together |
| `POST /api/tag` | form `n=<base>`, `tag=<name>` — re-file a note, empty clears it |
| `GET /api/todo` | the to-do list as plain text |
| `POST /api/todo` | replace the to-do list |
| `POST /save` | form fields: `ssid`, `pass`, `api`, `devpass`, `sound`, `synchrs` |

Prefer `/api/notes` over `/api/list`: it returns one entry per note with the tag and transcript already merged in, so a page needs a single request instead of three per note — which matters when the server is an ESP32. It is streamed chunk by chunk, so a card full of transcripts never has to fit in RAM. Notes whose audio was freed appear with `audio: false` and their text intact.

If `/www/index.html` doesn't exist, `/` redirects to the built-in manager at `/app` (browse, filter, download, upload, to-do, settings).

### Installing the full site

The repo ships one at [`www/index.html`](../www/index.html). Copy that single file to `/www/index.html` on the SD card, then use **Settings > IP** to put the device on your home Wi-Fi and browse to the address it shows. You get search, tag filtering, in-page audio playback, re-filing, deletion and the to-do list — served by the device, off the card, over your own network. Nothing leaves the house and there is no account to create.

Privacy here rests on the device password (Basic auth) and on your LAN, not on the URL being hard to guess. Change `devpass` from the default.

### Reading your notes away from home

The device page has an optional **Publish to your repo** panel. It copies your notes into a GitHub repo you own, in the same JSON the companion app at [`index.html`](../index.html) already reads — so the web app shows them from anywhere.

The push happens from this page rather than from the firmware, deliberately:

- The token stays in your browser. It is never stored on a device you carry around, where NVS holds it in plain text.
- A page served over plain HTTP *may* call `https://api.github.com`; it is the reverse (an HTTPS page calling a `192.168.x.x` device) that browsers refuse. That single fact is why the device serves the page and the page talks to GitHub, and not the other way round.

It reads the file before writing it, so notes another device published are merged rather than overwritten, and re-publishing updates notes in place instead of duplicating them.

**Publish text only unless you have a reason not to.** GitHub's contents API only reads back files up to 1 MB, and base64 audio passes that almost immediately — four short clips measured 1.8 MB in testing, at which point the file can no longer be read back. Transcripts, tags and timestamps are a few KB. Audio belongs on the card, reachable over your own network.

Use a fine-grained token scoped to that one private repo with Contents read and write, and give it an expiry. Anyone holding it can read those notes, so it — not the URL — is what keeps them yours.

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
