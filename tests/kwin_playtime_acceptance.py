"""Opt-in real KWin 6 desktop test. Temporarily activates its own disposable windows.
Usage: python3 tests/kwin_playtime_acceptance.py build/novelty build/novelty_kwin_window_fixture
Requires qdbus6 and a KDE desktop. Set NOVELTY_ACCEPTANCE_QPA=xcb to test XWayland.
Does not test real Proton, suspend, or Game Mode.
"""
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
import uuid
binary, fixture = map(lambda p: str(Path(p).resolve()), sys.argv[1:])
qdbus = shutil.which('qdbus6') or shutil.which('qdbus')
assert qdbus, 'qdbus6 is required for the opt-in acceptance driver'

def call(path, method, *args):
    return subprocess.check_output([qdbus,'org.kde.KWin',path,method,*map(str,args)],text=True,timeout=5).strip()
assert 'KWin version: 6.' in call('/KWin','org.kde.KWin.supportInformation')

def wait(predicate, message, timeout=7):
    until=time.monotonic()+timeout
    while time.monotonic()<until:
        if predicate(): return
        time.sleep(.05)
    raise AssertionError(message)

with tempfile.TemporaryDirectory(prefix='novelty-real-kwin-') as directory:
    base=Path(directory); callers=[]; games=[]
    env=dict(os.environ,NOVELTY_TEST_APP_DATA_DIR=str(base/'app'),QT_QPA_PLATFORM=os.environ.get('NOVELTY_ACCEPTANCE_QPA','wayland'))
    env.pop('STEAM_GAMEPADUI',None); env.pop('GAMESCOPE_WAYLAND_DISPLAY',None)
    env.update(XDG_CURRENT_DESKTOP='KDE',XDG_SESSION_DESKTOP='KDE')
    def script(source):
        name='novelty-acceptance-'+str(uuid.uuid4()); path=base/(name+'.js'); path.write_text(source)
        ident=call('/Scripting','org.kde.kwin.Scripting.loadScript',path,name)
        try: call('/Scripting/Script'+ident,'org.kde.kwin.Script.run')
        finally: call('/Scripting','org.kde.kwin.Scripting.unloadScript',name)
    try:
        other=base/'Other'; other.mkdir(); games.append(other)
        unrelated=subprocess.Popen([fixture],cwd=other,env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL); callers.append(unrelated)
        wait(lambda:(other/'pid').exists(),'unrelated window did not start'); other_pid=int((other/'pid').read_text())
        for backend in ('native','umu'):
            root=base/backend; game=root/'Game'; game.mkdir(parents=True); games.append(game)
            assert subprocess.run([binary,'--root',root,'--list'],env=env,capture_output=True).returncode == 0
            ident=str(uuid.uuid4())
            config=dict(schema=1,id=ident,name='KWin fixture',folder=str(game),executable=fixture,cwd=str(game),backend=backend)
            # Native executables must live inside the installation folder.
            native=game/'play'; shutil.copy2(fixture,native)
            config['executable']=str(native)
            if backend=='umu':
                runner=root/'GE-Proton99-1'; runner.mkdir(); (runner/'proton').touch(); (runner/'version').write_text('fixture')
                wrapper=root/'umu'
                wrapper.write_text(f'#!{sys.executable}\nimport os,pathlib,subprocess,sys\np=pathlib.Path(os.environ["WINEPREFIX"])\nif len(sys.argv)>2 and sys.argv[2]=="/c":\n (p/"drive_c/windows/system32").mkdir(parents=True,exist_ok=True)\n (p/"system.reg").touch()\n sys.exit(0)\nc=subprocess.Popen([{fixture!r}],start_new_session=True)\nc.wait()\n')
                wrapper.chmod(0o700); exe=game/'game.exe'; exe.touch()
                config.update(executable=str(exe),runner=str(runner),runner_revision='fixture')
                (root/'.novelty/settings.json').write_text(json.dumps(dict(schema=1,umu=str(wrapper))))
            with sqlite3.connect(root/'.novelty/library.sqlite') as db: db.execute('insert into games values(?,?,?)',(ident,str(game),json.dumps(config)))
            caller=subprocess.Popen([binary,'--root',root,'--run',ident],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE); callers.append(caller)
            wait(lambda:(game/'pid').exists(),f'{backend} fixture did not start'); pid=int((game/'pid').read_text()); time.sleep(.5)
            select=f'var windows=workspace.windowList().filter(function(w){{return w.pid==={pid};}});'
            script(select+"windows.forEach(function(w){w.minimized=false;}); workspace.activeWindow=windows[0];")
            time.sleep(1.2)
            script(select+"windows.forEach(function(w){w.minimized=true;});")
            time.sleep(1.5)
            script(select+"windows[1].minimized=false; workspace.activeWindow=windows[1];")
            time.sleep(1.2)
            caller.kill(); caller.wait(timeout=3) # Detached monitor survives its caller.
            script(select+"windows[0].minimized=false; windows[0].fullScreen=true; workspace.activeWindow=windows[0];")
            time.sleep(1.2)
            script(f'var other=workspace.windowList().filter(function(w){{return w.pid==={other_pid};}}); workspace.activeWindow=other[0];')
            time.sleep(1.5)
            (game/'release').touch()
            def complete():
                with sqlite3.connect(root/'.novelty/library.sqlite') as db:
                    row=db.execute('select state from play_sessions').fetchone(); return row and row[0]=='completed'
            wait(complete,'detached KWin monitor did not complete')
            with sqlite3.connect(root/'.novelty/library.sqlite') as db:
                opened=db.execute('select elapsed_ms from play_sessions').fetchone()[0]
                foreground,status=db.execute('select foreground_ms,status from play_focus').fetchone()
            logs='\n'.join(p.read_text(errors='replace') for p in (root/'.novelty').rglob('*.log'))
            assert 2200 < foreground < 5800 and opened >= foreground+2000,(backend,opened,foreground,status,logs)
            print(f'PASS real KWin {env["QT_QPA_PLATFORM"]} {backend}: opened={opened} ms foreground={foreground} ms; windowed/fullscreen, minimize/restore, two windows, unrelated PID and caller closure')
    finally:
        for game in games: (game/'release').touch()
        for p in callers:
            if p.poll() is None:
                try: p.wait(timeout=3)
                except subprocess.TimeoutExpired: p.terminate(); p.wait(timeout=3)
