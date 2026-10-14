#!/usr/bin/env python3
"""Install a verified, pinned umu zipapp side by side under a Games library."""
import argparse
import fcntl
import hashlib
import io
import json
import os
from pathlib import Path
import re
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
API = "https://api.github.com/repos/Open-Wine-Components/umu-launcher/releases/"


def release_spec(selector):
    if selector in (VERSION, "umu-" + VERSION):
        return VERSION, URL, SHA256, ZIPAPP_SHA256
    if selector == "umu-latest":
        endpoint = API + "latest"
    else:
        version = selector.removeprefix("umu-")
        if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
            raise ValueError("Choose umu-latest or a specific umu version")
        endpoint = API + "tags/" + version
    request = urllib.request.Request(endpoint, headers={"User-Agent": "Novelty-umu-installer/1"})
    with urllib.request.urlopen(request, timeout=30) as response:
        metadata = response.read(2 * 1024 * 1024 + 1)
    if len(metadata) > 2 * 1024 * 1024:
        raise RuntimeError("umu release metadata exceeds its size limit")
    release = json.loads(metadata)
    version = release.get("tag_name")
    if not isinstance(version, str) or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version) or release.get("draft") or release.get("prerelease"):
        raise RuntimeError("The selected umu release is not a stable version")
    if selector != "umu-latest" and version != selector.removeprefix("umu-"):
        raise RuntimeError("umu release tag differs from the requested version")
    name = f"umu-launcher-{version}-zipapp.tar"
    assets = [asset for asset in release.get("assets", []) if asset.get("name") == name]
    if len(assets) != 1:
        raise RuntimeError("The selected umu release has no unique zipapp archive")
    asset = assets[0]
    url = f"https://github.com/Open-Wine-Components/umu-launcher/releases/download/{version}/{name}"
    digest = asset.get("digest", "")
    if (asset.get("browser_download_url") != url or type(asset.get("size")) is not int or
            not 0 < asset["size"] <= LIMIT or not re.fullmatch(r"sha256:[0-9a-fA-F]{64}", digest)):
        raise RuntimeError("umu release asset is missing verified size or SHA-256")
    return version, url, digest.split(":", 1)[1].lower(), None


def install(games_folder, selector=VERSION):
    if not games_folder.is_dir():
        raise RuntimeError("Games folder must already exist")
    root = games_folder.resolve()
    tools = root / ".novelty" / "tools"
    if tools.resolve() != tools:
        raise RuntimeError("Managed tools must not be linked to another location")
    tools.mkdir(parents=True, exist_ok=True)
    version, url, archive_sha, pinned_zipapp_sha = release_spec(selector)
    target = tools / f"umu-{version}"
    # Never replace an existing tool: another game may still be importing its zipapp.
    with os.fdopen(os.open(tools / f".umu-{version}.lock", os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600), "a+b") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError("This umu version is already being installed; retry shortly") from error
        if target.exists() or target.is_symlink():
            executable = target / "umu-run"
            receipt = target / "novelty-install.json"
            recorded = json.loads(receipt.read_text()) if receipt.is_file() and not receipt.is_symlink() else {}
            expected = pinned_zipapp_sha or (recorded.get("zipapp_sha256") if recorded.get("version") == version and recorded.get("archive_sha256") == archive_sha else None)
            if (target.is_symlink() or not target.is_dir() or executable.is_symlink()
                    or not executable.is_file() or executable.stat().st_size > LIMIT
                    or not os.access(executable, os.X_OK)
                    or not isinstance(expected, str) or not re.fullmatch(r"[0-9a-f]{64}", expected)
                    or hashlib.sha256(executable.read_bytes()).hexdigest() != expected):
                raise RuntimeError("Existing pinned umu installation differs; it was not overwritten")
            return executable  # Verified offline reuse.
        request = urllib.request.Request(url, headers={"User-Agent": "Novelty-umu-installer/1"})
        with urllib.request.urlopen(request, timeout=30) as response:
            data = response.read(LIMIT + 1)
        if len(data) > LIMIT or hashlib.sha256(data).hexdigest() != archive_sha:
            raise RuntimeError("umu archive exceeds its size limit or checksum does not match")
        with tarfile.open(fileobj=io.BytesIO(data)) as tar:
            entries = [m for m in tar.getmembers() if Path(m.name).name == "umu-run"]
            if len(entries) != 1 or not entries[0].isfile() or not 0 < entries[0].size <= LIMIT:
                raise RuntimeError("Expected exactly one regular umu-run zipapp in upstream archive")
            with tar.extractfile(entries[0]) as source:
                payload = source.read(LIMIT + 1)
        zipapp_sha = hashlib.sha256(payload).hexdigest()
        if pinned_zipapp_sha and zipapp_sha != pinned_zipapp_sha:
            raise RuntimeError("umu zipapp checksum does not match")
        # Extract only verified bytes to our own filename; never extract archive paths.
        with tempfile.TemporaryDirectory(prefix=f".umu-{version}-", dir=tools) as work:
            staged = Path(work) / "ready"
            staged.mkdir()
            executable = staged / "umu-run"
            with executable.open("xb") as stream:
                stream.write(payload)
                stream.flush()
                os.fsync(stream.fileno())
            executable.chmod(0o755)
            (staged / "novelty-install.json").write_text(json.dumps({"schema": 1, "version": version,
                "archive_sha256": archive_sha, "zipapp_sha256": zipapp_sha}) + "\n")
            os.rename(staged, target)
        return target / "umu-run"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("games_folder", type=Path)
    parser.add_argument("--version", default=VERSION)
    args = parser.parse_args()
    print(install(args.games_folder, args.version))


def interrupted(signum, frame):
    raise RuntimeError("umu installation cancelled; retry when ready")


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, interrupted)
    try:
        main()
    except (OSError, RuntimeError, ValueError, json.JSONDecodeError, tarfile.TarError) as error:
        print(f"umu installation failed: {error}", file=sys.stderr)
        sys.exit(1)
