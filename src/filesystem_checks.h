#pragma once
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <sys/stat.h>
#include <sys/statvfs.h>

namespace novelty {
// QFileInfo/QDir cannot distinguish every stat/readdir error from missing/empty.
inline bool checkedExists(const QString &path) {
    struct stat info{};
    if (::lstat(QFile::encodeName(path).constData(), &info) == 0) return true;
    if (errno == ENOENT) return false;
    throw std::runtime_error(("Cannot inspect " + path + ": " + QString::fromLocal8Bit(std::strerror(errno))).toStdString());
}
inline void requireAvailableSpace(QString path, qint64 bytes) {
    while (!checkedExists(path) && path != "/") path = QFileInfo(path).absolutePath();
    struct statvfs space{};
    if (::statvfs(QFile::encodeName(path).constData(), &space) != 0)
        throw std::runtime_error(("Cannot check free space at " + path).toStdString());
    if (bytes < 0 || static_cast<long double>(space.f_bavail) * space.f_frsize < bytes + 16.0L * 1024 * 1024)
        throw std::runtime_error(("Insufficient free space to stage data safely at " + path).toStdString());
}
inline QFileInfoList checkedDirectoryEntries(const QString &path, int limit = 20000) {
    QFileInfoList result;
    for (const auto &entry : std::filesystem::directory_iterator(QFile::encodeName(path).constData())) {
        if (limit > 0 && result.size() >= limit)
            throw std::runtime_error(("Too many files in " + path).toStdString());
        result.append(QFileInfo(QFile::decodeName(entry.path().c_str())));
    }
    std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) { return a.fileName() < b.fileName(); });
    return result;
}
}
