# SPDX-License-Identifier: MIT
"""Give forwarded input a continuous client-side microsecond clock.

The firmware's sensor clock can freeze while reports and trackpad motion keep
arriving. Steam Input also uses this field for trackball timing. Incrementing a
frozen clock by one microsecond per report makes normal movement appear thousands
of times faster, and resetting that adjustment causes backwards time jumps.

Use elapsed monotonic receive time, anchored to the first sensor timestamp. Keep
all reports and fields other than this timestamp. Wrap naturally at 32 bits;
firmware sensor-clock stops/restarts do not restart the forwarded input clock.
"""
import time


class ImuClock:
    def __init__(self, clock_ns=time.monotonic_ns):
        self.clock_ns = clock_ns
        self.epoch_ns = None
        self.epoch_stamp = None
        self.last_elapsed = None
        self.adjustments = 0

    def normalize(self, report):
        expected = {0x42: 54, 0x45: 46}
        if not report or report[0] not in expected or len(report) != expected[report[0]]:
            return report
        raw = int.from_bytes(report[30:34], 'little')
        now = self.clock_ns()
        if self.epoch_ns is None:
            self.epoch_ns, self.epoch_stamp = now, raw
            elapsed = 0
        else:
            elapsed = max((now - self.epoch_ns) // 1000, self.last_elapsed + 1)
        self.last_elapsed = elapsed
        sent = (self.epoch_stamp + elapsed) & 0xffffffff
        if sent == raw:
            return report
        result = bytearray(report)
        result[30:34] = sent.to_bytes(4, 'little')
        self.adjustments += 1
        return bytes(result)
