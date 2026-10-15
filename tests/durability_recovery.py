import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

driver, fault = sys.argv[1:]
def run(root, action, **extra):
    return subprocess.run([driver, str(root), action], env=dict(os.environ, **extra),
                          capture_output=True, text=True, timeout=15)
with tempfile.TemporaryDirectory(prefix='novelty-durability-') as temporary:
    for action, path, at in [('restore', '.novelty-incoming-', 1),
                              ('restore', 'save-restore.json', 2),
                              ('snapshot', '.pending-', 1),
                              ('server', '.stage-', 1),
                              ('server', 'current.json', 1)]:
        for mode in ('fail', 'fail-always', 'crash'):
            root = Path(temporary) / f'{action}-{at}-{path.strip(".")}-{mode}'
            root.mkdir()
            setup = run(root, 'setup')
            assert setup.returncode == 0, setup.stderr
            failed = run(root, action, LD_PRELOAD=fault, NOVELTY_TEST_FLUSH_PATH=path,
                         NOVELTY_TEST_FLUSH_AT=str(at), NOVELTY_TEST_FLUSH_MODE=mode)
            assert failed.returncode == (88 if mode == 'crash' else 1), (action, path, mode, failed.stderr)
            recovered = run(root, 'server-recover' if action == 'server' else 'recover')
            assert recovered.returncode == 0, (action, path, mode, recovered.stderr)
            if action == 'restore':
                assert (root / 'Game/save/slot').read_bytes() in (b'old', b'new')
                assert not list(root.rglob('save-restore.json'))
            assert (root / 'Game/game.exe').read_bytes() == b'exe'
    root = Path(temporary) / 'enumeration-error'
    root.mkdir()
    assert run(root, 'setup').returncode == 0
    result = run(root, 'inventory', LD_PRELOAD=fault, NOVELTY_TEST_READDIR_PATH='/Game/save')
    assert result.returncode == 1, 'directory read failure became an empty live save inventory'
    assert (root / 'Game/save/slot').read_bytes() == b'new'
    print('PASS: real fsync failures and process interruption preserve verified snapshots and recoverable restore/server commits')
