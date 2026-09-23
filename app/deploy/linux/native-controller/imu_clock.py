# SPDX-License-Identifier: MIT
"""Work around Steam Input's neutral orientation on repeated Triton IMU times.

Keep every input report. Separate reports sharing a sensor timestamp by one
microsecond; the next real sensor timestamp remains unchanged. Never replace
a genuine clock reset with a large invented time interval.
"""
class ImuClock:
    def __init__(self):
        self.raw = None
        self.sent = None
        self.adjustments = 0

    def normalize(self, report):
        expected = {0x42: 54, 0x45: 46}
        if not report or report[0] not in expected or len(report) != expected[report[0]]:
            return report
        raw = int.from_bytes(report[30:34], 'little')
        sent = raw
        if self.raw is not None:
            advance = (raw - self.raw) & 0xffffffff
            ahead = (self.sent - raw) & 0xffffffff
            # Allow a duplicate or a small forward step overtaken by our prior
            # adjustment. A real backwards jump starts a new clock epoch.
            if advance < 0x80000000 and ahead < 1000:
                sent = (self.sent + 1) & 0xffffffff
        self.raw, self.sent = raw, sent
        if sent == raw:
            return report
        result = bytearray(report)
        result[30:34] = sent.to_bytes(4, 'little')
        self.adjustments += 1
        return bytes(result)
