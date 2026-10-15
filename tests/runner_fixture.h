#pragma once
#include "core.h"
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qdir.h>
#include <stdexcept>

// A complete published fixture, matching the installer's receipt contract.
inline void managedRunnerFixture(const QString &path) {
    QDir().mkpath(path);
    QFile proton(path + "/proton");
    if (!proton.exists()) { if (!proton.open(QIODevice::WriteOnly)) throw std::runtime_error("runner fixture"); proton.write("#!/bin/sh\n"); proton.close(); }
    QFile::setPermissions(proton.fileName(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QFile version(path + "/version");
    if (!version.exists()) { if (!version.open(QIODevice::WriteOnly)) throw std::runtime_error("version fixture"); version.write(QFileInfo(path).fileName().toUtf8()); version.close(); }
    novelty::writeJson(path + "/novelty-install.json", {{"schema", 1}, {"version", QFileInfo(path).fileName()},
        {"architecture", "x86_64"}, {"sha512", QString(128, 'a')}, {"size", 1}});
}
