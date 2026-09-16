#!/usr/bin/env bash
# Build and flash the Mono Note Mini.
#
#   ./flash.sh            build and upload
#   ./flash.sh --build    build only
#
# Use this rather than calling esptool by hand. Two things about this board
# make a hand-written command a good way to brick it until you find a cable:
#
# THE FLASH MODE IS dio, NOT qio
#   platformio.ini says qio and the board definition overrides it. Writing qio
#   into the bootloader header produces a watchdog boot loop that prints
#   "ets_loader.c 79" forever and never reaches the app. It is completely
#   recoverable - the ROM bootloader still answers - but it looks like a dead
#   device, and it is exactly what happened here once.
#
# THE PORT CHANGES DEPENDING ON WHAT IS RUNNING
#   The firmware uses native USB so the card can be shown as a drive, which
#   means the port it appears on is a software CDC port that only exists while
#   the firmware runs. In download mode the ROM presents its own USB-Serial
#   device instead, on a different port number. This script looks for whichever
#   is there.

set -uo pipefail
cd "$(dirname "$0")"
PIO="${PIO:-T:/pio-venv/Scripts/platformio.exe}"

if [ "${1:-}" = "--build" ]; then
  exec ./build.sh
fi

./build.sh || exit 1

find_port() {
  "$PIO" device list --serial 2>/dev/null \
    | grep -B2 "VID:PID=303A" \
    | grep -oE "^COM[0-9]+"
}

# Every port the board might be on, not just the first. Windows keeps listing
# ports after the device behind them has gone, and after all the USB mode
# switching this board does there is usually at least one such ghost - picking
# it and stopping is indistinguishable from the board being broken.
PORTS="$(find_port)"
if [ -z "$PORTS" ]; then
  echo "No board found. It is asleep, unplugged, or the cable is charge-only." >&2
  echo "Touch the screen to wake it, then run this again." >&2
  exit 1
fi

for PORT in $PORTS; do
  echo "trying $PORT"
  if ./build.sh --target upload --upload-port "$PORT"; then
    echo
    echo "flashed."
    exit 0
  fi
done

# esptool's auto-reset toggles DTR and RTS on a CDC port the firmware itself
# provides, and the firmware is not listening for that - so it never reaches
# download mode. Opening that port at 1200 baud does reboot it, after which the
# ROM appears on a different port.
echo "auto-reset did not take; asking it to reboot into download mode..."
# Tried more than once. The board is re-enumerating while this runs, and asking
# during the gap between the firmware's port disappearing and the ROM's
# appearing returns nothing at all.
BOOTPORT=""
for attempt in 1 2 3; do
  BOOTPORT="$("${PIO%platformio.exe}python.exe" tools/bootloader.py 2>/dev/null)"
  [ -n "$BOOTPORT" ] && break
  sleep 2
done
if [ -n "$BOOTPORT" ]; then
  echo "download mode on $BOOTPORT"
  if ./build.sh --target upload --upload-port "$BOOTPORT"; then
    echo
    echo "flashed."
    exit 0
  fi
fi

echo >&2
echo "Upload failed. In order, the things that fix it:" >&2
echo "  1. run this again - the port may have changed as it reset" >&2
echo "  2. hold BOOT while plugging the cable in, then run this again" >&2
echo "  3. check the cable carries data rather than only power" >&2
exit 1
