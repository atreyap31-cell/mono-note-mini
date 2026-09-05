#!/usr/bin/env bash
# Build and flash the Mono Note Mini from Linux.
#
#   ./firmware/flash.sh          build, upload, then open the serial monitor
#   ./firmware/flash.sh --build  build only
#   ./firmware/flash.sh --mon    monitor only, without flashing
#
# Run ./setup-linux.sh first - it installs PlatformIO and fixes the serial
# port permissions that otherwise make a working board look absent.

set -euo pipefail

BOLD=$'\e[1m'; DIM=$'\e[2m'; RED=$'\e[31m'; OFF=$'\e[0m'
say() { echo "${BOLD}$*${OFF}"; }
bad() { echo "${RED}$*${OFF}"; }

VENV="$HOME/.mono-note-mini-pio"
PIO="$VENV/bin/pio"
cd "$(dirname "${BASH_SOURCE[0]}")"

if [ ! -x "$PIO" ]; then
  bad "PlatformIO is not installed. Run ./setup-linux.sh from the repo root."
  exit 1
fi

case "${1:-}" in
  --build) say "Building"; exec "$PIO" run ;;
  --mon)   say "Monitor - Ctrl+A then K to quit"; exec "$PIO" device monitor ;;
esac

# A board in its ROM bootloader flashes perfectly and runs nothing, so say what
# was actually seen rather than letting a silent success imply the code is live.
say "Looking for the board"
PORT="$("$PIO" device list --serial 2>/dev/null | grep -o '/dev/ttyACM[0-9]*' | head -1 || true)"
if [ -z "$PORT" ]; then
  bad "No /dev/ttyACM* found."
  echo "${DIM}  - is the USB-C cable a data cable rather than charge-only?${OFF}"
  echo "${DIM}  - groups | grep dialout    (log out and back in if it is missing)${OFF}"
  echo "${DIM}  - if the port appears then vanishes, brltty is taking it:${OFF}"
  echo "${DIM}      sudo apt-get remove brltty${OFF}"
  exit 1
fi
echo "${DIM}  found $PORT${OFF}"

say "Building and uploading"
"$PIO" run --target upload

echo
say "Flashed."
echo "${DIM}A successful flash does not prove the firmware runs. If the screen never${OFF}"
echo "${DIM}changes, GPIO0 is being held low and the chip is sitting in its ROM${OFF}"
echo "${DIM}bootloader - check the BOOT button is not stuck or pressed by the case.${OFF}"
echo
say "Monitor - Ctrl+A then K to quit"
exec "$PIO" device monitor
