// Test-only interposition at the real destination durability boundary.
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" int syncfs(int fd) noexcept {
    const char *mode = std::getenv("NOVELTY_TEST_IMPORT_FAULT");
    const char *source = std::getenv("NOVELTY_TEST_IMPORT_SOURCE");
    if (const char *marker = std::getenv("NOVELTY_TEST_IMPORT_MARKER")) {
        const int output = open(marker, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (output < 0) _exit(90);
        const char *message = source && access(source, F_OK) == 0 ? "source-present\n" : "source-missing\n";
        if (write(output, message, std::strlen(message)) < 0) _exit(91);
        close(output);
    }
    if (mode && std::strcmp(mode, "fail") == 0) { errno = EIO; return -1; }
    const auto realSync = reinterpret_cast<int (*)(int)>(dlsym(RTLD_NEXT, "syncfs"));
    if (!realSync) _exit(92);
    const int result = realSync(fd);
    if (mode && std::strcmp(mode, "crash") == 0) _exit(88);
    if (mode && std::strcmp(mode, "cleanup") == 0 && source && chmod(source, 0) != 0) _exit(93);
    return result;
}
