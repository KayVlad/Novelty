"""Crash every removal boundary in disposable libraries; verify restore vs cleanup."""
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import uuid

binary, driver = map(lambda p: Path(p).resolve(), sys.argv[1:3])
with tempfile.TemporaryDirectory(prefix='novelty-removal-tests-') as temporary:
    base = Path(temporary)
    env = dict(os.environ, XDG_CONFIG_HOME=str(base / 'config'))

    def call(*args):
        return subprocess.run([driver, *map(str, args)], capture_output=True, text=True, env=env, timeout=5)

    def fixture(name, boundary):
        root = base / name
        game = root / 'Game'
        game.mkdir(parents=True)
        (game / 'game.exe').write_bytes(b'game data')
        (root / 'keep.txt').write_text('unrelated file')
        subprocess.run([binary, '--root', root, '--list'], check=True, capture_output=True, env=env)
        ident = str(uuid.uuid4())
        prefix = root / '.novelty/prefixes' / ident
        prefix.mkdir(parents=True)
        (prefix / 'save.dat').write_bytes(b'my save')
        config = dict(schema=1, id=ident, name='Game', folder=str(game), executable=str(game / 'game.exe'), cwd=str(game), backend='umu')
        dbpath = root / '.novelty/library.sqlite'
        with sqlite3.connect(dbpath) as db:
            db.execute('insert into games values(?,?,?)', (ident, str(game), json.dumps(config)))
        result = call('remove', root, ident, boundary)
        assert result.returncode == 88, (boundary, result.stdout, result.stderr)
        with sqlite3.connect(dbpath) as db:
            phase, raw = db.execute('select phase,manifest from removals').fetchone()
        return root, ident, prefix, dbpath, phase, json.loads(raw)

    def jobs(dbpath):
        with sqlite3.connect(dbpath) as db:
            return db.execute('select count(*) from removals').fetchone()[0]

    for boundary in ('intent', 'game-staged', 'prefix-staged', 'committed', 'folder-cleaned', 'state-cleaned'):
        root, ident, prefix, dbpath, phase, manifest = fixture(boundary, boundary)
        committed = boundary in ('committed', 'folder-cleaned', 'state-cleaned')
        assert phase == ('committed' if committed else 'staging')
        blocked = subprocess.run([binary, '--root', root, '--run', ident], env=env, capture_output=True, timeout=3)
        assert blocked.returncode == 2, 'unresolved removal allowed a launch'
        result = call('recover', root)
        assert result.returncode == 0 and jobs(dbpath) == 0, (boundary, result.stdout, result.stderr)
        assert not any(Path(move['staged']).exists() for move in manifest['moves'])
        with sqlite3.connect(dbpath) as db:
            assert db.execute('select count(*) from games').fetchone()[0] == (0 if committed else 1)
        if committed:
            assert not (root / 'Game').exists() and not prefix.exists()
            assert not (root / '.novelty/installations' / ident).exists()
        else:
            assert (root / 'Game/game.exe').read_bytes() == b'game data'
            assert (prefix / 'save.dat').read_bytes() == b'my save'
        assert (root / 'keep.txt').read_text() == 'unrelated file'
        assert call('recover', root).returncode == 0 and jobs(dbpath) == 0

    # A new folder at the original name must never be overwritten during restoration.
    root, ident, prefix, dbpath, phase, manifest = fixture('conflict', 'prefix-staged')
    (root / 'Game').mkdir()
    (root / 'Game/replacement.txt').write_bytes(b'new data')
    result = call('recover', root)
    assert jobs(dbpath) == 1 and 'Both original and staged' in result.stdout
    assert (root / 'Game/replacement.txt').read_bytes() == b'new data'
    assert (Path(manifest['moves'][0]['staged']) / 'game.exe').read_bytes() == b'game data'
    # Resolving just the conflict allows idempotent restoration.
    (root / 'Game').rename(root / 'Replacement kept')
    assert call('recover', root).returncode == 0 and jobs(dbpath) == 0
    assert (prefix / 'save.dat').read_bytes() == b'my save'
    assert (root / 'Replacement kept/replacement.txt').read_bytes() == b'new data'

    # Committed cleanup removes only staged originals; a new game at the old path stays.
    root, ident, prefix, dbpath, phase, manifest = fixture('new-original', 'committed')
    (root / 'Game').mkdir()
    (root / 'Game/new.exe').write_bytes(b'replacement game')
    assert call('recover', root).returncode == 0 and jobs(dbpath) == 0
    assert (root / 'Game/new.exe').read_bytes() == b'replacement game'

    # A swapped staging directory cannot authorize deleting unrelated contents.
    root, ident, prefix, dbpath, phase, manifest = fixture('swapped-stage', 'committed')
    staged = Path(manifest['moves'][0]['staged'])
    held = staged.with_name(staged.name + '-held')
    staged.rename(held)
    staged.mkdir()
    (staged / 'foreign.txt').write_bytes(b'keep foreign data')
    result = call('recover', root)
    assert jobs(dbpath) == 1 and 'cannot be verified' in result.stdout
    assert (staged / 'foreign.txt').read_bytes() == b'keep foreign data'
    assert (held / 'game.exe').read_bytes() == b'game data'
    # Restore the temporary fixture to finish its previously confirmed removal.
    (staged / 'foreign.txt').unlink()
    staged.rmdir()
    held.rename(staged)
    assert call('recover', root).returncode == 0 and jobs(dbpath) == 0

    # A corrupt manifest is retained for inspection, never guessed at.
    root, ident, prefix, dbpath, phase, manifest = fixture('damaged-record', 'prefix-staged')
    with sqlite3.connect(dbpath) as db:
        db.execute("update removals set manifest='broken'")
    result = call('recover', root)
    assert jobs(dbpath) == 1 and 'damaged' in result.stdout
    assert all(Path(move['staged']).exists() for move in manifest['moves'])
    with sqlite3.connect(dbpath) as db:
        db.execute('update removals set manifest=?', (json.dumps(manifest),))
    assert call('recover', root).returncode == 0 and jobs(dbpath) == 0

    # One-time launches cannot add references to any pending deletion target.
    root, ident, prefix, dbpath, phase, manifest = fixture('extra-reservations', 'intent')
    other = root / 'Other'; other.mkdir(); (other / 'game.exe').touch()
    runner = root / '.novelty/fixture-runner'; runner.mkdir(); (runner / 'proton').touch()
    other_id = str(uuid.uuid4())
    config = dict(schema=1, id=other_id, name='Other', folder=str(other), executable=str(other / 'game.exe'),
                  cwd=str(other), runner=str(runner), backend='umu')
    with sqlite3.connect(dbpath) as db:
        db.execute('insert into games values(?,?,?)', (other_id, str(other), json.dumps(config)))
        db.execute('insert into settings values(?,?)', ('app', json.dumps({'umu': '/bin/true'})))
    for directory in (root / 'Game', prefix, root / '.novelty/installations' / ident):
        patch = directory / 'patch.exe'; patch.touch()
        result = subprocess.run([binary, '--root', root, '--run', other_id, '--extra-exe=' + str(patch)], env=env, capture_output=True, timeout=3)
        state = root / '.novelty/installations' / other_id
        assert result.returncode == 1 and 'reserved' in json.loads((state / 'status.json').read_text())['message']
        assert not (state / 'extra-launch.json').exists(), 'reserved path reached preparation'
        assert jobs(dbpath) == 1 and patch.exists()
    assert call('recover', root).returncode == 0 and jobs(dbpath) == 0

print('PASS: removal crash recovery, replacement protection, damaged records and pending-path exclusion for extra executables')
