import threading
import unittest
from controller_io import CommandWorker, transient_haptic


class Commands(unittest.TestCase):
    def test_order_backpressure_expiry_and_stop(self):
        entered, release, finished = (threading.Event() for _ in range(3))
        now = [0.0]
        calls, replies = [], []
        def execute(op, data):
            calls.append((op, data))
            if len(calls) == 1:
                entered.set()
                self.assertTrue(release.wait(2))
            return b'reply' if op == 1 else b''
        def reply(seq, ok, data):
            replies.append((seq, ok, data))
            if seq == 'read': finished.set()
        worker = CommandWorker(execute, reply, capacity=3, clock=lambda: now[0])
        try:
            worker.submit('setting', 2, b'set')
            self.assertTrue(entered.wait(2))
            # A blocked USB operation must not block the caller/input reader.
            worker.submit('old-effect', 3, bytes([0x81,0,1,0,1,0,10,0]))
            worker.submit('stop-effect', 3, bytes([0x81,0,0,0,0,0,0,0]))
            worker.submit('read', 1, b'get')
            worker.submit('overflow', 2, b'never')
            self.assertIn(('overflow', False, b''), replies)
            now[0] = .100
            release.set()
            self.assertTrue(finished.wait(2))
            self.assertEqual([x[0] for x in calls], [2, 3, 1])
            self.assertEqual(calls[1][1][6:8], b'\0\0')
            self.assertIn(('old-effect', False, b''), replies)
            self.assertIn(('read', True, b'reply'), replies)
            self.assertEqual(worker.expired, 1)
        finally:
            release.set()
            worker.close()

    def test_command_failure_does_not_stop_following_query(self):
        finished = threading.Event()
        replies = []
        def execute(op, data):
            if op == 2: raise OSError('USB failed')
            return b'actual-reply'
        def reply(seq, ok, data):
            replies.append((seq, ok, data))
            if seq == 'query': finished.set()
        worker = CommandWorker(execute, reply)
        try:
            worker.submit('failed-setting', 2, b'')
            worker.submit('query', 1, b'')
            self.assertTrue(finished.wait(2))
            self.assertEqual(replies, [('failed-setting', False, b''), ('query', True, b'actual-reply')])
        finally: worker.close()

    def test_rumble_stop_and_features_never_expire_as_haptics(self):
        self.assertTrue(transient_haptic(3, bytes([0x80,0,0,1,0,0,0,0,0,0])))
        self.assertFalse(transient_haptic(3, bytes([0x80])+bytes(63)))
        self.assertFalse(transient_haptic(2, bytes([1,0x81])+bytes(62)))

if __name__ == '__main__': unittest.main()
