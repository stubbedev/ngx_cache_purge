/*
 * LD_PRELOAD fault injection for the torture scenarios.  Each fault is off
 * until its control file exists, so a scenario turns it on and off while
 * nginx runs:
 *
 *   /tmp/tor/fault.eio     readdir() of a directory whose path contains the
 *                          file's first line returns NULL with EIO
 *   /tmp/tor/fault.eloop   openat() of a name equal to the first line fails
 *                          with ELOOP (a name swapped for a symlink)
 *   /tmp/tor/fault.slow    readv() and pread() of a regular file sleep that
 *                          many microseconds first (a slow disk under the
 *                          key reads)
 *
 *   gcc -shared -fPIC -O2 -o tor_shim.so tor_shim.c -ldl
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

static int
control(const char *file, char *buf, size_t size)
{
    FILE    *f;
    size_t   n;

    f = fopen(file, "r");
    if (f == NULL) {
        return 0;
    }

    n = fread(buf, 1, size - 1, f);
    fclose(f);
    buf[n] = '\0';
    buf[strcspn(buf, "\n")] = '\0';

    return 1;
}

static int
dir_matches(DIR *d)
{
    char     want[PATH_MAX], link[64], path[PATH_MAX];
    ssize_t  n;
    int      saved = errno;

    /* errno as it was on every way out: readdir() leaves it alone at the
     * end of a directory, and the module relies on that */
    if (!control("/tmp/tor/fault.eio", want, sizeof(want)) || want[0] == '\0') {
        errno = saved;
        return 0;
    }

    snprintf(link, sizeof(link), "/proc/self/fd/%d", dirfd(d));
    n = readlink(link, path, sizeof(path) - 1);
    errno = saved;

    if (n <= 0) {
        return 0;
    }

    path[n] = '\0';

    return strstr(path, want) != NULL;
}

struct dirent *
readdir(DIR *d)
{
    static struct dirent *(*real)(DIR *);

    if (real == NULL) {
        real = (struct dirent *(*)(DIR *)) dlsym(RTLD_NEXT, "readdir");
    }

    if (dir_matches(d)) {
        errno = EIO;
        return NULL;
    }

    return real(d);
}

struct dirent64 *
readdir64(DIR *d)
{
    static struct dirent64 *(*real)(DIR *);

    if (real == NULL) {
        real = (struct dirent64 *(*)(DIR *)) dlsym(RTLD_NEXT, "readdir64");
    }

    if (dir_matches(d)) {
        errno = EIO;
        return NULL;
    }

    return real(d);
}

static int
name_matches(const char *name)
{
    char        want[PATH_MAX];
    const char *base;
    int         saved = errno;

    if (!control("/tmp/tor/fault.eloop", want, sizeof(want)) || want[0] == '\0')
    {
        errno = saved;
        return 0;
    }

    errno = saved;
    base = strrchr(name, '/');
    base = (base != NULL) ? base + 1 : name;

    return strcmp(base, want) == 0;
}

int
openat(int dirfd, const char *name, int flags, ...)
{
    static int (*real)(int, const char *, int, ...);
    mode_t      mode = 0;
    va_list     ap;

    if (real == NULL) {
        real = (int (*)(int, const char *, int, ...)) dlsym(RTLD_NEXT, "openat");
    }

    if (flags & O_CREAT) {
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }

    if (name_matches(name)) {
        errno = ELOOP;
        return -1;
    }

    return real(dirfd, name, flags, mode);
}

int
openat64(int dirfd, const char *name, int flags, ...)
{
    static int (*real)(int, const char *, int, ...);
    mode_t      mode = 0;
    va_list     ap;

    if (real == NULL) {
        real = (int (*)(int, const char *, int, ...)) dlsym(RTLD_NEXT,
                                                            "openat64");
    }

    if (flags & O_CREAT) {
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }

    if (name_matches(name)) {
        errno = ELOOP;
        return -1;
    }

    return real(dirfd, name, flags, mode);
}

/* what openat() compiles to with _FORTIFY_SOURCE: nginx calls these */
int
__openat_2(int dirfd, const char *name, int flags)
{
    static int (*real)(int, const char *, int);

    if (real == NULL) {
        real = (int (*)(int, const char *, int)) dlsym(RTLD_NEXT, "__openat_2");
    }

    if (name_matches(name)) {
        errno = ELOOP;
        return -1;
    }

    return real(dirfd, name, flags);
}

int
__openat64_2(int dirfd, const char *name, int flags)
{
    static int (*real)(int, const char *, int);

    if (real == NULL) {
        real = (int (*)(int, const char *, int)) dlsym(RTLD_NEXT,
                                                       "__openat64_2");
    }

    if (name_matches(name)) {
        errno = ELOOP;
        return -1;
    }

    return real(dirfd, name, flags);
}

/* regular files only: nginx readv()s sockets too */
static void
slow(int fd)
{
    char         buf[32];
    struct stat  st;
    int          saved = errno;

    if (control("/tmp/tor/fault.slow", buf, sizeof(buf))
        && fstat(fd, &st) == 0 && S_ISREG(st.st_mode))
    {
        usleep((useconds_t) atoi(buf));
    }

    errno = saved;
}

ssize_t
readv(int fd, const struct iovec *iov, int n)
{
    static ssize_t (*real)(int, const struct iovec *, int);

    if (real == NULL) {
        real = (ssize_t (*)(int, const struct iovec *, int))
                   dlsym(RTLD_NEXT, "readv");
    }

    slow(fd);

    return real(fd, iov, n);
}

/* the build before the key reads became readv(): the same fault for it */
ssize_t
pread(int fd, void *buf, size_t size, off_t off)
{
    static ssize_t (*real)(int, void *, size_t, off_t);

    if (real == NULL) {
        real = (ssize_t (*)(int, void *, size_t, off_t))
                   dlsym(RTLD_NEXT, "pread");
    }

    slow(fd);

    return real(fd, buf, size, off);
}

ssize_t
pread64(int fd, void *buf, size_t size, off_t off)
{
    static ssize_t (*real)(int, void *, size_t, off_t);

    if (real == NULL) {
        real = (ssize_t (*)(int, void *, size_t, off_t))
                   dlsym(RTLD_NEXT, "pread64");
    }

    slow(fd);

    return real(fd, buf, size, off);
}
