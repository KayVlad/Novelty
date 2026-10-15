from pathlib import Path
import select
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

with tempfile.TemporaryDirectory(prefix='novelty-http-') as directory:
    root = Path(directory)
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    server = subprocess.Popen([sys.argv[1], '--data', root, '--port', str(port)])
    connections = []
    try:
        until = time.monotonic() + 3
        while True:
            try:
                token = (root / 'access.token').read_text().strip()
                request = urllib.request.Request(f'http://127.0.0.1:{port}/v1/identity',
                                                 headers={'Authorization': 'Bearer ' + token})
                with urllib.request.urlopen(request, timeout=.3) as response:
                    assert response.status == 200
                break
            except (OSError, ValueError):
                if time.monotonic() > until:
                    raise
                time.sleep(.01)
        for _ in range(64):
            connection = socket.create_connection(('127.0.0.1', port), timeout=1)
            connection.sendall(b'GET /')
            connections.append(connection)
        overflow = socket.create_connection(('127.0.0.1', port), timeout=1)
        try:
            assert overflow.recv(1) == b'', 'request limit did not close excess connection'
        except ConnectionResetError:
            pass
        finally:
            overflow.close()
        for connection in connections:
            connection.close()
        connections.clear()
        until = time.monotonic() + 2
        while True:
            try:
                with urllib.request.urlopen(request, timeout=.3) as response:
                    assert response.status == 200
                break
            except OSError:
                if time.monotonic() > until:
                    raise
                time.sleep(.01)
        # Continuous progress must not extend an unauthenticated header forever.
        with socket.create_connection(('127.0.0.1', port), timeout=1) as slow:
            slow.sendall(b'GET /')
            started = time.monotonic()
            closed = False
            while time.monotonic() - started < 12:
                readable, _, _ = select.select([slow], [], [], .2)
                try:
                    if readable and slow.recv(1) == b'':
                        closed = True
                        break
                    slow.sendall(b'x')
                except (ConnectionResetError, BrokenPipeError):
                    closed = True
                    break
            assert closed, 'trickled bytes extended the absolute header deadline'
        print('PASS: HTTP connection limits, recovery and absolute header deadline')
    finally:
        for connection in connections:
            connection.close()
        server.kill()
        server.wait(timeout=3)
