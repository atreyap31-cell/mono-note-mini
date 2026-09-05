#!/usr/bin/env bash
# Set up a Linux machine to build and flash the Mono Note Mini.
#
#   git clone https://github.com/atreyap31-cell/mono-note-mini.git
#   cd mono-note-mini
#   ./setup-linux.sh
#
# Installs PlatformIO into its own virtualenv and fixes the two things that
# stop a working ESP32-S3 being flashable on Ubuntu and Mint: the serial port
# being owned by a group you are not in, and brltty stealing the device.
#
# Transcription is not installed - it needs a GPU and is not wanted here. The
# device works fully without it; sync simply skips that half.

set -euo pipefail

BOLD=$'\e[1m'; DIM=$'\e[2m'; RED=$'\e[31m'; GREEN=$'\e[32m'; OFF=$'\e[0m'
say()  { echo "${BOLD}$*${OFF}"; }
note() { echo "${DIM}   $*${OFF}"; }
ok()   { echo "${GREEN}   ok${OFF} $*"; }
bad()  { echo "${RED}   $*${OFF}"; }

VENV="$HOME/.mono-note-mini-pio"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

say "1. System packages"
if ! command -v python3 >/dev/null; then bad "python3 missing"; exit 1; fi
missing=()
dpkg -s python3-venv >/dev/null 2>&1 || missing+=(python3-venv)
dpkg -s python3-pip  >/dev/null 2>&1 || missing+=(python3-pip)
command -v git >/dev/null              || missing+=(git)
if [ ${#missing[@]} -gt 0 ]; then
  note "installing: ${missing[*]}"
  sudo apt-get update -qq
  sudo apt-get install -y "${missing[@]}"
fi
ok "python3 $(python3 -V | cut -d' ' -f2)"

say "2. PlatformIO"
if [ ! -x "$VENV/bin/pio" ]; then
  note "creating $VENV"
  python3 -m venv "$VENV"
  "$VENV/bin/pip" install --quiet --upgrade pip platformio
fi
ok "$("$VENV/bin/pio" --version)"

say "3. Serial port permissions"
# On Debian-family systems /dev/ttyACM* belongs to the dialout group. Without
# membership every upload fails with a permission error that reads like the
# board is missing rather than like a permissions problem.
if id -nG "$USER" | tr ' ' '\n' | grep -qx dialout; then
  ok "already in the dialout group"
else
  note "adding $USER to dialout"
  sudo usermod -aG dialout "$USER"
  bad "log out and back in before flashing - group changes only apply to new sessions"
fi

say "4. brltty"
# brltty claims USB serial devices it mistakes for braille displays, including
# the ESP32-S3. The port appears for a second and vanishes. This is the single
# most common reason an ESP32 will not flash on Ubuntu and Mint.
if dpkg -s brltty >/dev/null 2>&1; then
  bad "brltty is installed and will steal the board's serial port"
  note "if you do not use a braille display:  sudo apt-get remove brltty"
  note "otherwise mask just the usb part:     sudo systemctl mask brltty-udev.service"
else
  ok "not installed"
fi

say "5. udev rule"
RULE=/etc/udev/rules.d/99-mono-note-mini.rules
if [ ! -f "$RULE" ]; then
  note "writing $RULE"
  echo 'SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", MODE="0666", GROUP="dialout"' \
    | sudo tee "$RULE" >/dev/null
  sudo udevadm control --reload-rules && sudo udevadm trigger
fi
ok "udev rule in place"

say "6. Build"
cd "$REPO/firmware"
if "$VENV/bin/pio" run >/tmp/mnm-build.log 2>&1; then
  ok "$(grep -o 'Flash:.*' /tmp/mnm-build.log | tail -1)"
else
  bad "build failed - see /tmp/mnm-build.log"
  tail -5 /tmp/mnm-build.log
  exit 1
fi

echo
say "Done."
note "flash with:   ./firmware/flash.sh"
note "the first build downloaded the ESP32 toolchain, so later ones are quick"
echo
note "If the board is not found: it appears as /dev/ttyACM0. Check with"
note "  ls -l /dev/ttyACM*        and    groups | grep dialout"
note "If it appears then disappears, brltty is the cause - see step 4."
