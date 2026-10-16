#!/usr/bin/env python3
"""Recreate local development headers for this Deck's Qt 6.9.1 runtime.

This is a development fallback, not a distribution package. Prefer a full Qt SDK.
Sources retain their upstream copyright and license headers in ignored .tools/.
"""
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import re
import subprocess
import urllib.request

VERSION = "6.9.1"
PROJECT = Path(__file__).resolve().parents[1]
INCLUDE = PROJECT / ".tools" / "qt-include"
BASE = f"https://raw.githubusercontent.com/qt/qtbase/v{VERSION}/"

def get(url):
    request = urllib.request.Request(url, headers={"User-Agent": "Novelty-development-bootstrap"})
    with urllib.request.urlopen(request, timeout=30) as response:
        return response.read().decode()

def module(path):
    return {"corelib": "QtCore", "gui": "QtGui", "widgets": "QtWidgets", "sql": "QtSql", "network": "QtNetwork", "dbus": "QtDBus"}[path.split("/")[1]]

def main():
    host_version = subprocess.check_output(["qmake6", "-query", "QT_VERSION"], text=True).strip()
    if host_version != VERSION:
        raise RuntimeError(f"Fallback headers require Qt {VERSION}; host has {host_version}. Use a full SDK.")
    tree = json.loads(get(f"https://api.github.com/repos/qt/qtbase/git/trees/v{VERSION}?recursive=1"))
    if tree.get("truncated"):
        raise RuntimeError("Incomplete upstream tree")
    mapping = {}
    for entry in tree["tree"]:
        path = entry["path"]
        if entry["type"] == "blob" and re.match(r"src/(corelib|gui|widgets|sql|network|dbus)/", path) and path.endswith(".h"):
            if "/private/" in path or "/3rdparty/" in path:
                continue
            name = Path(path).name
            if name not in mapping or len(path) < len(mapping[name]):
                mapping[name] = path
    needed = set()
    for p in [*PROJECT.glob("src/*.*"), *PROJECT.glob("tests/*.cpp")]:
        needed.update(re.findall(r'#\s*include\s*<Qt\w+/(q[\w-]+\.h)>', p.read_text()))
    needed.update({"qshareddata.h", "qsslconfiguration.h", "qsslpresharedkeyauthenticator.h"})
    completed = set()
    def fetch(name):
        path = mapping[name]
        dest = INCLUDE / module(path) / name
        text = dest.read_text() if dest.exists() else get(BASE + path)
        dest.parent.mkdir(parents=True, exist_ok=True)
        if not dest.exists():
            dest.write_text(text)
        return text
    with ThreadPoolExecutor(max_workers=8) as pool:
        while needed - completed:
            batch = sorted(n for n in needed - completed if n in mapping)
            if not batch:
                break
            for name, source in zip(batch, pool.map(fetch, batch)):
                completed.add(name)
                needed.update(re.findall(r'#\s*include\s*[<"](?:Qt(?:Core|Gui|Widgets|Sql|Network|DBus)/)?(q[\w-]+\.h)[>"]', source))
    for mod in ["Core", "Gui", "Widgets", "Sql", "Network", "DBus"]:
        folder = INCLUDE / ("Qt" + mod)
        folder.mkdir(parents=True, exist_ok=True)
        pri = Path(f"/usr/lib/qt6/mkspecs/modules/qt_lib_{mod.lower()}.pri").read_text()
        features = {}
        for category, value in [("enabled", 1), ("disabled", -1)]:
            for found in re.finditer(rf'QT\.{mod.lower()}\.{category}_features[ \t]*=[ \t]*([^\n]*)', pri):
                for feature in found[1].split():
                    feature = feature.replace("-", "_")
                    if re.fullmatch(r"\w+", feature):
                        features[feature] = value
        (folder / f"qt{mod.lower()}-config.h").write_text("#pragma once\n" + "\n".join(
            f"#ifndef QT_FEATURE_{name}\n#define QT_FEATURE_{name} {value}\n#endif" for name, value in features.items()) + "\n")
    core = INCLUDE / "QtCore"
    (core / "qconfig.h").write_text('#pragma once\n#define QT_VERSION_MAJOR 6\n#define QT_VERSION_MINOR 9\n#define QT_VERSION_PATCH 1\n#include <QtCore/qtcore-config.h>\n')
    (core / "qtversion.h").write_text('#pragma once\n#include <QtCore/qtversionchecks.h>\n#define QT_VERSION_STR "6.9.1"\n')
    template = get(BASE + "cmake/modulecppexports.h.in")
    for mod in ["Core", "Gui", "Widgets", "Sql", "Network", "DBus"]:
        name = "qt" + mod.lower() + "exports"
        text = template.replace("@header_base_name_upper@", name.upper()).replace("@module_define_infix@", mod.upper())
        (INCLUDE / ("Qt" + mod) / (name + ".h")).write_text(text)
    text = get(BASE + "src/corelib/global/qtdeprecationdefinitions.h.in")
    (core / "qtdeprecationdefinitions.h").write_text(text.replace("@QT_DISABLE_DEPRECATED_UP_TO@", "0x050000").replace("@QT_WARN_DEPRECATED_UP_TO@", "0x060901"))
    for mod, name, source in [("QtCore", "QList", "qlist.h"), ("QtCore", "QObject", "qobject.h"),
                              ("QtCore", "QRect", "qrect.h"), ("QtCore", "QScopedPointer", "qscopedpointer.h"),
                              ("QtCore", "QSize", "qsize.h"), ("QtCore", "QSizeF", "qsize.h"),
                              ("QtCore", "QString", "qstring.h"), ("QtCore", "QUrl", "qurl.h"),
                              ("QtCore", "QVariant", "qvariant.h"), ("QtCore", "QIODevice", "qiodevice.h"),
                              ("QtCore", "QSharedDataPointer", "qshareddata.h"),
                              ("QtCore", "QFlags", "qflags.h"), ("QtCore", "QMetaType", "qmetatype.h"),
                              ("QtNetwork", "QSslConfiguration", "qsslconfiguration.h"),
                              ("QtNetwork", "QSslPreSharedKeyAuthenticator", "qsslpresharedkeyauthenticator.h"),
                              ("QtNetwork", "QNetworkRequest", "qnetworkrequest.h"),
                              ("QtNetwork", "QNetworkAccessManager", "qnetworkaccessmanager.h"),
                              ("QtGui", "QTransform", "qtransform.h"), ("QtWidgets", "QLayout", "qlayout.h")]:
        (INCLUDE / mod / name).write_text(f"#include <{mod}/{source}>\n")
    print(f"Prepared {len(completed)} upstream Qt headers under {INCLUDE}")

if __name__ == "__main__":
    main()
