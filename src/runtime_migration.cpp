#include "runtime_migration.h"
#include <QtCore/qcryptographichash.h>
#include <QtCore/qdir.h>
#include <QtCore/qdiriterator.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsonobject.h>
#include <QtCore/qregularexpression.h>
#include <QtCore/qset.h>
#include <QtCore/qstandardpaths.h>
#include <QtCore/quuid.h>
#include <algorithm>
#include <stdexcept>
#include <unistd.h>

namespace novelty {
namespace {
constexpr qint64 maxBytes = 128LL * 1024 * 1024 * 1024;
constexpr int maxEntries = 300000;
const QString receiptName = ".novelty-runtime-migration.json";
void checkStop(std::stop_token stop) {
    if (stop.stop_requested()) throw std::runtime_error("Runtime migration was cancelled. The original runtime is unchanged.");
}
QByteArray linkText(const QString &path) {
    const auto encoded = QFile::encodeName(path);
    QByteArray value(16384, Qt::Uninitialized);
    const auto size = ::readlink(encoded.constData(), value.data(), size_t(value.size()));
    if (size < 0 || size == value.size()) throw std::runtime_error("Cannot read runtime symbolic link.");
    value.resize(qsizetype(size)); return value;
}
QByteArray hashFile(const QString &path, std::stop_token stop) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read runtime file for verification.");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        checkStop(stop);
        const auto chunk = file.read(1024 * 1024);
        if (chunk.isEmpty() && file.error() != QFile::NoError)
            throw std::runtime_error("Cannot hash runtime file.");
        hash.addData(chunk);
    }
    return hash.result();
}
void copyNode(const QString &source, const QString &target, std::stop_token stop,
              int &entries, qint64 &bytes) {
    checkStop(stop);
    const QFileInfo info(source);
    if (++entries > maxEntries) throw std::runtime_error("Legacy runtime contains too many entries.");
    if (info.isSymbolicLink()) {
        const auto destination = QFile::encodeName(target);
        const auto link = linkText(source);
        if (::symlink(link.constData(), destination.constData()) != 0)
            throw std::runtime_error("Cannot copy runtime symbolic link.");
    } else if (info.isDir()) {
        if (!QDir().mkdir(target)) throw std::runtime_error("Cannot create managed runtime directory.");
        QDir dir(source);
        for (const auto &child : dir.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System |
                                                    QDir::NoDotAndDotDot, QDir::Name))
            copyNode(child.absoluteFilePath(), target + '/' + child.fileName(), stop, entries, bytes);
        if (!QFile::setPermissions(target, info.permissions()))
            throw std::runtime_error("Cannot preserve managed runtime directory permissions.");
    } else if (info.isFile()) {
        if (info.size() < 0 || info.size() > maxBytes - bytes)
            throw std::runtime_error("Legacy runtime exceeds the migration size limit.");
        bytes += info.size();
        QFile from(source), to(target);
        if (!from.open(QIODevice::ReadOnly) || !to.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            throw std::runtime_error("Cannot copy legacy runtime file.");
        while (!from.atEnd()) {
            checkStop(stop);
            const auto chunk = from.read(1024 * 1024);
            if ((chunk.isEmpty() && from.error() != QFile::NoError) || to.write(chunk) != chunk.size())
                throw std::runtime_error("Cannot copy legacy runtime file.");
        }
        if (!to.flush()) throw std::runtime_error("Cannot flush managed runtime copy.");
        to.close(); from.close();
        if (!QFile::setPermissions(target, info.permissions()) || hashFile(source, stop) != hashFile(target, stop))
            throw std::runtime_error("Runtime file changed or failed verification during migration.");
    } else throw std::runtime_error("Legacy runtime contains an unsupported file type.");
}
QByteArray treeDigest(const QString &root, std::stop_token stop) {
    QList<QString> paths;
    QDirIterator it(root, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        checkStop(stop);
        const auto path = it.next();
        const auto relative = QDir(root).relativeFilePath(path);
        if (relative == receiptName) continue;
        paths.append(relative);
        if (paths.size() > maxEntries) throw std::runtime_error("Managed runtime contains too many entries.");
    }
    std::sort(paths.begin(), paths.end());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 bytes = 0;
    for (const auto &relative : paths) {
        checkStop(stop);
        const auto path = root + '/' + relative;
        const QFileInfo info(path);
        hash.addData(relative.toUtf8()); hash.addData(QByteArray(1, '\0'));
        if (info.isSymbolicLink()) { hash.addData(QByteArray("L")); hash.addData(linkText(path)); }
        else if (info.isDir()) {
            hash.addData(QByteArray("D"));
            hash.addData(QByteArray::number(int(info.permissions())));
        }
        else if (info.isFile()) {
            if (info.size() < 0 || info.size() > maxBytes - bytes)
                throw std::runtime_error("Managed runtime exceeds the migration size limit.");
            bytes += info.size();
            hash.addData(QByteArray("F")); hash.addData(QByteArray::number(info.size()));
            hash.addData(QByteArray::number(int(info.permissions())));
            hash.addData(hashFile(path, stop));
        } else throw std::runtime_error("Managed runtime contains an unsupported file type.");
        hash.addData(QByteArray(1, '\0'));
    }
    return hash.result().toHex();
}
void verifyReceipt(const QString &destination, std::stop_token stop) {
    const auto receipt = readJson(destination + '/' + receiptName);
    if (receipt["schema"] != 1 || receipt["sha256"].toString().size() != 64 ||
        receipt["sha256"].toString().toLatin1() != treeDigest(destination, stop))
        throw std::runtime_error("Managed runtime copy needs review; the library still uses its original runtime.");
}
}

QString legacyUmuRuntimePath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/umu";
}

void migrateLegacyUmuRuntime(Library &lib, const QString &source, std::stop_token stop) {
    if (lib.managedRuntimeStorage()) return;
    const auto destination = lib.dataRoot() + "/runtime";
    if (QFileInfo(destination).exists() || QFileInfo(destination).isSymbolicLink()) {
        if (!QFileInfo(destination).isDir() || QFileInfo(destination).isSymbolicLink())
            throw std::runtime_error("Managed runtime path needs review before migration.");
        verifyReceipt(destination, stop);
        checkStop(stop); lib.enableManagedRuntimeStorage(); return;
    }
    const auto absoluteSource = QDir::cleanPath(QFileInfo(source).absoluteFilePath());
    if (!QFileInfo(absoluteSource).isDir() || canonicalPath(absoluteSource) != absoluteSource ||
        absoluteSource == lib.dataRoot() || absoluteSource.startsWith(lib.dataRoot() + '/') ||
        lib.dataRoot().startsWith(absoluteSource + '/'))
        throw std::runtime_error("Choose an existing legacy umu runtime folder outside Games storage.");
    const auto stage = lib.dataRoot() + "/.runtime-migration-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!QDir().mkdir(stage)) throw std::runtime_error("Cannot stage runtime migration inside Games.");
    try {
        if (!QDir().mkdir(stage + "/umu"))
            throw std::runtime_error("Cannot stage umu runtime directory inside Games.");
        int entries = 0, runtimes = 0; qint64 bytes = 0;
        QSet<QString> knownRuntimes;
        static const QRegularExpression validRuntime("^[a-z0-9][a-z0-9-]{0,39}$");
        const auto appIds = readJson(lib.dataRoot() + "/runtime-appids.json");
        for (auto it = appIds.begin(); it != appIds.end(); ++it)
            if (validRuntime.match(it.value().toString()).hasMatch()) knownRuntimes.insert(it.value().toString());
        for (const auto &name : knownRuntimes) {
            checkStop(stop);
            const auto path = absoluteSource + '/' + name;
            if (!QFileInfo(path).exists()) continue;
            if (QFileInfo(path).isSymbolicLink() || !QFileInfo(path).isDir() ||
                !QFileInfo(path + "/.installed.ok").isFile() || QFileInfo(path + "/.installed.ok").isSymbolicLink() ||
                !QFileInfo(path + "/toolmanifest.vdf").isFile() || QFileInfo(path + "/toolmanifest.vdf").isSymbolicLink())
                throw std::runtime_error("A legacy Steam Linux Runtime is incomplete.");
            copyNode(path, stage + "/umu/" + name, stop, entries, bytes); ++runtimes;
        }
        if (!runtimes) throw std::runtime_error("No complete Steam Linux Runtime was found in the chosen folder.");
        const auto shim = absoluteSource + "/umu-shim";
        if (QFileInfo(shim).exists()) copyNode(shim, stage + "/umu/umu-shim", stop, entries, bytes);
        const auto digest = QString::fromLatin1(treeDigest(stage, stop));
        writeJson(stage + '/' + receiptName, {{"schema", 1}, {"source", absoluteSource},
                                                 {"sha256", digest}, {"runtime_count", runtimes}});
        verifyReceipt(stage, stop);
        checkStop(stop);
        if (!QDir().rename(stage, destination))
            throw std::runtime_error("Cannot publish the verified runtime copy in Games.");
        lib.enableManagedRuntimeStorage();
    } catch (...) {
        if (QFileInfo(stage).isDir()) QDir(stage).removeRecursively();
        throw;
    }
}

} // namespace novelty
