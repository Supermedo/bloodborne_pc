/* Small host-only helpers for regression tests, using the user's temporary directory. */
#ifndef BB_TEST_PLATFORM_H
#define BB_TEST_PLATFORM_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
static inline int bb_test_setenv(const char *name, const char *value, int overwrite) {
    return !overwrite && getenv(name) ? 0 : _putenv_s(name, value);
}
#define setenv bb_test_setenv
#define unsetenv(name) _putenv_s(name, "")
static inline int bb_test_temp(char *path, size_t size, const char *prefix) {
    _set_error_mode(_OUT_TO_STDERR);
    const char *directory = getenv("TEMP");
    if (!directory) directory = ".";
    snprintf(path, size, "%s/%s-XXXXXX", directory, prefix);
    if (_mktemp_s(path, size)) return -1;
    return _open(path, _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
}
#else
static inline int bb_test_temp(char *path, size_t size, const char *prefix) {
    const char *directory = getenv("TMPDIR");
    snprintf(path, size, "%s/%s-XXXXXX", directory ? directory : "/tmp", prefix);
    return mkstemp(path);
}
#endif
#endif
