import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

payload = b'progress' * 100000
identifier = str(uuid.uuid4())
manifest = dict(schema=2, id=identifier, profile_id=str(uuid.uuid4()), profile_game_id=str(uuid.uuid4()),
                parent_ids=[], files=1, bytes=len(payload), items=[dict(scope='game', path='save', entries=[
                    dict(path='', type='directory'), dict(path='slot', type='file', size=len(payload),
                                                         sha256=hashlib.sha256(payload).hexdigest())])])
metadata = json.dumps(manifest).encode()
archive = b'NVSB\0\1\0\0' + struct.pack('>I', len(metadata)) + metadata + payload
class Server(ThreadingHTTPServer):
    daemon_threads = True
class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_): pass
    def do_GET(self):
        assert self.headers['Authorization'] == 'Bearer test-token'
        if self.server.mode == 'setup-stall':
            time.sleep(2)
            return
        self.send_response(200)
        self.send_header('Content-Length', str(len(archive)))
        for name, value in [('Server-Id', self.server.identity), ('Revision', '1'), ('Snapshot-Id', identifier), ('Files', '1')]:
            self.send_header('X-Novelty-' + name, value)
        self.end_headers()
        try:
            for offset in range(0, len(archive), 32768):
                self.wfile.write(archive[offset:offset + 32768]); self.wfile.flush()
                time.sleep(2 if self.server.mode == 'stall' else .075)
        except (BrokenPipeError, ConnectionResetError): pass

with tempfile.TemporaryDirectory(prefix='novelty-client-deadline-') as directory:
    with Server(('127.0.0.1', 0), Handler) as server:
        server.identity = str(uuid.uuid4())
        thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
        for mode in ('slow', 'stall', 'setup-stall'):
            server.mode = mode
            destination = Path(directory) / mode
            started = time.monotonic()
            result = subprocess.run([sys.argv[1], f'http://127.0.0.1:{server.server_port}', destination, '400'],
                                    capture_output=True, timeout=8)
            elapsed = time.monotonic() - started
            if mode == 'slow':
                assert result.returncode == 0 and elapsed > 1, result.stderr
                assert (destination / 'items/0/slot').read_bytes() == payload
            else:
                assert result.returncode == 1 and elapsed < 1.5, (mode, elapsed, result.stderr)
                assert not destination.exists()
        server.shutdown(); thread.join()
print('PASS: progressive transfers outlive the inactivity interval; stalled setup and transfers remain bounded')
