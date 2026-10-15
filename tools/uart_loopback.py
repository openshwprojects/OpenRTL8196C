#!/usr/bin/env python3
"""Check that the USB-UART adapter really transmits.

Short the adapter's TX and RX pins together (disconnect it from the router
first), then run:

    python tools/uart_loopback.py COM4

Every byte written must come straight back.  If nothing returns, the
adapter's TX pin (or its driver) is dead; if it works here but the router
never reacts, the problem is between the adapter TX pin and the router's
RX pad.
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 38400

ser = serial.Serial(port, baud, timeout=0.5)
ser.reset_input_buffer()
payload = bytes(range(32, 127))
ser.write(payload)
ser.flush()
got = ser.read(len(payload))
ser.close()

print(f"sent {len(payload)} bytes, received {len(got)}")
if got == payload:
    print("loopback OK: adapter TX works")
    sys.exit(0)
print("loopback FAILED: " + (repr(got) if got else "nothing received"))
sys.exit(1)
