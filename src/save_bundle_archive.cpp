#include "save_bundle_archive.h"
#include "saves.h"
#include <QtCore/qcryptographichash.h>
#include <QtCore/qdir.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qjsonarray.h>
#include <QtCore/qjsondocument.h>
#include <QtCore/qjsonobject.h>
#include <QtCore/qsavefile.h>
#include <QtCore/qset.h>
#include <algorithm>
#include <stdexcept>

namespace novelty {
namespace {
constexpr qint64 maxPayload = 512LL * 1024 * 1024;
constexpr quint32 maxManifest = 1024 * 1024;
constexpr qint64 chunkSize = 1024 * 1024;
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
bool exists(const QString &path) {
    const QFileInfo info(path); return info.exists() || info.isSymbolicLink();
}
void noLinks(const QString &path) {
    auto current = QString("/");
    for (const auto &part : QDir::cleanPath(QFileInfo(path).absoluteFilePath()).split('/', Qt::SkipEmptyParts)) {
        current = QDir(current).filePath(part);
        if (QFileInfo(current).isSymbolicLink()) fail("Save archive path passes through a symbolic link.");
    }
}
bool safeRelative(const QString &path, bool emptyRoot = false) {
    if (path.isEmpty()) return emptyRoot;
    if (path.size() > 4096 || path.startsWith('/') || path.contains('\\') || path.contains(QChar::Null) ||
        QDir::cleanPath(path) != path) return false;
    for (const auto &part : path.split('/')) if (part.isEmpty() || part == "." || part == "..") return false;
    return true;
}
struct FileEntry { QString path; qint64 size; QByteArray hash; };
struct ArchiveIndex { QList<FileEntry> files; QStringList directories; qint64 bytes = 0; };
ArchiveIndex index(const QJsonObject &manifest) {
    if (manifest["schema"].toInt() != 2 || !manifest["items"].isArray()) fail("Unsupported save archive manifest.");
    ArchiveIndex result;
    QSet<QString> seen;
    const auto items = manifest["items"].toArray();
    if (items.isEmpty() || items.size() > 32) fail("Save archive has invalid item count.");
    int entries = 0;
    for (int i = 0; i < items.size(); ++i) {
        const auto prefix = QString("items/") + QString::number(i);
        const auto item = items[i].toObject();
        if (!item["entries"].isArray()) fail("Save archive item has no entries.");
        for (const auto &value : item["entries"].toArray()) {
            if (++entries > 20000 || !value.isObject()) fail("Save archive has invalid entries.");
            const auto entry = value.toObject();
            const auto relative = entry["path"].toString();
            if (!safeRelative(relative, true)) fail("Save archive contains an unsafe path.");
            const auto path = prefix + (relative.isEmpty() ? QString() : "/" + relative);
            if (seen.contains(path)) fail("Save archive contains a duplicate path.");
            seen.insert(path);
            if (entry["type"] == "directory") result.directories.append(path);
            else if (entry["type"] == "file") {
                const double sizeValue = entry["size"].toDouble(-1);
                const auto hash = entry["sha256"].toString().toLatin1();
                if (sizeValue < 0 || sizeValue > maxPayload - result.bytes ||
                    static_cast<qint64>(sizeValue) != sizeValue || hash.size() != 64)
                    fail("Save archive file size or hash is invalid.");
                for (const auto byte : hash) if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
                    fail("Save archive file hash is invalid.");
                result.files.append({path, static_cast<qint64>(sizeValue), hash});
                result.bytes += static_cast<qint64>(sizeValue);
            } else fail("Save archive entry type is invalid.");
        }
    }
    if (manifest["files"].toInt(-1) != result.files.size() ||
        manifest["bytes"].toDouble(-1) != static_cast<double>(result.bytes))
        fail("Save archive totals are invalid.");
    return result;
}
void writeAll(QIODevice &device, const QByteArray &data) {
    qsizetype written = 0;
    while (written < data.size()) {
        const auto count = device.write(data.constData() + written, data.size() - written);
        if (count <= 0) fail("Cannot write save archive.");
        written += count;
    }
}
QByteArray readExact(QIODevice &device, qint64 length) {
    QByteArray data;
    data.reserve(length);
    while (data.size() < length) {
        const auto chunk = device.read(length - data.size());
        if (chunk.isEmpty()) fail("Save archive is truncated.");
        data += chunk;
    }
    return data;
}
void transfer(QIODevice &from, QIODevice &to, qint64 size, const QByteArray &expected) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (size > 0) {
        const auto data = readExact(from, std::min(size, chunkSize));
        writeAll(to, data);
        hash.addData(data);
        size -= data.size();
    }
    if (hash.result().toHex() != expected) fail("Save archive file changed or failed verification.");
}
QByteArray header(quint32 manifestLength) {
    QByteArray bytes("NVSB", 4);
    bytes.append(char(0)); bytes.append(char(1)); // format version 1
    bytes.append(char(0)); bytes.append(char(0)); // reserved
    for (int shift = 24; shift >= 0; shift -= 8) bytes.append(char((manifestLength >> shift) & 0xff));
    return bytes;
}
}

void writePortableSaveArchive(const QString &bundleDirectory, const QString &archivePath) {
    const auto manifest = verifyPortableSaveBundle(bundleDirectory);
    const auto files = index(manifest).files;
    const auto metadata = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    if (metadata.isEmpty() || metadata.size() > maxManifest) fail("Save archive manifest is too large.");
    noLinks(archivePath);
    QSaveFile output(archivePath);
    if (!output.open(QIODevice::WriteOnly)) fail("Cannot create save archive.");
    writeAll(output, header(static_cast<quint32>(metadata.size())));
    writeAll(output, metadata);
    for (const auto &entry : files) {
        const auto path = QDir(bundleDirectory).filePath(entry.path);
        noLinks(path);
        QFile input(path);
        if (!input.open(QIODevice::ReadOnly) || input.size() != entry.size)
            fail("Save bundle changed while archiving.");
        transfer(input, output, entry.size, entry.hash);
        if (!input.atEnd()) fail("Save bundle changed while archiving.");
    }
    if (!output.commit()) fail("Cannot publish save archive.");
}

void extractPortableSaveArchive(const QString &archivePath, const QString &destinationDirectory) {
    noLinks(archivePath); noLinks(destinationDirectory);
    if (exists(destinationDirectory)) fail("Save archive destination already exists.");
    QFile input(archivePath);
    if (!input.open(QIODevice::ReadOnly) || input.size() < 12 ||
        input.size() > maxPayload + maxManifest + 12) fail("Save archive is unavailable or too large.");
    const auto prefix = readExact(input, 12);
    const auto length = (quint32(quint8(prefix[8])) << 24) | (quint32(quint8(prefix[9])) << 16) |
                        (quint32(quint8(prefix[10])) << 8) | quint32(quint8(prefix[11]));
    if (prefix.left(8) != QByteArray("NVSB\0\1\0\0", 8) || length == 0 || length > maxManifest ||
        input.size() < 12 + length) fail("Save archive header is invalid.");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(readExact(input, length), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) fail("Save archive manifest is invalid.");
    const auto manifest = document.object();
    const auto contents = index(manifest);
    if (input.size() != 12 + length + contents.bytes) fail("Save archive payload length is invalid.");
    if (!QDir().mkpath(destinationDirectory + "/items")) fail("Cannot create save archive destination.");
    try {
        for (const auto &path : contents.directories)
            if (!QDir().mkpath(QDir(destinationDirectory).filePath(path)))
                fail("Cannot create archived save directory.");
        for (const auto &entry : contents.files) {
            const auto path = QDir(destinationDirectory).filePath(entry.path);
            if (exists(path) || !QDir().mkpath(QFileInfo(path).absolutePath()))
                fail("Save archive contains conflicting paths.");
            QFile output(path);
            if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
                fail("Cannot create archived save file.");
            transfer(input, output, entry.size, entry.hash);
            if (!output.flush()) fail("Cannot finish archived save file.");
        }
        QFile metadata(destinationDirectory + "/manifest.json");
        if (!metadata.open(QIODevice::WriteOnly | QIODevice::NewOnly)) fail("Cannot create archived save manifest.");
        writeAll(metadata, QJsonDocument(manifest).toJson());
        if (!metadata.flush()) fail("Cannot finish archived save manifest.");
        metadata.close();
        if (verifyPortableSaveBundle(destinationDirectory) != manifest)
            fail("Extracted save bundle changed during verification.");
    } catch (...) {
        QDir(destinationDirectory).removeRecursively();
        throw;
    }
}

} // namespace novelty
