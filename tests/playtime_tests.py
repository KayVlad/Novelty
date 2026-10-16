"""Disposable process/SQLite regressions, not real-device acceptance."""
import ctypes
from datetime import datetime
import json
import os
from pathlib import Path
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time
import uuid

binary, driver, clock_library = map(lambda p: str(Path(p).resolve()), sys.argv[1:])
assert ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0) == 0
BACKEND = r'''import os, pathlib, signal, subprocess, sys, time
folder = pathlib.Path.cwd()
args = sys.argv[1:]
if 'WINEPREFIX' in os.environ:
    prefix = pathlib.Path(os.environ['WINEPREFIX'])
    if len(args) > 1 and args[1] == '/c':
        (prefix / 'drive_c/windows/system32').mkdir(parents=True, exist_ok=True)
        (prefix / 'system.reg').touch()
        (prefix / 'drive_c/windows/system32/winecfg.exe').touch()
        (prefix / 'drive_c/windows/regedit.exe').touch()
        time.sleep(.7)
        sys.exit(0)
    if args and (args[0] == 'winetricks' or args[0].endswith(('winecfg.exe', 'regedit.exe', 'patch.exe', 'setup.exe'))):
        time.sleep(.3)
        sys.exit(0)
mode = (folder / 'mode').read_text() if (folder / 'mode').exists() else 'quick'
(folder / 'game-started').write_text(str(os.getpid()))
if mode == 'descendant' and '--child' not in args:
    subprocess.Popen([sys.executable, __file__, '--child'], start_new_session=True,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
    sys.exit(0)
if mode in ('hold', 'descendant'):
    while not (folder / 'release').exists(): time.sleep(.02)
elif mode == 'clock':
    time.sleep(.25)
    offset = pathlib.Path(os.environ['NOVELTY_TEST_WALLCLOCK_OFFSET'])
    offset.write_text('172800')
    time.sleep(.25)
    offset.write_text('-172800')
    time.sleep(.25)
else:
    time.sleep(.4)
if mode == 'fail': sys.exit(7)
if mode == 'crash': os.kill(os.getpid(), signal.SIGKILL)
'''

def wait_for(predicate, message, timeout=6):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        value = predicate()
        if value: return value
        time.sleep(.025)
    raise AssertionError(message)

def rows(root):
    with sqlite3.connect(root / '.novelty/library.sqlite', timeout=1) as db:
        db.row_factory = sqlite3.Row
        if not db.execute("select 1 from sqlite_master where name='play_sessions'").fetchone(): return []
        return [dict(row) for row in db.execute('select * from play_sessions order by rowid')]

with tempfile.TemporaryDirectory(prefix='novelty-playtime-') as directory:
    base = Path(directory)
    env = dict(os.environ, NOVELTY_TEST_APP_DATA_DIR=str(base / 'app'))
    callers = []
    owned_pids = {}
    try:
        def command(root, *args, extra_env=None):
            return subprocess.run([binary, '--root', root, *args], env=extra_env or env,
                                  capture_output=True, timeout=8)
        def fixture(name, backend='native', ident=None):
            root = base / name
            folder = root / 'Game'
            folder.mkdir(parents=True)
            assert command(root, '--list').returncode == 0
            ident = ident or str(uuid.uuid4())
            program = folder / 'play.py' if backend == 'native' else root / 'fake-umu'
            program.write_text(f'#!{sys.executable}\n' + BACKEND); program.chmod(0o700)
            executable = program if backend == 'native' else folder / 'game.exe'
            if backend != 'native': executable.touch()
            runner = root / 'GE-Proton99-1'; runner.mkdir(); (runner / 'proton').touch(); (runner / 'version').write_text('fixture')
            (runner / 'protonfixes').mkdir()
            (runner / 'protonfixes/winetricks').write_text('#!/bin/sh\nexit 0\n'); (runner / 'protonfixes/winetricks').chmod(0o700)
            config = dict(schema=1, id=ident, name='Game', folder=str(folder), executable=str(executable),
                          cwd=str(folder), backend=backend, runner=str(runner), runner_revision='fixture', steam_id='123')
            with sqlite3.connect(root / '.novelty/library.sqlite') as db:
                db.execute('insert into games values(?,?,?)', (ident, str(folder), json.dumps(config)))
            (root / '.novelty/settings.json').write_text(json.dumps(dict(schema=1, umu=str(program))))
            return root, folder, ident
        def start(root, ident):
            process = subprocess.Popen([binary, '--root', root, '--run', ident], env=env,
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
            callers.append(process)
            return process
        def stats(root, ident, action='stats', *arguments):
            result = subprocess.run([driver, action, root, ident, *arguments], env=env, capture_output=True, timeout=4)
            assert result.returncode == 0, result.stderr
            return json.loads(result.stdout)

        root, folder, ident = fixture('Native sessions')
        assert stats(root, ident)['sessions'] == 0
        for mode, code in [('quick', 0), ('fail', 1), ('crash', 1)]:
            (folder / 'mode').write_text(mode)
            result = command(root, '--run', ident)
            assert result.returncode == code, (mode, result.returncode, result.stderr)
        sessions = rows(root)
        assert len(sessions) == 3 and len({s['session_id'] for s in sessions}) == 3
        assert all(s['state'] == 'completed' and s['elapsed_ms'] >= 350 and s['ended_utc'] for s in sessions)
        assert stats(root, ident)['total_ms'] == sum(s['elapsed_ms'] for s in sessions)
        before = stats(root, ident)
        moved = root / 'Relinked'; folder.rename(moved)
        assert stats(root, ident, 'relink', moved)['total_ms'] == before['total_ms']
        assert stats(root, ident, 'runner', root / 'GE-Proton99-1')['sessions'] == 3
        (moved / 'mode').write_text('quick')
        assert command(root, '--run', ident).returncode == 0 and len(rows(root)) == 4
        other, other_folder, _ = fixture('Independent device library', ident=ident)
        assert stats(other, ident)['sessions'] == 0
        assert command(other, '--run', ident).returncode == 0 and len(rows(other)) == 1
        assert len(rows(root)) == 4 and rows(root)[0]['session_id'] != rows(other)[0]['session_id']

        umu, umu_folder, umu_id = fixture('UMU exclusions', 'umu')
        assert command(umu, '--prepare', umu_id).returncode == 0 and not rows(umu)
        for tool in ('winecfg', 'regedit'):
            assert command(umu, '--run', umu_id, '--prefix-tool', tool).returncode == 0 and not rows(umu)
        patch = umu_folder / 'patch.exe'; patch.touch()
        assert command(umu, '--run', umu_id, '--extra-exe', patch).returncode == 0 and not rows(umu)
        with sqlite3.connect(umu / '.novelty/library.sqlite') as db:
            config = json.loads(db.execute('select config from games').fetchone()[0]); config['winetricks'] = ['cjkfonts']
            db.execute('update games set config=?', (json.dumps(config),))
        approval = subprocess.check_output([driver, 'approval', umu, umu_id], env=env).decode().strip()
        assert command(umu, '--prepare', umu_id, '--approve-winetricks', approval).returncode == 0 and not rows(umu)
        assert command(umu, '--run', umu_id).returncode == 0
        assert len(rows(umu)) == 1 and rows(umu)[0]['elapsed_ms'] >= 350
        # A changed UMU runner keeps the same local game/session identity.
        new_runner = umu / 'GE-Proton99-2'; new_runner.mkdir(); (new_runner / 'proton').touch(); (new_runner / 'version').write_text('new')
        (new_runner / 'protonfixes').mkdir()
        (new_runner / 'protonfixes/winetricks').write_text('#!/bin/sh\nexit 0\n'); (new_runner / 'protonfixes/winetricks').chmod(0o700)
        assert stats(umu, umu_id, 'runner', new_runner)['sessions'] == 1
        started = time.monotonic()
        approval = subprocess.check_output([driver, 'approval', umu, umu_id], env=env).decode().strip()
        assert command(umu, '--run', umu_id, '--approve-winetricks', approval).returncode == 0 and len(rows(umu)) == 2
        wall_ms = (time.monotonic() - started) * 1000
        # The second run performs preparation for the changed runner, but records only game time.
        assert 350 <= rows(umu)[1]['elapsed_ms'] < wall_ms - 500, rows(umu)

        install, install_folder, install_id = fixture('Installer excluded', 'umu')
        installer = install / 'setup.exe'; installer.touch()
        with sqlite3.connect(install / '.novelty/library.sqlite') as db:
            config = json.loads(db.execute('select config from games').fetchone()[0])
            config.update(executable='', install_pending=True, installer=str(installer))
            db.execute('update games set config=?', (json.dumps(config),))
        result = command(install, '--install', install_id)
        assert result.returncode == 0 and not rows(install), result.stderr

        steam, steam_folder, steam_id = fixture('Steam untracked', 'steam')
        steam_program = steam / 'steam'
        steam_program.write_text('#!/bin/sh\ntouch dispatched\n'); steam_program.chmod(0o700)
        (steam / '.novelty/settings.json').write_text(json.dumps(dict(schema=1, steam=str(steam_program))))
        assert command(steam, '--run', steam_id).returncode == 0
        wait_for(lambda: (steam_folder / 'dispatched').exists(), 'Steam was not dispatched')
        assert not rows(steam)

        failed, failed_folder, failed_id = fixture('Exec failure')
        (failed_folder / 'play.py').write_text('#!/nonexistent/novelty-test-interpreter\n')
        assert command(failed, '--run', failed_id).returncode != 0 and not rows(failed)
        broken, broken_folder, broken_id = fixture('Optional statistics failure')
        with sqlite3.connect(broken / '.novelty/library.sqlite') as db:
            db.execute('create table play_sessions (bad_schema text)')
        assert command(broken, '--run', broken_id).returncode == 0 and (broken_folder / 'game-started').exists()

        descendants, descendants_folder, descendants_id = fixture('Detached descendants')
        (descendants_folder / 'mode').write_text('descendant')
        caller = start(descendants, descendants_id)
        session = wait_for(lambda: rows(descendants), 'no descendant session')[0]
        owned_pids[session['owner_pid']] = session['owner_start_ticks']
        time.sleep(.5)
        assert caller.poll() is None and rows(descendants)[0]['state'] == 'active'
        caller.kill(); caller.wait(timeout=3)  # Headless caller/GUI ownership is not the detached monitor's lifetime.
        time.sleep(.3)
        assert stats(descendants, descendants_id)['active']
        (descendants_folder / 'release').touch()
        wait_for(lambda: rows(descendants)[0]['state'] == 'completed', 'detached monitor did not finalize')
        assert rows(descendants)[0]['elapsed_ms'] >= 700

        interrupted, held_folder, held_id = fixture('Checkpoint and monitor SIGKILL')
        (held_folder / 'mode').write_text('hold')
        caller = start(interrupted, held_id)
        session = wait_for(lambda: rows(interrupted), 'no active checkpoint session')[0]
        monitor = session['owner_pid']; owned_pids[monitor] = session['owner_start_ticks']
        wait_for(lambda: rows(interrupted)[0]['elapsed_ms'] >= 30000, 'periodic durable checkpoint did not advance', timeout=34)
        established = rows(interrupted)[0]['elapsed_ms']
        os.kill(monitor, signal.SIGKILL)
        assert caller.wait(timeout=4) != 0
        (held_folder / 'release').touch()
        time.sleep(.2)
        assert command(interrupted, '--list').returncode == 0
        recovered = rows(interrupted)
        assert len(recovered) == 1 and recovered[0]['state'] == 'interrupted'
        assert recovered[0]['elapsed_ms'] == established and recovered[0]['ended_utc'] is None
        assert command(interrupted, '--list').returncode == 0 and rows(interrupted) == recovered
        assert (interrupted / '.novelty/installations' / held_id / 'runtime.json').exists()
        assert command(interrupted, '--run', held_id).returncode == 2, 'statistics recovery cleared the runtime safety barrier'

        # Simulated boot change and PID reuse preserve only committed duration.
        for boot, ticks in [(str(uuid.uuid4()), session['owner_start_ticks']), (session['boot_id'], 'invalid-start-ticks')]:
            with sqlite3.connect(interrupted / '.novelty/library.sqlite') as db:
                db.execute("update play_sessions set state='active',boot_id=?,owner_pid=?,owner_start_ticks=?", (boot, os.getpid(), ticks))
            assert command(interrupted, '--list').returncode == 0
            assert rows(interrupted)[0]['elapsed_ms'] == established and rows(interrupted)[0]['state'] == 'interrupted'

        clock, clock_folder, clock_id = fixture('Wall clock jumps')
        (clock_folder / 'mode').write_text('clock')
        offset = clock / 'offset'; offset.write_text('0')
        clock_env = dict(env, LD_PRELOAD=clock_library, NOVELTY_TEST_WALLCLOCK_OFFSET=str(offset))
        assert command(clock, '--run', clock_id, extra_env=clock_env).returncode == 0
        recorded = rows(clock)[0]
        assert 650 <= recorded['elapsed_ms'] < 2000, recorded
        assert datetime.fromisoformat(recorded['ended_utc'].replace('Z', '+00:00')) < datetime.fromisoformat(recorded['started_utc'].replace('Z', '+00:00'))
        print('PASS: real game/descendant sessions, crashes, exclusions, independent libraries, identity preservation, 30-second checkpoint, interrupted/PID/boot recovery and wall-clock jumps')
    finally:
        for release in base.glob('*/Game/release'): release.touch()
        for mode in base.glob('*/Game/mode'): (mode.parent / 'release').touch()
        for process in callers:
            if process.poll() is None: process.kill(); process.wait(timeout=3)
        for pid, ticks in owned_pids.items():
            try:
                fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
                if fields[19] == ticks: os.kill(pid, signal.SIGTERM)
            except (FileNotFoundError, ProcessLookupError): pass
        until = time.monotonic() + 3
        while time.monotonic() < until:
            try:
                pid, _ = os.waitpid(-1, os.WNOHANG)
                if not pid: time.sleep(.02)
            except ChildProcessError: break
