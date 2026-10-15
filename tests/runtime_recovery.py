"""Real Linux process-tree/crash checks, using only temporary fake-game libraries."""
import ctypes
import json
import os
from pathlib import Path
import signal
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
import uuid

binary = Path(sys.argv[1]).resolve()
# Adopt fixture children if we kill their Novelty supervisor; leave no orphan fixtures.
assert ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0) == 0

BACKEND = r'''import json, os, pathlib, subprocess, sys, time
p = pathlib.Path(os.environ['WINEPREFIX'])
p.mkdir(parents=True, exist_ok=True)
if sys.argv[1] == '--child':
    (p / 'child.pid').write_text(str(os.getpid()))
    deadline = time.monotonic() + 15
    while not (p / 'release').exists() and time.monotonic() < deadline:
        time.sleep(.02)
    sys.exit(0)
if len(sys.argv) > 2 and sys.argv[2] == '/c':
    (p / 'system.reg').touch()
    (p / 'drive_c/windows/system32').mkdir(parents=True, exist_ok=True)
    (p / 'drive_c/windows/system32/winecfg.exe').touch()
    (p / 'drive_c/windows/regedit.exe').touch()
    with (p / 'attempts').open('a') as f: f.write('prepare\n')
else:
    (p / 'invocation.json').write_text(json.dumps(dict(args=sys.argv[1:], cwd=os.getcwd())))
    if (p / 'write-game-save').exists():
        save = pathlib.Path(sys.argv[1]).parent / 'UserData' / 'slot.sav'
        save.parent.mkdir(parents=True, exist_ok=True)
        save.write_bytes(b'headless saved progress')
if (p / 'spawn-child').exists():
    subprocess.Popen([sys.executable, __file__, '--child'], start_new_session=True,
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
sys.exit(0)
'''


def wait_for(predicate, message, timeout=5):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        if predicate():
            return
        time.sleep(.02)
    raise AssertionError(message)


def read(path):
    return json.loads(path.read_text())


with tempfile.TemporaryDirectory(prefix='novelty-process-tests-') as temporary:
    base = Path(temporary)
    env = dict(os.environ, XDG_CONFIG_HOME=str(base / 'config'))
    supervisors = []
    children = []
    try:
        def fixture(name):
            root = base / name
            game = root / 'Game'
            game.mkdir(parents=True)
            exe = game / 'game.exe'
            exe.touch()
            runner = root / 'runner'
            runner.mkdir()
            (runner / 'proton').touch()
            (runner / 'version').write_text('fixture')
            fake = root / 'fake-umu'
            fake.write_text(f'#!{sys.executable}\n' + BACKEND)
            fake.chmod(0o700)
            subprocess.run([binary, '--root', root, '--list'], check=True, env=env, capture_output=True)
            ident = str(uuid.uuid4())
            config = dict(schema=1, id=ident, name='Game', folder=str(game), executable=str(exe),
                          cwd=str(game), runner=str(runner), runner_revision='fixture', backend='umu')
            with sqlite3.connect(root / '.novelty/library.sqlite') as db:
                db.execute('insert into games values(?,?,?)', (ident, str(game), json.dumps(config)))
            (root / '.novelty/settings.json').write_text(json.dumps(dict(schema=1, umu=str(fake))))
            prefix = root / '.novelty/prefixes' / ident
            prefix.mkdir(parents=True)
            (prefix / 'save.dat').write_bytes(b'existing save')
            state = root / '.novelty/installations' / ident
            return root, ident, prefix, state

        def launch(root, ident, prepare=True, tool=None):
            args = [binary, '--root', root, '--prepare' if prepare else '--run', ident]
            if tool: args += ['--prefix-tool', tool]
            process = subprocess.Popen(args,
                                       env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                                       start_new_session=True)
            supervisors.append(process)
            return process

        def command(root, *args):
            return subprocess.run([binary, '--root', root, *args], env=env, capture_output=True, timeout=3)

        def child(prefix):
            wait_for(lambda: (prefix / 'child.pid').exists() and (prefix / 'child.pid').stat().st_size > 0,
                     'detached child did not start')
            pid = int((prefix / 'child.pid').read_text())
            children.append(pid)
            return pid

        root, ident, prefix, state = fixture('Normal descendants')
        (prefix / 'spawn-child').touch()
        process = launch(root, ident)
        child(prefix)
        time.sleep(.15)  # The fake backend exits; its detached child is still running.
        assert process.poll() is None, 'supervisor exited before descendant'
        assert read(state / 'setup.json')['state'] == 'running', 'premature setup completion'
        assert command(root, '--prepare', ident).returncode == 2, 'duplicate preparation allowed'
        (prefix / 'release').touch()
        assert process.wait(timeout=5) == 0
        assert not (state / 'runtime.json').exists(), 'normal completion kept crash barrier'
        assert read(state / 'setup.json')['state'] == 'complete'

        # Headless Play suggests paths; only confirmed mappings authorize snapshots.
        save_root, save_id, save_prefix, save_state = fixture('Headless after-exit saves')
        assert command(save_root, '--prepare', save_id).returncode == 0
        (save_prefix / 'write-game-save').touch()
        assert command(save_root, '--run', save_id).returncode == 0
        save_config = read(save_state / 'saves.json')
        assert save_config['mappings'] == [], 'headless Play confirmed a heuristic without consent'
        assert save_config['candidates'] == [{'scope': 'game', 'path': 'UserData'}]
        assert not (save_state / 'save-head.json').exists(), 'unconfirmed saves were captured'
        save_config.update(mappings=[{'scope': 'game', 'path': 'UserData'}],
                           discovery_state='identified', discovery_source='manual', candidates=[])
        (save_state / 'saves.json').write_text(json.dumps(save_config))
        assert command(save_root, '--run', save_id).returncode == 0
        save_head = read(save_state / 'save-head.json')['snapshot_id']
        save_manifest = read(save_state / 'snapshots' / save_head / 'manifest.json')
        assert save_manifest['files'] == 1 and save_manifest['items'][0]['path'] == 'UserData', 'headless Play did not capture its save'
        assert (save_state / 'snapshots' / save_head / 'items' / '0' / 'slot.sav').read_bytes() == b'headless saved progress'

        # Imported peer history must never block or replace ordinary headless Play.
        conflict_id = str(uuid.uuid4())
        conflict_folder = save_state / 'snapshots' / conflict_id
        shutil.copytree(save_state / 'snapshots' / save_head, conflict_folder)
        conflict_manifest = dict(save_manifest, id=conflict_id, profile_id=str(uuid.uuid4()), parent_ids=[])
        (conflict_folder / 'manifest.json').write_text(json.dumps(conflict_manifest))
        (save_prefix / 'write-game-save').unlink()
        assert command(save_root, '--run', save_id).returncode == 0, 'headless Play must ignore imported peer history'
        # Changes made outside the launcher stay local during ordinary Play.
        conflict_manifest['parent_ids'] = [save_head]
        (conflict_folder / 'manifest.json').write_text(json.dumps(conflict_manifest))
        live_save = save_root / 'Game' / 'UserData' / 'slot.sav'
        live_save.write_bytes(b'progress made outside Novelty')
        assert command(save_root, '--run', save_id).returncode == 0, 'ordinary headless Play must not apply an ahead peer save'
        assert live_save.read_bytes() == b'progress made outside Novelty'
        assert command(save_root, '--run', save_id, '--peer-checked').returncode == 0, 'legacy shortcut marker remains compatible'

        # Kill the supervisor during a real game invocation, leaving its descendant alive.
        (prefix / 'release').unlink()
        (prefix / 'child.pid').unlink()
        process = launch(root, ident, prepare=False)
        pid = child(prefix)
        os.killpg(process.pid, signal.SIGKILL)  # Force-close the caller's whole process group.
        process.wait(timeout=3)
        os.kill(pid, 0)  # Surviving child, now adopted by this fixture driver.
        assert (state / 'runtime.json').exists(), 'killed caller lost the live monitor barrier'
        marker = (state / 'runtime.json').read_bytes()
        assert command(root, '--run', ident).returncode == 2
        assert command(root, '--prepare', ident).returncode == 2
        assert (state / 'runtime.json').read_bytes() == marker, 'blocked retry changed recovery evidence'
        (prefix / 'release').touch()
        wait_for(lambda: not (state / 'runtime.json').exists(), 'monitor did not clear barrier after descendants exited')
        children.remove(pid)
        (prefix / 'spawn-child').unlink()
        assert command(root, '--prepare', ident).returncode == 0, 'same-boot recovery refused after monitor completion'
        assert (prefix / 'save.dat').read_bytes() == b'existing save'

        assert command(root, '--debug-wine').returncode == 1, 'debug option accepted without a game action'
        assert command(root, '--prepare', ident, '--debug-wine').returncode == 1, 'debug option accepted with preparation'
        assert command(root, '--run', ident, '--debug-wine').returncode == 0, 'debug CLI game launch failed'
        assert 'One-launch Wine debug' in (state / 'launch.log').read_text(), 'debug CLI did not mark the log'

        # Kill during preparation after system.reg exists: no successful setup receipt.
        root, ident, prefix, state = fixture('Killed preparation')
        (prefix / 'spawn-child').touch()
        process = launch(root, ident)
        pid = child(prefix)
        assert (prefix / 'system.reg').exists()
        process.kill()
        process.wait(timeout=3)
        assert read(state / 'setup.json')['state'] == 'running'
        assert not (state / 'recipe.lock.json').exists()
        assert command(root, '--prepare', ident).returncode == 2
        assert (prefix / 'attempts').read_text() == 'prepare\n'
        assert (prefix / 'save.dat').read_bytes() == b'existing save'
        (prefix / 'release').touch()
        wait_for(lambda: not (state / 'runtime.json').exists(), 'preparation monitor did not finish')
        children.remove(pid)
        root, ident, prefix, state = fixture('Prefix tool descendants')
        assert command(root, '--prepare', ident).returncode == 0
        assert command(root, '--prefix-tool=winecfg').returncode == 1, 'tool accepted without an installation action'
        assert command(root, '--prepare', ident, '--prefix-tool=winecfg').returncode == 1
        assert command(root, '--run=' + ident, '--prefix-tool=regedit').returncode == 0, 'headless equals syntax failed'
        (prefix / 'spawn-child').touch()
        process = launch(root, ident, prepare=False, tool='winecfg')
        pid = child(prefix)
        assert read(state / 'status.json')['state'] == 'Configuring'
        assert process.poll() is None, 'tool descendant escaped supervision'
        assert command(root, '--run', ident).returncode == 2, 'game launched while tool was open'
        assert command(root, '--prepare', ident).returncode == 2, 'preparation ran while tool was open'
        assert command(root, '--run', ident, '--prefix-tool=regedit').returncode == 2
        process.kill()
        process.wait(timeout=3)
        os.kill(pid, 0)
        assert command(root, '--run', ident, '--prefix-tool=regedit').returncode == 2, 'tool bypassed crash barrier'
        assert (state / 'runtime.json').exists(), 'prefix tool lost its monitor barrier'
        (prefix / 'release').touch()
        wait_for(lambda: not (state / 'runtime.json').exists(), 'prefix tool monitor did not finish')
        children.remove(pid)
        assert (prefix / 'save.dat').read_bytes() == b'existing save'
        root, ident, prefix, state = fixture('Extra executable descendants')
        patch = base / 'Patches 日本語' / 'patch.exe'
        patch.parent.mkdir(); patch.touch()
        args = ['space inside', '$(do-not-execute)', '--option=value', '', '日本語']
        extra_args = ['--run=' + ident, '--extra-exe=' + str(patch)] + ['--extra-arg=' + arg for arg in args]
        assert command(root, '--extra-exe=' + str(patch)).returncode == 1
        assert command(root, '--run', ident, '--extra-arg=value').returncode == 1
        assert command(root, '--run', ident, '--extra-exe=' + str(patch), '--prefix-tool=winecfg').returncode == 1
        assert command(root, *extra_args).returncode == 0
        assert read(prefix / 'invocation.json') == dict(args=[str(patch)] + args, cwd=str(patch.parent)), 'CLI changed literal arguments/default cwd'
        completed = read(state / 'extra-launch.json')
        assert completed['state'] == 'complete' and completed['arguments'] == args
        assert command(root, '--run', ident).returncode == 0
        assert read(prefix / 'invocation.json')['args'] == [str(root / 'Game/game.exe')], 'extra executable became the saved launch'
        assert read(state / 'extra-launch.json') == completed
        (prefix / 'spawn-child').touch()
        process = subprocess.Popen([binary, '--root', root, *extra_args], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        supervisors.append(process)
        pid = child(prefix)
        assert process.poll() is None and read(state / 'extra-launch.json')['state'] == 'running'
        assert command(root, '--run', ident).returncode == 2
        assert command(root, '--prepare', ident).returncode == 2
        process.kill(); process.wait(timeout=3)
        os.kill(pid, 0)
        operation = (state / 'extra-launch.json').read_bytes()
        assert command(root, *extra_args).returncode == 2
        assert (state / 'extra-launch.json').read_bytes() == operation, 'blocked retry replaced interrupted evidence'
        assert read(state / 'extra-launch.json.previous') == completed
        assert (state / 'runtime.json').exists(), 'extra executable lost its monitor barrier'
        (prefix / 'release').touch()
        wait_for(lambda: not (state / 'runtime.json').exists(), 'extra executable monitor did not finish')
        children.remove(pid)
        assert (prefix / 'save.dat').read_bytes() == b'existing save'
        (prefix / 'spawn-child').unlink()
        assert command(root, '--run', ident).returncode == 0
        assert read(state / 'extra-launch.json')['state'] == 'interrupted', 'normal recovery revived stale operation references'
        assert read(prefix / 'invocation.json')['args'] == [str(root / 'Game/game.exe')], 'recovery automatically retried a patch'
        root, ident, prefix, state = fixture('New installer descendants')
        source = base / 'External installer' / 'setup.exe'; source.parent.mkdir(); source.touch()
        with sqlite3.connect(root / '.novelty/library.sqlite') as db:
            config = json.loads(db.execute('select config from games where id=?', (ident,)).fetchone()[0])
            config.update(install_pending=True, installer=str(source), executable='')
            db.execute('update games set config=? where id=?', (json.dumps(config), ident))
        (root / 'Game/game.exe').unlink()
        assert command(root, '--install', ident, '--run', ident).returncode == 1
        assert command(root, '--install', ident, '--extra-exe=' + str(source)).returncode == 1
        assert command(root, '--run', ident).returncode == 1, 'pending installation accepted Play'
        assert command(root, '--prepare', ident).returncode == 0
        (prefix / 'spawn-child').touch()
        process = subprocess.Popen([binary, '--root', root, '--install=' + ident], env=env,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        supervisors.append(process); pid = child(prefix)
        assert read(state / 'extra-launch.json')['kind'] == 'installer'
        assert (prefix / 'dosdevices/n:').resolve() == root / 'Game'
        assert read(prefix / 'invocation.json') == dict(args=[str(source)], cwd=str(source.parent))
        process.kill(); process.wait(timeout=3)
        operation = (state / 'extra-launch.json').read_bytes()
        assert command(root, '--install', ident).returncode == 2, 'installer bypassed crash barrier'
        assert (state / 'extra-launch.json').read_bytes() == operation
        (prefix / 'release').touch()
        wait_for(lambda: not (state / 'runtime.json').exists(), 'installer monitor did not finish')
        children.remove(pid)
        assert (prefix / 'save.dat').read_bytes() == b'existing save'
        print('PASS: detached descendants, same-boot monitor recovery after caller SIGKILL, setup/game/tools/extras/installers and save preservation')
    finally:
        for process in supervisors:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=3)
            process.stderr.close()
        for pid in children:
            try:
                os.kill(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                os.waitpid(pid, 0)
            except ChildProcessError:
                pass
