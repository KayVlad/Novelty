"""Real monitor/SQLite/D-Bus tests on a private bus, without a compositor."""
import ctypes
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
binary, driver, mock = map(lambda p: str(Path(p).resolve()), sys.argv[1:])
assert ctypes.CDLL(None).prctl(36, 1, 0, 0, 0) == 0

def wait(predicate, message, timeout=7):
    until = time.monotonic()+timeout
    while time.monotonic() < until:
        value = predicate()
        if value: return value
        time.sleep(.04)
    raise AssertionError(message)

with tempfile.TemporaryDirectory(prefix='novelty-foreground-') as directory:
    base = Path(directory); state_file = base/'state.json'
    env = dict(os.environ, NOVELTY_TEST_APP_DATA_DIR=str(base/'app'), NOVELTY_TEST_FOCUS_STATE=str(state_file))
    for name in ('STEAM_GAMEPADUI', 'GAMESCOPE_WAYLAND_DISPLAY'): env.pop(name, None)
    env.update(XDG_CURRENT_DESKTOP='KDE', XDG_SESSION_DESKTOP='KDE')
    def state(**changes):
        value = dict(known=True, active=True, minimized=False, hidden=False, id='window-a', pid=0)
        value.update(changes); temporary = state_file.with_suffix('.new'); temporary.write_text(json.dumps(value)); temporary.replace(state_file)
    state(active=False)
    server = subprocess.Popen([mock],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    assert server.stdout.readline().strip() == 'ready'
    callers = []
    try:
        def fixture(name, umu=False):
            root = base/name; game = root/'Game'; game.mkdir(parents=True)
            assert subprocess.run([binary,'--root',root,'--list'],env=env,capture_output=True).returncode == 0
            program = game/'game.py'
            # A wrapper starts a detached child, as a Proton invocation can do.
            program.write_text(f'#!{sys.executable}\nimport os,pathlib,time\np=pathlib.Path.cwd()\n(p/"pid").write_text(str(os.getpid()))\nwhile not (p/"release").exists(): time.sleep(.02)\n')
            program.chmod(0o700); ident = str(uuid.uuid4())
            config = dict(schema=1,id=ident,name='Fixture',folder=str(game),executable=str(program),cwd=str(game),backend='native')
            if umu:
                runner = root/'GE-Proton99-1'; runner.mkdir(); (runner/'proton').touch(); (runner/'version').write_text('fixture')
                wrapper = root/'umu'
                wrapper.write_text(f'#!{sys.executable}\nimport os,pathlib,subprocess,sys\np=pathlib.Path(os.environ["WINEPREFIX"])\nif len(sys.argv)>2 and sys.argv[2]=="/c":\n (p/"drive_c/windows/system32").mkdir(parents=True,exist_ok=True)\n (p/"system.reg").touch()\n sys.exit(0)\nc=subprocess.Popen([{str(program)!r}],start_new_session=True)\nc.wait()\n')
                wrapper.chmod(0o700); exe=game/'game.exe'; exe.touch()
                config.update(backend='umu',executable=str(exe),runner=str(runner),runner_revision='fixture')
                (root/'.novelty/settings.json').write_text(json.dumps(dict(schema=1,umu=str(wrapper))))
            with sqlite3.connect(root/'.novelty/library.sqlite') as db: db.execute('insert into games values(?,?,?)',(ident,str(game),json.dumps(config)))
            return root, game, ident
        def rows(root, table):
            with sqlite3.connect(root/'.novelty/library.sqlite',timeout=1) as db:
                db.row_factory=sqlite3.Row
                if not db.execute('select 1 from sqlite_master where name=?',(table,)).fetchone(): return []
                return [dict(r) for r in db.execute('select * from '+table)]
        def run(root,ident,run_env=env):
            p=subprocess.Popen([binary,'--root',root,'--run',ident],env=run_env,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
            callers.append(p); return p
        def stats(root, ident):
            p=subprocess.run([driver,'stats',root,ident],env=env,capture_output=True,timeout=5)
            assert p.returncode == 0,p.stderr
            return json.loads(p.stdout)
        # Native focus gain/loss/minimization/restore and unverified bridge outage.
        root, game, ident = fixture('Native')
        state(pid_file=str(game/'pid'))
        caller=run(root,ident); wait(lambda:(game/'pid').exists(),'native process did not start')
        time.sleep(1)
        owner=rows(root,'play_sessions')[0]['owner_pid']
        spoof=subprocess.run(['dbus-send','--session','--print-reply','--dest=org.novelty.Focus.p'+str(owner),'/Focus','org.novelty.Focus.Changed','string:'+json.dumps(dict(known=True,active=True,minimized=False,pid=int((game/'pid').read_text()),id='spoof'))],env=env,capture_output=True,timeout=3)
        assert spoof.returncode != 0 and b'AccessDenied' in spoof.stderr, spoof
        state(pid_file=str(game/'pid'),minimized=True); time.sleep(.5)
        state(pid_file=str(game/'pid'),active=False); time.sleep(.5)
        state(pid_file=str(game/'pid'),locked=True); time.sleep(.5)
        state(pid_file=str(game/'pid'),id='window-b'); time.sleep(1)
        state(pid_file=str(game/'pid'),known=False); time.sleep(.5)
        state(pid_file=str(game/'pid')); time.sleep(.5)
        state(stop=True); time.sleep(2)
        state(pid_file=str(game/'pid')); time.sleep(.5)
        (game/'release').touch(); assert caller.wait(timeout=6) == 0,caller.stderr.read()
        session=rows(root,'play_sessions')[0]; focus=rows(root,'play_focus')[0]
        assert 1200 < focus['foreground_ms'] < 5500,(session,focus)
        assert session['elapsed_ms'] > focus['foreground_ms'] and focus['observed_ms'] >= focus['foreground_ms']
        assert sum(r['elapsed_ms'] for r in rows(root,'play_focus_days')) == focus['foreground_ms']
        assert stats(root,ident)['foreground_ms'] == focus['foreground_ms']
        assert stats(root,ident)['foreground_ms'] == stats(root,ident)['foreground_ms']
        # An actual UMU-style child remains supervised after the headless/GUI caller dies.
        proton, pg, pi = fixture('Proton',True); state(pid_file=str(pg/'pid'))
        caller=run(proton,pi); wait(lambda:(pg/'pid').exists(),'Proton child did not start')
        wait(lambda:rows(proton,'play_sessions'),'no Proton session'); time.sleep(.6)
        caller.kill(); caller.wait(timeout=3); time.sleep(.6)
        assert stats(proton,pi)['active']
        # Wait for a real 30s FULL-synchronous dual-counter checkpoint, then kill the monitor.
        wait(lambda:rows(proton,'play_sessions')[0]['elapsed_ms'] >= 30000,'dual checkpoint missing',timeout=33)
        session=rows(proton,'play_sessions')[0]; focus=rows(proton,'play_focus')[0]; daily=rows(proton,'play_focus_days')
        assert session['elapsed_ms'] >= focus['foreground_ms'] > 20000,(session,focus)
        assert sum(r['elapsed_ms'] for r in daily) == focus['foreground_ms']
        os.kill(session['owner_pid'],signal.SIGKILL); (pg/'release').touch(); time.sleep(.3)
        recovered=stats(proton,pi)
        assert not recovered['active'] and recovered['foreground_ms'] == focus['foreground_ms']
        assert rows(proton,'play_sessions')[0]['state'] == 'interrupted'
        assert rows(proton,'play_focus') == [focus] and rows(proton,'play_focus_days') == daily
        assert stats(proton,pi)['foreground_ms'] == recovered['foreground_ms']
        # Game Mode is deliberately unavailable, even when a KWin service exists.
        deck,dg,di=fixture('Game Mode'); state(pid_file=str(dg/'pid'))
        deck_env=dict(env,XDG_CURRENT_DESKTOP='gamescope',NOVELTY_GAMESCOPE_DIAGNOSTICS='1')
        tools=base/'tools'; tools.mkdir(); xprop=tools/'xprop'
        xprop.write_text('#!/bin/sh\ncase "$1" in\n-root) echo "GAMESCOPE_FOCUSED_WINDOW(CARDINAL) = 123"; echo "GAMESCOPE_FOCUSED_APP(CARDINAL) = 456";;\n*) echo "_NET_WM_PID(CARDINAL) = 789"; echo "STEAM_GAME(CARDINAL) = 456";;\nesac\n'); xprop.chmod(0o700)
        deck_env['PATH']=str(tools)+os.pathsep+env['PATH']
        caller=run(deck,di,deck_env); wait(lambda:(dg/'pid').exists(),'Game Mode process did not start'); time.sleep(.7)
        (dg/'release').touch(); assert caller.wait(timeout=5) == 0
        assert rows(deck,'play_sessions')[0]['elapsed_ms'] >= 600
        assert rows(deck,'play_focus')[0]['foreground_ms'] == 0
        logs='\n'.join(p.read_text(errors='replace') for p in (deck/'.novelty').rglob('*.log'))
        assert 'Gamescope diagnostic' in logs and 'GAMESCOPE_FOCUSED_APP' in logs and '_NET_WM_PID' in logs, logs
        # A disappearing compositor, then a missing compositor, never grows foreground.
        gone,gg,gi=fixture('Lost compositor'); state(pid_file=str(gg/'pid'))
        caller=run(gone,gi); wait(lambda:(gg/'pid').exists(),'disconnect fixture did not start'); time.sleep(.7)
        server.terminate(); server.wait(timeout=3); time.sleep(1)
        (gg/'release').touch(); assert caller.wait(timeout=5) == 0
        focus=rows(gone,'play_focus')[0]
        assert 0 < focus['foreground_ms'] < 1200 and rows(gone,'play_sessions')[0]['elapsed_ms'] > focus['foreground_ms']
        missing,mg,mi=fixture('No compositor'); caller=run(missing,mi)
        wait(lambda:(mg/'pid').exists(),'unsupported fixture did not start'); time.sleep(.4); (mg/'release').touch()
        assert caller.wait(timeout=5) == 0 and rows(missing,'play_focus')[0]['foreground_ms'] == 0
        print('PASS: private-bus focus edges, minimization, multiple windows, native/UMU children, caller closure, 30s atomic checkpoints, SIGKILL recovery, persistence, Game Mode exclusion and diagnostic IDs/PIDs')
    finally:
        for path in base.glob('*/Game'): (path/'release').touch()
        for p in callers:
            if p.poll() is None: p.kill(); p.wait(timeout=3)
        server.terminate(); server.wait(timeout=3)
        until=time.monotonic()+3
        while time.monotonic()<until:
            try:
                pid,_=os.waitpid(-1,os.WNOHANG)
                if not pid: time.sleep(.02)
            except ChildProcessError: break
