# SPDX-License-Identifier: MIT
"""Stream-only Steam button routing for Triton state reports.

The local UI guard must be live before filtering any reports. No button other
than Steam is delayed. Tap decisions use physical releases, not repeated reports.
"""
class GuideTaps:
    MASK = 1 << 16
    def __init__(self, double_seconds=0.25, pulse_seconds=0.08):
        self.window = double_seconds
        self.pulse_seconds = pulse_seconds
        self.down = False
        self.pending = None
        self.second = False
        self.pulse_until = None
        self.latest = None
        self.sequence = None

    @staticmethod
    def supported(report):
        return bool(report) and ((report[0] == 0x42 and len(report) == 54) or
                                 (report[0] == 0x45 and len(report) == 46))

    def cancel(self):
        self.pending = self.pulse_until = None
        self.second = self.down = False

    def receive(self, report, now):
        if not self.supported(report):
            return report, None
        pressed = bool(int.from_bytes(report[2:6], 'little') & self.MASK)
        action = None
        if pressed and not self.down:
            self.second = self.pending is not None and now <= self.pending
            if self.second:
                self.pending = None
        elif not pressed and self.down:
            if self.second:
                self.second = False
                action = 'local'
            else:
                self.pending = now + self.window
        self.down = pressed
        self.latest = bytes(report)
        return self.render(now), action

    def render(self, now):
        if self.latest is None:
            return None
        report = bytearray(self.latest)
        bits = int.from_bytes(report[2:6], 'little') & ~self.MASK
        if self.pulse_until is not None and now < self.pulse_until:
            bits |= self.MASK
        report[2:6] = bits.to_bytes(4, 'little')
        # Synthetic press/release reports must not reuse a physical sequence.
        self.sequence = report[1] if self.sequence is None else (self.sequence + 1) & 255
        report[1] = self.sequence
        return bytes(report)

    def tick(self, now):
        if self.pending is not None and now >= self.pending:
            self.pending = None
            self.pulse_until = now + self.pulse_seconds
            return self.render(now), 'host'
        if self.pulse_until is not None and now >= self.pulse_until:
            self.pulse_until = None
            return self.render(now), None
        return None, None

    def timeout(self, now):
        times = [x for x in (self.pending, self.pulse_until) if x is not None]
        return max(0, min(times)-now) if times else None
