#!/usr/bin/env python3
"""Install a verified, pinned umu zipapp side by side under a Games library."""
import argparse
import fcntl
import hashlib
import io
import os
from pathlib import Path
import signal
import sys
import tarfile
import tempfile
import urllib.request

VERSION = "1.4.4"
URL = f"https://github.com/Open-Wine-Components/umu-launcher/releases/download/{VERSION}/umu-launcher-{VERSION}-zipapp.tar"
SHA256 = "eb590691841f7fad3fc3ad8fd5db4ccb87849fe7948e62b28ece7a4ee48cc851"
ZIPAPP_SHA256 = "d0005a58602041229cc467dab03dc0c0b9e8cce09a8145b16b7683244cf17804"
LIMIT = 4 * 1024 * 1024


def install(games_folder):
    if not games_folder.is_dir():
        raise RuntimeError("Games folder must already exist")
    root = games_folder.resolve()
    tools = root / ".novelty" / "tools"
    if tools.resolve() != tools:
        raise RuntimeError("Managed tools must not be linked to another location")
    tools.mkdir(parents=True, exist_ok=True)
    target = tools / f"umu-{VERSION}"
    # Never replace an existing tool: another game may still be importing its zipapp.
    with os.fdopen(os.open(tools / f".umu-{VERSION}.lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600), "a+b") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError("This umu version is already being installed; retry shortly") from error
        if target.exists() or target.is_symlink():
            executable = target / "umu-run"
            if (target.is_symlink() or not target.is_dir() or executable.is_symlink()
                    or not executable.is_file() or executable.stat().st_size > LIMIT
                    or not os.access(executable, os.X_OK)
                    or hashlib.sha256(executable.read_bytes()).hexdigest() != ZIPAPP_SHA256):
                raise RuntimeError("Existing pinned umu installation differs; it was not overwritten")
            return executable  # Verified offline reuse.
        request = urllib.request.Request(URL, headers={"User-Agent": "Novelty-umu-installer/1"})
        with urllib.request.urlopen(request, timeout=30) as response:
            data = response.read(LIMIT + 1)
        if len(data) > LIMIT or hashlib.sha256(data).hexdigest() != SHA256:
            raise RuntimeError("umu archive exceeds its size limit or checksum does not match")
        with tarfile.open(fileobj=io.BytesIO(data)) as tar:
            entries = [m for m in tar.getmembers() if Path(m.name).name == "umu-run"]
            if len(entries) != 1 or not entries[0].isfile() or not 0 < entries[0].size <= LIMIT:
                raise RuntimeError("Expected exactly one regular umu-run zipapp in upstream archive")
            with tar.extractfile(entries[0]) as source:
                payload = source.read(LIMIT + 1)
        if hashlib.sha256(payload).hexdigest() != ZIPAPP_SHA256:
            raise RuntimeError("umu zipapp checksum does not match")
        # Extract only verified bytes to our own filename; never extract archive paths.
        with tempfile.TemporaryDirectory(prefix=f".umu-{VERSION}-", dir=tools) as work:
            staged = Path(work) / "ready"
            staged.mkdir()
            executable = staged / "umu-run"
            with executable.open("xb") as stream:
                stream.write(payload)
                stream.flush()
                os.fsync(stream.fileno())
            executable.chmod(0o755)
            os.rename(staged, target)
        return target / "umu-run"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("games_folder", type=Path)
    args = parser.parse_args()
    print(install(args.games_folder))


def interrupted(signum, frame):
    raise RuntimeError("umu installation cancelled; retry when ready")


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, interrupted)
    try:
        main()
    except (OSError, RuntimeError, tarfile.TarError) as error:
        print(f"umu installation failed: {error}", file=sys.stderr)
        sys.exit(1)
