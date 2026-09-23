#!/usr/bin/env python3
"""Synthetic input for transport tests; never opens a physical controller."""
import selectors
import sys
import time
import struct
s = selectors.DefaultSelector()
s.register(sys.stdin, selectors.EVENT_READ)
print('READY', flush=True)
sequence = 0
while True:
    for key, _ in s.select(0.004):
        line = key.fileobj.readline().strip()
        if not line or line == 'X':
            sys.exit(0)
        fields = line.split()
        if len(fields) == 4 and fields[0] == 'Q':
            print('R ' + fields[1] + ' -1 -', flush=True)
    report = bytearray(54)
    report[0] = 0x42
    sequence = (sequence + 1) & 0xffff
    report[1] = sequence & 0xff
    struct.pack_into('<I', report, 30, (sequence * 4000) & 0xffffffff)
    # A toggles every second; left stick follows a repeatable ramp.
    report[2] = 1 if int(time.monotonic()) % 2 else 0
    struct.pack_into('<h', report, 10, (sequence * 100) % 60000 - 30000)
    struct.pack_into('<h', report, 46, 32767)
    print('I ' + report.hex(), flush=True)
