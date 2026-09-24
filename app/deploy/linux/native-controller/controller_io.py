"""Ordered controller commands, independent of the input-report reader."""
import queue
import threading
import time


def transient_haptic(op, data):
    if op != 3 or not data:
        return False
    if data[0] == 0x80 and len(data) >= 10:
        return any(data[3:5]) or any(data[6:8])
    if data[0] == 0x81 and len(data) >= 8:
        return any(data[6:8])
    return False


class CommandWorker:
    def __init__(self, execute, reply, capacity=32, max_haptic_age=.050,
                 clock=time.monotonic):
        self.execute, self.reply, self.clock = execute, reply, clock
        self.max_haptic_age = max_haptic_age
        self.queue = queue.Queue(capacity)
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.completed = self.expired = self.rejected = 0
        self.max_wait = 0.0
        self.thread.start()

    def submit(self, seq, op, data):
        try:
            self.queue.put_nowait((self.clock(), seq, op, data))
        except queue.Full:
            self.rejected += 1
            self.reply(seq, False, b'')

    def run(self):
        while not self.stop.is_set():
            try:
                queued, seq, op, data = self.queue.get(timeout=.05)
            except queue.Empty:
                continue
            wait = self.clock() - queued
            self.max_wait = max(self.max_wait, wait)
            if transient_haptic(op, data) and wait > self.max_haptic_age:
                self.expired += 1
                self.reply(seq, False, b'')
                continue
            try:
                result = self.execute(op, data)
                self.completed += 1
                self.reply(seq, True, result)
            except (OSError, ValueError):
                self.reply(seq, False, b'')

    def close(self):
        # Do not replay queued effects after the stream ends.
        self.stop.set()
        self.thread.join(timeout=1)
