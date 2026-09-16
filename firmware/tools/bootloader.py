"""Put the board into download mode and report the port it appears on.

Since the firmware took over native USB - so the card can be shown as a drive -
esptool's usual auto-reset does not work. It toggles DTR and RTS on a CDC port
the firmware itself provides, and the firmware is not listening for that.

Opening that port at 1200 baud does work: the USB stack in the core treats it
as a request to reboot into the bootloader. The chip then disappears and comes
back as the ROM's own USB-Serial device, on a different port number, which is
what this prints.

Told apart by the serial number: the ROM reports it with colons
(28:84:85:90:77:8C), the firmware's CDC port without (28848590778C).
"""

import subprocess
import sys
import time

import serial
import serial.tools.list_ports


def openable(dev):
    """Windows keeps listing ports after the device behind them has gone, and
    a stale one reports "a device attached to the system is not functioning"
    when touched. Repeated USB mode switching produces these constantly here,
    and picking one is indistinguishable from the board being broken."""
    try:
        serial.Serial(dev).close()
        return True
    except Exception:
        return False


def ports():
    out = []
    for p in serial.tools.list_ports.comports():
        hwid = (p.hwid or "").upper()
        if "303A" not in hwid:
            continue
        if not openable(p.device):
            continue
        # The ROM prints the MAC with colons; the firmware's CDC port does not.
        rom = ":" in (p.serial_number or "")
        out.append((p.device, rom))
    return out


def main():
    found = ports()
    if not found:
        print("no board", file=sys.stderr)
        return 1

    for dev, rom in found:
        if rom:
            print(dev)          # already in download mode
            return 0

    dev = found[0][0]
    try:
        p = serial.Serial(dev, 1200)
        p.dtr = False
        p.rts = False
        time.sleep(0.25)
        p.close()
    except Exception:
        # Opening it is itself enough to make it re-enumerate on some hosts, so
        # a failure here is not fatal - what matters is what turns up next.
        pass

    for _ in range(30):
        time.sleep(0.4)
        for d, rom in ports():
            if rom:
                print(d)
                return 0
    print("did not reach download mode", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
