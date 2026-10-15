#include "filesystem_durability.h"
#include "core.h"
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace novelty {
namespace {
[[noreturn]] void failure(const QString &path) {
    throw std::runtime_error(("Cannot make filesystem changes durable at " + path + ": " +
                              QString::fromLocal8Bit(std::strerror(errno))).toStdString());
}
void flush(const QString &path, bool directory) {
    const int fd = open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW |
                        (directory ? O_DIRECTORY : 0));
    if (fd < 0) failure(path);
    int result;
    do { result = fsync(fd); } while (result < 0 && errno == EINTR);
    const int error = errno;
    close(fd);
    if (result < 0) { errno = error; failure(path); }
}
void tree(const std::filesystem::path &path) {
    const auto name = QFile::decodeName(path.c_str());
    const auto status = std::filesystem::symlink_status(path); // never follow links
    if (std::filesystem::is_regular_file(status)) flush(name, false);
    else if (std::filesystem::is_directory(status)) {
        for (const auto &child : std::filesystem::directory_iterator(path)) tree(child.path());
        flush(name, true); // children before the directory that names them
    } else throw std::runtime_error(("Cannot persist unsupported save file: " + name).toStdString());
}
}
void persistTree(const QString &path) { tree(std::filesystem::path(QFile::encodeName(path).constData())); }
void persistParent(const QString &path) {
    // Include ancestors: mkpath may have created several previously unpersisted directories.
    auto parent = QFileInfo(path).absolutePath();
    while (true) {
        flush(parent, true);
        if (parent == "/") break;
        parent = QFileInfo(parent).absolutePath();
    }
}
void writeDurableJson(const QString &path, const QJsonObject &value) {
    writeJson(path, value);
    flush(path, false);
    persistParent(path);
}
void removeDurableFile(const QString &path) {
    if (!QFile::remove(path)) throw std::runtime_error(("Cannot retire recovery record: " + path).toStdString());
    persistParent(path);
}
}
