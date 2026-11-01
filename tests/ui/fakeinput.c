// LD_PRELOAD shim: /dev/input/eventN opens become a connection to a Unix
// socket served by session.py, and the evdev ioctls betternotes uses are
// answered as if the node were a keyboard. Test-only.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static char fake[1024];

static int fake_open(const char *path, int flags) {
    if (strncmp(path, "/dev/input/event", 16) != 0) return -2;
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = {0};
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, "/tmp/fakekbd.sock");
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) { close(s); errno = ENOENT; return -1; }
    if (flags & O_NONBLOCK) fcntl(s, F_SETFL, O_NONBLOCK);
    if (s < (int)sizeof(fake)) fake[s] = 1;
    return s;
}

#define REAL_OPEN(name)                                                       \
    int name(const char *path, int flags, ...) {                              \
        int r = fake_open(path, flags);                                       \
        if (r != -2) return r;                                                \
        static int (*real)(const char *, int, ...);                          \
        if (!real) real = dlsym(RTLD_NEXT, #name);                            \
        va_list ap; va_start(ap, flags);                                      \
        int mode = (flags & O_CREAT) ? va_arg(ap, int) : 0;                   \
        va_end(ap);                                                           \
        return real(path, flags, mode);                                       \
    }
REAL_OPEN(open)
REAL_OPEN(open64)

int close(int fd) {
    static int (*real)(int);
    if (!real) real = dlsym(RTLD_NEXT, "close");
    if (fd >= 0 && fd < (int)sizeof(fake)) fake[fd] = 0;
    return real(fd);
}

int ioctl(int fd, unsigned long req, ...) {
    va_list ap; va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (fd < 0 || fd >= (int)sizeof(fake) || !fake[fd]) {
        static int (*real)(int, unsigned long, ...);
        if (!real) real = dlsym(RTLD_NEXT, "ioctl");
        return real(fd, req, arg);
    }
    unsigned nr = _IOC_NR(req), len = _IOC_SIZE(req);
    if (_IOC_TYPE(req) != 'E') { errno = EINVAL; return -1; }
    if (nr == 0x20 + EV_KEY) {                       // EVIOCGBIT(EV_KEY)
        unsigned char *b = arg;
        memset(b, 0xff, len);                        // every key, incl. Q + Enter
        return (int)len;
    }
    if (nr == 0x06) {                                // EVIOCGNAME
        const char *n = "Fake BT Keyboard";
        strncpy(arg, n, len);
        return (int)strlen(n) + 1;
    }
    if (nr == 0x90) return 0;                        // EVIOCGRAB
    errno = EINVAL;
    return -1;
}
