from pathlib import Path
import os
import concurrent.futures
import base64
import hashlib
import json
import struct
import uuid
import urllib.error
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
    env = dict(os.environ)
    if len(sys.argv) > 2:
        env.update(LD_PRELOAD=sys.argv[2], NOVELTY_TEST_FLUSH_PATH='.stage-',
                   NOVELTY_TEST_FLUSH_MODE='slow', NOVELTY_TEST_FLUSH_MARKER=str(root / 'flush.marker'))
    def start():
        return subprocess.Popen([sys.argv[1], '--data', root, '--port', str(port)], env=env)
    server = start()
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
        base = f'http://127.0.0.1:{port}'
        key = base64.urlsafe_b64encode('game/key with spaces é'.encode()).decode().rstrip('=')
        path = '/v1/games/' + key
        def exchange(method, path=path, data=None, authorized=True, expected=200, **headers):
            if authorized:
                headers['Authorization'] = 'Bearer ' + token
            req = urllib.request.Request(base + path, data=data, method=method, headers=headers)
            try:
                response = urllib.request.urlopen(req, timeout=10)
            except urllib.error.HTTPError as error:
                response = error
            with response:
                body = response.read()
                assert response.status == expected, (method, response.status, body)
                return response.headers, body
        identity = json.loads(exchange('GET', '/v1/identity')[1])['server_id']
        uuid.UUID(identity)
        empty, _ = exchange('HEAD')
        assert empty['X-Novelty-Server-Id'] == identity
        assert empty['X-Novelty-Revision'] == '0' and empty['X-Novelty-Files'] == '0'
        for method in ('HEAD', 'GET', 'PUT'):
            exchange(method, data=b'bad' if method == 'PUT' else None, authorized=False, expected=401)
        payload = b'progress' * (8 * 1024 * 1024)  # 64 MiB real archive, payload and hashes
        manifest = dict(schema=2, id=str(uuid.uuid4()), profile_id=str(uuid.uuid4()),
                        profile_game_id=str(uuid.uuid4()), parent_ids=[], files=1, bytes=len(payload),
                        items=[dict(scope='game', path='save', entries=[
                            dict(path='', type='directory'),
                            dict(path='slot.dat', type='file', size=len(payload), sha256=hashlib.sha256(payload).hexdigest())])])
        metadata = json.dumps(manifest, separators=(',', ':')).encode()
        archive = b'NVSB\0\1\0\0' + struct.pack('>I', len(metadata)) + metadata + payload
        _, ack = exchange('PUT', data=archive, **{'X-Expected-Revision': '0'})
        ack = json.loads(ack)
        assert ack['revision'] == 1 and ack['snapshot_id'] == manifest['id'] and ack['files'] == 1
        head, body = exchange('HEAD')
        assert not body and head['X-Novelty-Revision'] == '1'
        assert head['X-Novelty-Snapshot-Id'] == manifest['id']
        downloaded_headers, downloaded = exchange('GET')
        length = struct.unpack('>I', downloaded[8:12])[0]
        assert downloaded[:8] == b'NVSB\0\1\0\0'
        assert json.loads(downloaded[12:12 + length]) == manifest
        assert downloaded[12 + length:] == payload
        assert hashlib.sha256(downloaded[12 + length:]).hexdigest() == manifest['items'][0]['entries'][1]['sha256']
        if len(sys.argv) > 2:
            (root / 'flush.marker').unlink(missing_ok=True)
            with concurrent.futures.ThreadPoolExecutor() as pool:
                committing = pool.submit(exchange, 'PUT', data=archive, **{'X-Expected-Revision': '1'})
                until = time.monotonic() + 2
                while not (root / 'flush.marker').exists():
                    assert time.monotonic() < until and not committing.done(), 'did not reach slow commit barrier'
                    time.sleep(.005)
                started = time.monotonic()
                assert exchange('HEAD')[0]['X-Novelty-Revision'] == '1'
                exchange('GET', '/v1/identity')
                assert time.monotonic() - started < .4, 'commit work blocked HTTP event loop or HEAD'
                assert json.loads(committing.result()[1])['revision'] == 2
            revision = '2'
        else:
            revision = '1'
        exchange('PUT', data=archive, expected=409, **{'X-Expected-Revision': '0'})
        corrupt = archive[:-1] + bytes([archive[-1] ^ 1])
        exchange('PUT', data=corrupt, expected=422, **{'X-Expected-Revision': revision})
        assert exchange('HEAD')[0]['X-Novelty-Revision'] == revision
        # HEAD uses committed metadata; GET still verifies payload bytes before sending.
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
        owned = root / '.request-Ab1234'; owned.mkdir()
        (owned / 'request-owner.json').write_text(json.dumps(dict(schema=1, server_id=identity, directory=owned.name)))
        (owned / 'upload.nvsb').write_bytes(b'interrupted request')
        ambiguous = root / '.request-Xy9876'; ambiguous.mkdir()
        (ambiguous / 'precious').write_bytes(b'keep ambiguous stage')
        server.kill(); server.wait(timeout=3); server = start()
        until = time.monotonic() + 3
        while True:
            try:
                exchange('GET', '/v1/identity')
                break
            except OSError:
                assert time.monotonic() < until
                time.sleep(.01)
        assert not owned.exists() and (ambiguous / 'precious').read_bytes() == b'keep ambiguous stage'
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
        print('PASS: real HTTP authenticated bundle round trip, stale revision, corruption, connection caps and deadlines')
    finally:
        for connection in connections:
            connection.close()
        server.kill()
        server.wait(timeout=3)
