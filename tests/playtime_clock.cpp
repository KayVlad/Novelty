// Test-only wall-clock changes. CLOCK_MONOTONIC, sleeps and timeouts stay real.
#include <cstdlib>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
namespace {
long offset() {
    const char *path = std::getenv("NOVELTY_TEST_WALLCLOCK_OFFSET");
    if (!path) return 0;
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char bytes[64]{}; const auto count = read(fd, bytes, sizeof(bytes) - 1); close(fd);
    return count > 0 ? std::strtol(bytes, nullptr, 10) : 0;
}
}
extern "C" int clock_gettime(clockid_t clock, timespec *value) noexcept {
    const auto result = syscall(SYS_clock_gettime, clock, value);
    if (result == 0 && clock == CLOCK_REALTIME) value->tv_sec += offset();
    return result;
}
extern "C" int gettimeofday(timeval *value, void *zone) noexcept {
    const auto result = syscall(SYS_gettimeofday, value, zone);
    if (result == 0) value->tv_sec += offset();
    return result;
}
