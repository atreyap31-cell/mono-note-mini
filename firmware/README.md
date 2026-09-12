# Mono Note Mini — device firmware

A voice recorder with one button, on a Waveshare ESP32-S3 1.54" e-paper board.

Press the top button to start recording, press it again to stop. The note is
saved to the SD card and uploaded to the machine that serves the web page the
next time Wi-Fi is available. The screen shows the battery and how many notes
have gone up. That is the entire device.

Everything else — reading notes, tags, transcription, settings — is on the web
page. The device holds no account and no credential.

## What the screen says

| | |
|---|---|
| `no notes yet` | nothing recorded |
| `3 of 7 synced` | four still waiting for Wi-Fi |
| `all synced` | everything is up |
| a filled circle | recording |
| a square, then `SAVED` | it stopped and wrote the file |

It sleeps after 30 seconds and leaves the same screen on the glass, so waking
changes nothing. It does not sleep while on USB, so it can always be reflashed.

## Setting it up

There is nowhere on the device to type a Wi-Fi password, so it advertises over
Bluetooth whenever it is awake. On the web page, **Set up device over
Bluetooth**:

- **Scan** lists the networks the device can actually see from where it is
  sitting, strongest first.
- **Use my router's button** does WPS: press the button on the router within
  two minutes and nothing is typed at all.

The clock and time zone are set from the browser on every save, so notes are
named with real timestamps.

## Hardware facts that contradict the documentation

Each of these cost hours.

- **There is no touch panel** on this variant, whatever the vendor header
  implies. An I2C scan finds nothing at 0x38.
- **The flash is 8 MB, not 16.** A 16 MB partition table boots in a loop.
- **A successful flash is not evidence that any code runs.** If GPIO0 is held
  low the chip sits in its ROM bootloader, and flashing succeeds because the
  bootloader is what flashing talks to.
- **Opening the serial port resets the chip into that bootloader.** DTR and RTS
  are wired to the strapping pins, so a diagnostic that opens the port creates
  the exact fault it then reports. This was misdiagnosed twice here as a stuck
  BOOT button. To look at the chip honestly, use esptool with `--before
  no_reset`.
- **Never print from the button-sampling task.** A write to the USB console
  blocks until a host drains it, which freezes every button on the device.
- **The console is UART0, not USB.** `CONFIG_ESP_CONSOLE_UART_DEFAULT=y`, so
  `printf` goes to pins nothing is attached to. Serial silence means nothing.
- **GCC 14.2 ICEs** in `try_forward_edges` on this source. The `-fno-*` flags
  and `-O2` in `platformio.ini` hold it off; it is nondeterministic, so a retry
  often succeeds.

## Flashing

Easiest: open the web page and press **Update device**. It drives the chip over
Web Serial from the browser, with no toolchain at all. Chrome or Edge on a
computer.

Otherwise:

```powershell
.\flash.ps1              # build, upload, then open the serial monitor
.\flash.ps1 -Build       # build only
.\flash.ps1 -Monitor     # just watch the serial output
```

On Linux, run `./setup-linux.sh` from the repo root once, then
`./firmware/flash.sh`.

PlatformIO lives at `T:\pio-venv` with its toolchain at `T:\.platformio`, both
off the system drive.

If an upload fails it is almost always, in order: a charge-only USB cable, the
device being asleep and off the bus (press a button), or another program
holding the port. On Ubuntu and Mint add **brltty** to that list — it claims
the board as a braille display and the port vanishes.

## Bring-up checklist

1. **Does the screen change when you press?** That is the only real proof the
   firmware is running. Serial tells you nothing — the console is on UART0.
2. **Record something.** `NOT SAVED` means the card write failed; check it is
   FAT32 and not write-protected. Nothing at all usually means the PSRAM
   allocation failed, which means `board_build.arduino.memory_type` does not
   match the board.
3. **Check Bluetooth.** It advertises whenever awake. Any BLE scanner should
   show `Mono Note Mini`; if it does, the firmware is definitely running.
4. **Set up Wi-Fi from the page**, then confirm the counter reaches
   `all synced`.

## The wordmark

The resting screen is a battery bar and the script `mn` wordmark, nothing else.
E-paper holds an image for free, so what the device is left showing is what the
device looks like.

The wordmark lives in [`src/logo_mn.h`](src/logo_mn.h) as a 152x54 one-bit
bitmap, blitted by `uiBitmap()`. It was rasterised from **Script MT Bold** at
large size and downsampled, so the hairline joins are decided by area coverage
rather than by which pixel centre an outline happened to cross; the thinnest
surviving stroke is 2 px. Regenerate it from any face by rendering `mn`,
cropping to the ink, scaling to 152 px wide and packing row-major at 8 px per
byte, MSB leftmost.

Worth knowing before this ships anywhere commercial: it is a raster of a
licensed Microsoft/Monotype typeface. Font EULAs generally cover output like
this, and for a personal project it is unremarkable, but the bitmap can be
swapped for an open-licensed face without touching any code — only
`logo_mn.h` changes.

## `attic/`

The previous firmware's encryption, voice unlock and manual QR code. Not
compiled — PlatformIO builds everything under `src/`, so moving them out is
what retires them. The whole previous version, which had menus, Bluetooth note
transfer, GitHub sync, a PIN and voice unlock, is backed up at
`T:\mnm-backup-2026-09-10`.
