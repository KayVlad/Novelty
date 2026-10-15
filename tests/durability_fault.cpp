// Interpose only fsync, at real transaction barriers, never in production code.
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
extern "C" int fsync(int fd) noexcept {
    const auto real = reinterpret_cast<int (*)(int)>(dlsym(RTLD_NEXT, "fsync"));
    char link[64], path[4096]; std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    const auto n = readlink(link, path, sizeof(path) - 1);
    const char *match = std::getenv("NOVELTY_TEST_FLUSH_PATH");
    static int hits = 0;
    if (n >= 0) path[n] = 0;
    const bool matched = n >= 0 && match && std::strstr(path, match);
    const int at = std::getenv("NOVELTY_TEST_FLUSH_AT") ? std::atoi(std::getenv("NOVELTY_TEST_FLUSH_AT")) : 1;
    if (matched && ++hits >= at) {
        const char *mode = std::getenv("NOVELTY_TEST_FLUSH_MODE");
        if (mode && std::strcmp(mode, "slow") == 0) {
            if (const auto marker = std::getenv("NOVELTY_TEST_FLUSH_MARKER")) {
                const int out = open(marker, O_WRONLY | O_CREAT, 0600); if (out >= 0) close(out);
            }
            usleep(200000); return real(fd);
        }
        if (mode && std::strcmp(mode, "crash") == 0) { if (real(fd)) _exit(89); _exit(88); }
        if (hits == at || (mode && std::strcmp(mode, "fail-always") == 0)) { errno = EIO; return -1; }
    }
    return real(fd);
}

extern "C" dirent *readdir(DIR *directory) {
    const auto real = reinterpret_cast<dirent *(*)(DIR *)>(dlsym(RTLD_NEXT, "readdir"));
    const char *match = std::getenv("NOVELTY_TEST_READDIR_PATH");
    char link[64], path[4096]; std::snprintf(link, sizeof(link), "/proc/self/fd/%d", dirfd(directory));
    const auto n = readlink(link, path, sizeof(path) - 1);
    if (n >= 0) path[n] = 0;
    if (n >= 0 && match && std::strstr(path, match)) { errno = EIO; return nullptr; }
    return real(directory);
}
namespace {
bool failRestoreRename(const char *source) {
    return std::getenv("NOVELTY_TEST_RESTORE_RENAME_FAILURE") &&
        (std::strstr(source, ".novelty-incoming-") || std::strstr(source, ".novelty-held-"));
}
}
extern "C" int rename(const char *source, const char *destination) noexcept {
    if (failRestoreRename(source)) { errno = EIO; return -1; }
    const auto real = reinterpret_cast<int (*)(const char *, const char *)>(dlsym(RTLD_NEXT, "rename"));
    return real(source, destination);
}
extern "C" int renameat2(int from, const char *source, int to, const char *destination, unsigned flags) noexcept {
    if (failRestoreRename(source)) { errno = EIO; return -1; }
    const auto real = reinterpret_cast<int (*)(int, const char *, int, const char *, unsigned)>(dlsym(RTLD_NEXT, "renameat2"));
    if (!real) { errno = ENOSYS; return -1; }
    return real(from, source, to, destination, flags);
}
