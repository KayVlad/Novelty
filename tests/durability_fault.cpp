// Interpose only fsync, at real transaction barriers, never in production code.
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <cstdio>
#include <dirent.h>
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
