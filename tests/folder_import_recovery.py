import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile

driver, fault_library = map(str, map(Path, sys.argv[1:]))
with tempfile.TemporaryDirectory(prefix='novelty-import-destination-') as destination_area:
    destination_area = Path(destination_area)
    try:
        source_area = tempfile.TemporaryDirectory(prefix='novelty-import-source-', dir='/dev/shm')
    except OSError:
        print('SKIP: no second writable filesystem')
        sys.exit(77)
    with source_area as source_path:
        source_area = Path(source_path)
        if source_area.stat().st_dev == destination_area.stat().st_dev:
            print('SKIP: no second writable filesystem')
            sys.exit(77)
        for mode in ('success', 'fail', 'crash', 'cleanup'):
            root = destination_area / mode
            root.mkdir()
            source_parent = source_area / mode
            source = source_parent / 'Game'
            source.mkdir(parents=True)
            (source / 'game.exe').write_bytes(b'game executable')
            (source / 'Data').mkdir()
            (source / 'Data/asset.bin').write_bytes(b'asset data')
            marker = destination_area / (mode + '.marker')
            env = dict(os.environ, LD_PRELOAD=fault_library,
                       NOVELTY_TEST_IMPORT_FAULT=mode,
                       NOVELTY_TEST_IMPORT_SOURCE=str(source / 'game.exe'),
                       NOVELTY_TEST_IMPORT_MARKER=str(marker))
            result = subprocess.run([driver, root, source], env=env, capture_output=True, text=True, timeout=15)
            assert marker.exists() and marker.read_text() == 'source-present\n', (mode, result.stderr)
            with sqlite3.connect(root / '.novelty/library.sqlite') as db:
                registered = db.execute('select count(*) from games').fetchone()[0]
            if mode in ('success', 'cleanup'):
                assert result.returncode == 0, result.stderr
                assert registered == 1 and result.stdout.startswith(str(root / 'Game') + '\n')
                assert (root / 'Game/game.exe').read_bytes() == b'game executable'
                assert (root / 'Game/Data/asset.bin').read_bytes() == b'asset data'
                assert not source.exists()
                if mode == 'cleanup':
                    originals = list(source_parent.glob('.novelty-moved-*'))
                    assert len(originals) == 1 and 'remains for review' in result.stdout
                    (originals[0] / 'game.exe').chmod(0o600)
                    assert (originals[0] / 'game.exe').read_bytes() == b'game executable'
            else:
                assert result.returncode == (1 if mode == 'fail' else 88), result.stderr
                assert registered == 0
                assert (source / 'game.exe').read_bytes() == b'game executable'
                assert (source / 'Data/asset.bin').read_bytes() == b'asset data'
                if mode == 'fail':
                    assert not (root / 'Game').exists()
        print('PASS: cross-filesystem import durability, interruption and cleanup failure')
