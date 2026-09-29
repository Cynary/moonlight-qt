# SPDX-License-Identifier: MIT
"""Keep a leased guide-button guard in Steam's local UI during a native stream."""
import json
import pathlib
import queue
import threading
import time
import urllib.request


class GuideGuard:
    def __init__(self):
        self.ready = threading.Event()
        self.stopping = threading.Event()
        self.commands = queue.Queue(maxsize=4)
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def local(self):
        try:
            self.commands.put_nowait('local')
        except queue.Full:
            pass

    def close(self):
        self.stopping.set()
        self.thread.join(timeout=3)

    def run(self):
        import sys
        try:
            import websocket
        except ImportError:
            print('Guide routing inactive: python websocket-client is unavailable', file=sys.stderr)
            return
        ws = None
        sequence = 0
        def evaluate(expression):
            nonlocal sequence
            sequence += 1
            ws.send(json.dumps({'id': sequence, 'method': 'Runtime.evaluate',
                'params': {'expression': expression, 'returnByValue': True}}))
            while True:
                result = json.loads(ws.recv())
                if result.get('id') == sequence:
                    if 'error' in result or result.get('result', {}).get('exceptionDetails'):
                        raise RuntimeError(str(result))
                    return result['result']['result'].get('value')
        try:
            with urllib.request.urlopen('http://127.0.0.1:8080/json', timeout=2) as response:
                pages = json.load(response)
            page = next(p for p in pages if p.get('title') == 'SharedJSContext')
            ws = websocket.create_connection(page['webSocketDebuggerUrl'], timeout=2, suppress_origin=True)
            script = pathlib.Path(__file__).with_name('guide_guard.js').read_text()
            evaluate(script)
            self.ready.set()
            print('Guide routing ready: single tap host, double tap local (250 ms)', file=sys.stderr)
            while not self.stopping.is_set():
                try:
                    command = self.commands.get(timeout=.5)
                except queue.Empty:
                    command = None
                expression = '(() => { const g=window.__moonmachineGuideGuard; if(!g) return false; g.deadline=performance.now()+3000; '
                expression += 'return g.local(); })()' if command == 'local' else 'return true; })()'
                if not evaluate(expression):
                    raise RuntimeError('Steam guide guard unavailable')
        except Exception as error:
            print('Guide routing stopped: ' + str(error), file=sys.stderr)
        finally:
            self.ready.clear()
            if ws:
                try:
                    evaluate('window.__moonmachineGuideGuard?.restore()')
                except Exception:
                    pass
                ws.close()
