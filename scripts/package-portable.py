#!/usr/bin/env python3
"""Build an unpacked, relocatable Novelty test package from a native build.

The package retains the host's glibc and graphics stack. Clean-device acceptance
and bundled-library license review remain separate release gates.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import sysconfig
import tempfile

PROJECT = Path(__file__).resolve().parents[1]
SYSTEM_LIBRARY = re.compile(
    r"^(?:ld-linux|libc\.so|libm\.so|libpthread\.so|libdl\.so|librt\.so|"
    r"libresolv\.so|libnss_|libGL\.so|libGLX\.so|libOpenGL\.so|"
    r"libEGL\.so|libGLdispatch\.so)"
)
PLUGINS = {
    "platforms": ("libqxcb.so", "libqwayland-generic.so", "libqwayland-egl.so", "libqoffscreen.so"),
    "sqldrivers": ("libqsqlite.so",),
    "imageformats": ("libqjpeg.so", "libqsvg.so", "libqwebp.so"),
    "iconengines": ("libqsvgicon.so",),
    "tls": ("libqopensslbackend.so",),
}


def dependencies(binary: Path) -> dict[str, Path]:
    result = subprocess.run(("ldd", str(binary)), check=True, text=True, capture_output=True)
    found = {}
    for line in result.stdout.splitlines():
        if "not found" in line:
            raise RuntimeError(f"Missing dependency for {binary.name}: {line.strip()}")
        match = re.match(r"\s*(\S+)\s+=>\s+(/\S+)\s+\(", line)
        if match and "/" not in match[1] and not SYSTEM_LIBRARY.match(match[1]):
            found[match[1]] = Path(match[2])
    return found


def plugin_root() -> Path:
    qmake = shutil.which("qmake6")
    if not qmake:
        raise RuntimeError("qmake6 is required to locate the matching Qt plugins")
    value = subprocess.check_output((qmake, "-query", "QT_INSTALL_PLUGINS"), text=True).strip()
    root = Path(value)
    if not root.is_dir():
        raise RuntimeError(f"Qt plugin directory is unavailable: {root}")
    return root


def package(binary: Path, output: Path) -> None:
    if output.exists():
        raise RuntimeError(f"Output already exists: {output}")
    if not binary.is_file():
        raise RuntimeError(f"Build Novelty first: {binary}")
    server_binary = binary.with_name("novelty-save-server")
    if not server_binary.is_file():
        raise RuntimeError(f"Build the save server first: {server_binary}")
    output.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=".novelty-package-", dir=output.parent))
    try:
        destination = stage / "bin" / "novelty"
        destination.parent.mkdir()
        shutil.copy2(binary, destination)
        server_destination = stage / "bin" / "novelty-save-server"
        shutil.copy2(server_binary, server_destination)
        helpers = stage / "share" / "novelty"
        helpers.mkdir(parents=True)
        for name in ("install-ge.py", "install-umu.py"):
            shutil.copy2(PROJECT / "scripts" / name, helpers / name)

        if sys.version_info < (3, 12):
            raise RuntimeError("Bundled installer Python must be version 3.12 or newer")
        python_home = stage / "python"
        python_binary = python_home / "bin" / "python3-real"
        python_binary.parent.mkdir(parents=True)
        shutil.copy2(sys.executable, python_binary)
        stdlib = Path(sysconfig.get_path("stdlib"))
        bundled_stdlib = python_home / "lib" / f"python{sys.version_info.major}.{sys.version_info.minor}"
        shutil.copytree(stdlib, bundled_stdlib, ignore=shutil.ignore_patterns(
            "__pycache__", "site-packages", "test", "tests", "tkinter", "idlelib", "turtledemo", "ensurepip", "venv"))
        python_launcher = python_home / "bin" / "python3"
        python_launcher.write_text('''#!/bin/sh
set -eu
novelty_app_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
export LD_LIBRARY_PATH="$novelty_app_dir/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PYTHONHOME="$novelty_app_dir/python"
export PYTHONNOUSERSITE=1 PYTHONDONTWRITEBYTECODE=1
unset PYTHONPATH PYTHONUSERBASE
exec "$novelty_app_dir/python/bin/python3-real" "$@"
''')
        python_launcher.chmod(0o755)

        root = plugin_root()
        copied_plugins = []
        for category, names in PLUGINS.items():
            for name in names:
                source = root / category / name
                if not source.is_file():
                    if category in ("platforms", "sqldrivers") and name in ("libqxcb.so", "libqsqlite.so"):
                        raise RuntimeError(f"Required Qt plugin is missing: {source}")
                    continue
                target = stage / "plugins" / category / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
                copied_plugins.append(target)

        libraries = {}
        python_extensions = list(bundled_stdlib.rglob("*.so"))
        for target in (destination, server_destination, python_binary, *copied_plugins, *python_extensions):
            for name, path in dependencies(target).items():
                previous = libraries.get(name)
                if previous and previous.resolve() != path.resolve():
                    raise RuntimeError(f"Conflicting host libraries named {name}")
                libraries[name] = path
        libdir = stage / "lib"
        libdir.mkdir()
        for name, source in sorted(libraries.items()):
            shutil.copy2(source, libdir / name)

        launcher = stage / "Novelty"
        launcher.write_text('''#!/bin/sh
set -eu
novelty_app_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export NOVELTY_ORIGINAL_LD_LIBRARY_PATH="${LD_LIBRARY_PATH-}"
export NOVELTY_ORIGINAL_QT_PLUGIN_PATH="${QT_PLUGIN_PATH-}"
export NOVELTY_BUNDLED_LIB_DIR="$novelty_app_dir/lib"
export LD_LIBRARY_PATH="$novelty_app_dir/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$novelty_app_dir/plugins"
exec "$novelty_app_dir/bin/novelty" "$@"
''')
        launcher.chmod(0o755)
        server_launcher = stage / "NoveltySaveServer"
        server_launcher.write_text('''#!/bin/sh
set -eu
novelty_app_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export LD_LIBRARY_PATH="$novelty_app_dir/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$novelty_app_dir/bin/novelty-save-server" "$@"
''')
        server_launcher.chmod(0o755)
        (stage / "bin" / "qt.conf").write_text("[Paths]\nPlugins=../plugins\n")
        (stage / "README.txt").write_text(
            "Run ./Novelty from this folder. Keep it writable for bin/data/settings.json.\n"
            "This is a native test package, not clean-device acceptance. It uses the host\n"
            "glibc, graphics drivers and certificate store. Python for installers is bundled.\n"
            "GE-Proton, umu and Steam Linux Runtime are managed in the Games folder.\n"
            "Optional self-hosted saves: run ./NoveltySaveServer --data PRIVATE_FOLDER.\n"
            "The owner-only access token is PRIVATE_FOLDER/access.token; use a TLS reverse proxy\n"
            "for access from another device. The service listens on loopback only.\n"
        )
        manifest = {}
        for file in sorted(p for p in stage.rglob("*") if p.is_file()):
            manifest[str(file.relative_to(stage))] = hashlib.sha256(file.read_bytes()).hexdigest()
        (stage / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        os.rename(stage, output)
    except BaseException:
        shutil.rmtree(stage, ignore_errors=True)
        raise


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=PROJECT / "build" / "novelty")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    package(args.binary.resolve(), args.output.absolute())


if __name__ == "__main__":
    main()
