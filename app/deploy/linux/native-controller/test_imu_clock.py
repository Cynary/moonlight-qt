import unittest
from imu_clock import ImuClock


def report(stamp, kind=0x42):
    b = bytearray(54 if kind == 0x42 else 46)
    b[0] = kind
    b[2:6] = (1 << 21).to_bytes(4, 'little')
    b[30:34] = stamp.to_bytes(4, 'little')
    return bytes(b)


def stamp(b): return int.from_bytes(b[30:34], 'little')


class ClockTests(unittest.TestCase):
    def test_frozen_firmware_clock_preserves_real_duration_and_release(self):
        now = [0]
        clock = ImuClock(lambda: now[0])
        before = None
        for i in range(8025):
            now[0] = i * 4_000_000
            b = bytearray(report(3063346503))
            if i >= 100: b[2:6] = bytes(4)
            forwarded = clock.normalize(bytes(b))
            self.assertEqual(forwarded[:30], b[:30])
            self.assertEqual(forwarded[34:], b[34:])
            if before is not None: self.assertEqual((stamp(forwarded)-before)&0xffffffff, 4000)
            before = stamp(forwarded)
        self.assertEqual((before-3063346503)&0xffffffff, 8024*4000)

    def test_wrap_and_sensor_restart_do_not_reset_forwarded_clock(self):
        now = [0]
        clock = ImuClock(lambda: now[0])
        self.assertEqual(stamp(clock.normalize(report(0xfffffff0))), 0xfffffff0)
        now[0] = 4_000_000
        self.assertEqual(stamp(clock.normalize(report(0, 0x45))), 3984)
        now[0] = 8_000_000
        self.assertEqual(stamp(clock.normalize(report(123456))), 7984)

    def test_same_receive_tick_is_monotonic(self):
        clock = ImuClock(lambda: 0)
        self.assertEqual(stamp(clock.normalize(report(1))), 1)
        self.assertEqual(stamp(clock.normalize(report(1))), 2)

    def test_other_reports_unchanged(self):
        clock = ImuClock()
        self.assertEqual(clock.normalize(b'\x43test'), b'\x43test')

if __name__ == '__main__': unittest.main()
