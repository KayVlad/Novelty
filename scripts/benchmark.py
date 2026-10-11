#!/usr/bin/env python3
"""Measure the release frontend offscreen with a temporary 500-game library."""
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import signal
import sys
import tempfile
import time
import uuid

project = Path(__file__).resolve().parents[1]
binary = project / "build/novelty"
running = "--running" in sys.argv[1:]
with tempfile.TemporaryDirectory(prefix="novelty-benchmark-") as directory:
    root = Path(directory) / "Games"
    root.mkdir()
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", XDG_CONFIG_HOME=str(Path(directory) / "config"))
    subprocess.run([str(binary), "--root", str(root), "--list"], check=True, env=env)
    database = sqlite3.connect(root / ".novelty/library.sqlite")
    first_id = None
    runner = root / ".novelty/benchmark-runner"
    fake = root / ".novelty/benchmark-umu"
    if running:
        runner.mkdir()
        (runner / "proton").touch()
        (runner / "version").write_text("benchmark")
        fake.write_text('#!/bin/sh\nif [ "$1" = "" ]; then mkdir -p "$WINEPREFIX"; touch "$WINEPREFIX/system.reg"; exit 0; fi\ntouch "$WINEPREFIX/running"\nexec sleep 30\n')
        fake.chmod(0o700)
        database.execute("insert into settings values(?,?)", ("app", json.dumps({"umu": str(fake)})))
    for number in range(500):
        folder = root / f"Game {number:03}"
        folder.mkdir()
        executable = folder / "game.exe"
        executable.touch()
        ident = str(uuid.uuid4())
        config = {"schema": 1, "id": ident, "name": folder.name, "folder": str(folder), "executable": str(executable), "cwd": str(folder), "backend": "umu", "engine": "Generic"}
        if number == 0:
            first_id = ident
            if running:
                config.update(runner=str(runner), runner_revision="benchmark")
        database.execute("insert into games values(?,?,?)", (ident, str(folder), json.dumps(config)))
    database.commit()
    database.close()
    process = backend = None
    try:
        if running:
            backend = subprocess.Popen([str(binary), "--root", str(root), "--run", first_id], env=env, start_new_session=True)
            until = time.monotonic() + 5
            while not (root / ".novelty/prefixes" / first_id / "running").exists():
                if backend.poll() is not None or time.monotonic() > until:
                    raise RuntimeError("Benchmark backend failed to start")
                time.sleep(.02)
        process = subprocess.Popen([str(binary), "--root", str(root), "--startup-report"], env=env, stderr=subprocess.PIPE)
        # Header reports the first queued UI event, before waiting on discovery.
        print(process.stderr.readline().decode().strip())
        time.sleep(1)
        status = Path(f"/proc/{process.pid}/status").read_text()
        print(next(line for line in status.splitlines() if line.startswith("VmRSS:")))
        start = Path(f"/proc/{process.pid}/stat").read_text().split()
        time.sleep(2)
        end = Path(f"/proc/{process.pid}/stat").read_text().split()
        print("Frontend CPU ticks over 2 seconds:", int(end[13]) + int(end[14]) - int(start[13]) - int(start[14]))
        print("500 entries;", "monitoring a fake running game;" if running else "idle;", "offscreen, not a cold boot or Gaming Mode benchmark.")
    finally:
        if process:
            process.terminate()
            process.wait(timeout=5)
            process.stderr.close()
        if backend:
            # Only the process group created above for this disposable fixture.
            try:
                os.killpg(backend.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            backend.wait(timeout=5)
