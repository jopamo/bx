#define _GNU_SOURCE

#include <stdio.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"

static void usage(FILE* out, const char* name) {
    fprintf(out, "Usage: %s [-n | -N] [--] KEY...\n"
                 "       %s -w [-n | -q] [--] KEY=VALUE...\n"
                 "Read or write Linux kernel settings through /proc/sys.\n", name, name);
}

#ifdef __linux__
#include <limits.h>
#include <linux/magic.h>
#include <linux/openat2.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <sys/vfs.h>

#include "lib/fd_ops.h"
#include "lib/open_resolve.h"
#include "lib/option_scan.h"
#include "lib/xreadwrite.h"

struct sysctl_request {
    char* path;
    char* value;
};

/* Slash form can address components with literal dots, such as VLAN names. */
static bool sysctl_key_path(char* path) {
    if (*path == '-')
        return false;
    const char* separator = strpbrk(path, "./");
    bool dotted = separator && *separator == '.';
    char* part = path;
    for (char* p = path;; p++) {
        unsigned char c = (unsigned char)*p;
        if (dotted && c == '/')
            return false;
        if (dotted && c == '.')
            *p = '/';
        if (*p == '/' || *p == '\0') {
            size_t n = (size_t)(p - part);
            if (n == 0 || n > NAME_MAX ||
                (n == 1 && part[0] == '.') ||
                (n == 2 && part[0] == '.' && part[1] == '.'))
                return false;
            if (*p == '\0')
                return true;
            part = p + 1;
        } else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                     (c >= '0' && c <= '9') || strchr("_.-:@", c))) {
            return false;
        }
    }
}

static bool sysctl_print_name(const char* path) {
    for (const char* p = path; *p; p++)
        if (putchar(*p == '/' ? '.' : (unsigned char)*p) == EOF)
            return false;
    return true;
}

static bool sysctl_emit(int fd, const struct sysctl_request* req, bool values,
                        bool names, struct bx_diag_ctx* diag) {
    if (!values && !sysctl_print_name(req->path))
        goto output_error;
    if (!values && !names && fputs(" = ", stdout) == EOF)
        goto output_error;
    int last = '\0';
    if (!names) {
        if (req->value != NULL) {
            if (fputs(req->value, stdout) == EOF)
                goto output_error;
        } else {
            char buf[65536]; /* Accommodate a page-sized value in one read. */
            for (;;) {
                ssize_t n = bx_xread(fd, buf, sizeof(buf));
                if (n < 0) {
                    bx_perror_path(diag, req->path);
                    return false;
                }
                if (n == 0)
                    break;
                if (fwrite(buf, 1, (size_t)n, stdout) != (size_t)n)
                    goto output_error;
                last = (unsigned char)buf[n - 1];
            }
        }
    }
    if ((last == '\n' || putchar('\n') != EOF) && fflush(stdout) != EOF)
        return true;
output_error:
    bx_perror_path(diag, "standard output");
    return false;
}

static bool sysctl_apply(int root, const struct sysctl_request* req, bool quiet, bool values,
                         bool names, struct bx_diag_ctx* diag) {
    bool writing = req->value != NULL;
    int flags = (writing ? O_WRONLY : O_RDONLY) | O_CLOEXEC | O_NONBLOCK;
    int fd = bx_openat_resolve(root, req->path, flags, 0,
                               RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV);
    if (fd < 0) {
        if (errno == ENOSYS)
            bx_diag(diag, "secure /proc/sys lookup requires openat2");
        else
            bx_perror_path(diag, req->path);
        return false;
    }
    bool ok = false;
    struct stat st;
    if (fstat(fd, &st) < 0) {
        bx_perror_path(diag, req->path);
        goto done;
    }
    if (!S_ISREG(st.st_mode)) {
        bx_diag(diag, "'%s' is not a sysctl value", req->path);
        goto done;
    }
    if (writing) {
        char newline = '\n';
        size_t len = strlen(req->value);
        const struct iovec data[] = {{req->value, len}, {&newline, 1}};
        ssize_t n;
        /* A second write at a nonzero offset is not a reliable continuation. */
        do {
            n = writev(fd, data, 2);
        } while (n < 0 && errno == EINTR);
        if (n < 0 || (size_t)n != len + 1) {
            if (n >= 0)
                errno = EIO;
            bx_perror_path(diag, req->path);
            goto done;
        }
    }
    ok = (quiet && writing) || sysctl_emit(fd, req, values, names, diag);
done:
    return bx_fd_close(&fd, ok ? req->path : NULL, diag) && ok;
}
#endif

int bx_sysctl_main(int argc, char** argv) {
    int handled = bx_cli_maybe_handle_help_or_version(argc, argv, "sysctl", "-h", "-V", usage);
    if (handled >= 0)
        return handled;
    struct bx_diag_ctx diag = {.progname = "sysctl"};
#ifdef __linux__
    static const struct bx_option_spec options[] = {
        {'n', 'n', "values", BX_OPTION_NO_VALUE},
        {'N', 'N', "names", BX_OPTION_NO_VALUE},
        {'q', 'q', "quiet", BX_OPTION_NO_VALUE},
        {'w', 'w', "write", BX_OPTION_NO_VALUE},
    };
    bool writing = false, quiet = false, values = false, names = false;
    struct bx_option_cursor cursor;
    size_t first;
    bx_option_cursor_init(&cursor, argv, 1, (size_t)argc, BX_OPTION_PREFIX | BX_OPTION_ABBREVIATE,
                          options, sizeof(options) / sizeof(options[0]));
    for (;;) {
        struct bx_option_result result = bx_option_next(&cursor);
        if (result.event == BX_OPTION_END) {
            first = result.index;
            break;
        }
        if (result.event != BX_OPTION_MATCH) {
            if (result.short_name)
                bx_diag(&diag, "invalid option -- '%c'", result.short_name);
            else
                bx_diag(&diag, "unrecognized option '%s'", result.token);
            return 1;
        }
        switch (result.spec->id) {
        case 'n': values = true; break;
        case 'N': names = true; break;
        case 'q': quiet = true; break;
        case 'w': writing = true; break;
        }
    }
    size_t count = (size_t)argc - first;
    if (!count || (names && (values || writing)) || (!writing && quiet) || (quiet && values)) {
        usage(stderr, diag.progname);
        return 1;
    }
    struct sysctl_request* requests = calloc(count, sizeof(*requests));
    if (requests == NULL) {
        bx_perror_path(&diag, "settings");
        return 1;
    }

    int root = -1, status = 1;
    for (size_t i = 0; i < count; i++) {
        char* arg = argv[first + i];
        char* eq = strchr(arg, '=');
        if (writing != (eq != NULL)) {
            bx_diag(&diag, writing ? "expected KEY=VALUE" : "use -w to write settings");
            goto done;
        }
        size_t len = eq ? (size_t)(eq - arg) : strlen(arg);
        if (len >= PATH_MAX) {
            bx_diag(&diag, "sysctl key is too long");
            goto done;
        }
        requests[i].path = strndup(arg, len);
        if (requests[i].path == NULL) {
            bx_perror_path(&diag, "sysctl key");
            goto done;
        }
        if (!sysctl_key_path(requests[i].path) || (eq && strpbrk(eq + 1, "\r\n"))) {
            bx_diag(&diag, "invalid sysctl key or value");
            goto done;
        }
        if (eq)
            requests[i].value = eq + 1;
    }

    root = bx_fd_open_cloexec("/proc/sys", O_RDONLY | O_DIRECTORY | O_NOFOLLOW, 0);
    struct statfs fs;
    if (root < 0 || fstatfs(root, &fs) < 0) {
        bx_perror_path(&diag, "/proc/sys");
        goto done;
    }
    if (fs.f_type != PROC_SUPER_MAGIC) {
        bx_diag(&diag, "/proc/sys is not procfs");
        goto done;
    }
    for (size_t i = 0; i < count; i++)
        if (!sysctl_apply(root, &requests[i], quiet, values, names, &diag))
            goto done;
    status = 0;
done:
    bx_fd_cleanup(&root);
    for (size_t i = 0; i < count; i++)
        free(requests[i].path);
    free(requests);
    return status;
#else
    bx_diag(&diag, "Linux sysctl is not supported on this platform");
    return 1;
#endif
}
