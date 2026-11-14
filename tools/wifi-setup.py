#!/usr/bin/env python3
"""Gives the zones example its Wi-Fi credentials, without a serial monitor.

    python3 tools/wifi-setup.py [/dev/cu.usbserial-XXXX]

Asks for the network name and the password (hidden, never echoed and never stored on this
machine), answers the board's two questions, then shows what the board does with them. The
credentials stay in the board's NVS, so this is a one-time step.
"""
import getpass
import os
import sys
import time

try:
    import serial
except ModuleNotFoundError:
    # pyserial is rarely in the system Python, but PlatformIO ships one that has it. Switch to
    # it rather than asking anyone to install anything.
    _pio = os.path.expanduser("~/.platformio/penv/bin/python")
    if os.path.exists(_pio) and os.environ.get("_TFTCORE_RELAUNCHED") != "1":
        os.environ["_TFTCORE_RELAUNCHED"] = "1"
        os.execv(_pio, [_pio, os.path.abspath(__file__)] + sys.argv[1:])
    print("This needs pyserial. Either install it (pip3 install pyserial) or run it with")
    print("PlatformIO's Python: ~/.platformio/penv/bin/python " + os.path.abspath(__file__))
    sys.exit(1)

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbserial-8310"
BAUD = 115200


def read_until(port, needle, seconds, echo=True):
    """Reads until `needle` shows up. Returns the text read, or None on timeout."""
    end = time.time() + seconds
    text = ""
    while time.time() < end:
        chunk = port.read(512).decode("utf-8", "replace")
        if chunk:
            text += chunk
            if echo:
                sys.stdout.write(chunk)
                sys.stdout.flush()
        if needle in text:
            return text
    return None


def main():
    print(f"Talking to the board on {PORT}.")
    try:
        port = serial.Serial(PORT, BAUD, timeout=0.2, dsrdtr=False, rtscts=False)
    except serial.SerialException as e:
        print(f"Cannot open {PORT}: {e}")
        print("A serial monitor open on that port is the usual reason. Close it and try again.")
        return 1

    # Restart the board so the exchange always starts from a known point.
    port.dtr = False
    port.rts = True
    time.sleep(0.2)
    port.reset_input_buffer()
    port.rts = False

    print("Restarting it and waiting for its first question (about 10 seconds)...\n")
    if read_until(port, "1 of 2", 40) is None:
        print("\nThe board never asked for a network name.")
        print("It may already have credentials: press w in a serial monitor to forget them.")
        return 1

    ssid = input("\nName of the Wi-Fi network: ").strip()
    if not ssid:
        print("Nothing typed, stopping.")
        return 1
    port.write((ssid + "\n").encode())
    port.flush()

    if read_until(port, "2 of 2", 15, echo=False) is None:
        print("The board did not ask for the password. Run this again.")
        return 1

    # getpass keeps it off the screen and out of the shell history; it is sent straight to the
    # board, which stores it, and this script keeps no copy.
    password = getpass.getpass(f'Password for "{ssid}" (hidden): ')
    port.write((password + "\n").encode())
    port.flush()
    del password

    print("\nSent. What the board does next:\n")
    joined = read_until(port, "Wi-Fi up", 45)
    if joined is None:
        print("\n\nThe board did not report joining. The password may be wrong; it forgets the")
        print("credentials on failure, so running this again is enough to retry.")
        return 1
    read_until(port, "zzz-never-matches", 8)   # a few more seconds of its output
    print("\n\nDone: the board is on the network and will join by itself from now on.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
