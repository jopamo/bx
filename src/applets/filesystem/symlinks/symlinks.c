/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Independent implementation of the symlinks command-line interface. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dispatch/applets.h"

struct options { bool change, remove, other, recurse, shorten, test, verbose; int status; };

static void *memory(size_t n) {
    void *p = malloc(n);
    if (!p) { perror("symlinks"); exit(1); }
    return p;
}

static void error(struct options *o, const char *path) {
    fprintf(stderr, "symlinks: %s: %s\n", path, strerror(errno));
    o->status = 1;
}

static char *join(const char *a, const char *b) {
    size_t n = strlen(a), m = strlen(b);
    if (n && a[n - 1] == '/') n--;
    if (n > SIZE_MAX - m - 2) { errno = ENOMEM; perror("symlinks"); exit(1); }
    char *p = memory(n + m + 2);
    memcpy(p, a, n);
    p[n] = '/';
    memcpy(p + n + 1, b, m + 1);
    return p;
}

static char *link_text(int fd, const char *name) {
    for (size_t cap = 256; cap <= SIZE_MAX / 2; cap *= 2) {
        char *p = memory(cap + 1);
        ssize_t n = readlinkat(fd, name, p, cap);
        if (n < 0) { free(p); return NULL; }
        if ((size_t)n < cap) { p[n] = 0; return p; }
        free(p);
    }
    errno = ENAMETOOLONG;
    return NULL;
}

/* Remove empty and dot components, never cancel a component before "..". */
static char *tidy(const char *s) {
    size_t len = strlen(s), out = 0;
    char *p = memory(len + 2);
    if (*s == '/') p[out++] = '/';
    for (size_t i = 0; i < len;) {
        while (s[i] == '/') i++;
        size_t begin = i;
        while (s[i] && s[i] != '/') i++;
        size_t n = i - begin;
        if (!n || (n == 1 && s[begin] == '.')) continue;
        if (out && p[out - 1] != '/') p[out++] = '/';
        memcpy(p + out, s + begin, n);
        out += n;
    }
    if (!out) p[out++] = '.';
    if (len && s[len - 1] == '/' && p[out - 1] != '/') p[out++] = '/';
    p[out] = 0;
    return p;
}

/* base is a physical directory. target's remaining components stay verbatim. */
static char *relative(const char *base, const char *target) {
    const char *b = base + 1, *t = target + 1;
    while (*b && *t) {
        size_t nb = strcspn(b, "/"), nt = strcspn(t, "/");
        if (nb != nt || memcmp(b, t, nb)) break;
        b += nb; t += nt;
        if (*b == '/') b++;
        if (*t == '/') t++;
    }
    size_t up = 0;
    for (const char *p = b; *p; p++) if (p == b || p[-1] == '/') up++;
    size_t len = strlen(t);
    if (up > (SIZE_MAX - len - 2) / 3) { errno = ENOMEM; perror("symlinks"); exit(1); }
    char *p = memory(up * 3 + len + 2), *w = p;
    for (size_t i = 0; i < up; i++) { memcpy(w, "../", 3); w += 3; }
    if (len) { memcpy(w, t, len); w += len; }
    else if (w == p) *w++ = '.';
    *w = 0;
    return p;
}

static bool same_link(int fd, const char *name, const struct stat *old, const char *text) {
    struct stat now;
    if (fstatat(fd, name, &now, AT_SYMLINK_NOFOLLOW)) return false;
    if (!S_ISLNK(now.st_mode) || now.st_dev != old->st_dev || now.st_ino != old->st_ino) {
        errno = ESTALE; return false;
    }
    char *current = link_text(fd, name);
    if (!current) return false;
    bool same = !strcmp(current, text);
    free(current);
    if (!same) errno = ESTALE;
    return same;
}

static int replace_link(int fd, const char *name, const struct stat *old,
                        const char *before, const char *after) {
    char temp[64];
    for (unsigned attempt = 0; attempt < 32; attempt++) {
        uint64_t nonce;
        ssize_t n;
        do { n = getrandom(&nonce, sizeof(nonce), 0); } while (n < 0 && errno == EINTR);
        if (n != sizeof(nonce)) { if (n >= 0) errno = EIO; return -1; }
        snprintf(temp, sizeof(temp), ".bx-symlinks-%016llx", (unsigned long long)nonce);
        if (!symlinkat(after, fd, temp)) {
            int result = -1;
            if (same_link(fd, name, old, before)) result = renameat(fd, temp, fd, name);
            int saved = errno;
            if (result) unlinkat(fd, temp, 0);
            errno = saved;
            return result;
        }
        if (errno != EEXIST) return -1;
    }
    errno = EEXIST;
    return -1;
}

static void report(const char *kind, const char *path, const char *text) {
    printf("%-9s %s -> %s\n", kind, path, text);
}

static void inspect_link(struct options *o, int fd, const char *base,
                         const char *name, const char *display, dev_t device,
                         const struct stat *st) {
    char *text = link_text(fd, name);
    if (!text) { error(o, display); return; }
    struct stat target;
    if (fstatat(fd, name, &target, 0)) {
        if (errno != ENOENT && errno != ENOTDIR && errno != ELOOP) {
            error(o, display); free(text); return;
        }
        report("dangling:", display, text);
        if (o->remove && !o->test) {
            if (!same_link(fd, name, st, text) || unlinkat(fd, name, 0)) error(o, display);
            else report("deleted:", display, text);
        }
        free(text); return;
    }
    if (target.st_dev != device && !o->other) {
        report("other_fs:", display, text);
        free(text); return;
    }
    char *clean = tidy(text), *candidate = NULL;
    const char *kind = "relative:";
    if (*text == '/') {
        kind = "absolute:";
        candidate = relative(base, clean);
    } else {
        if (strcmp(clean, text)) kind = "messy:";
        candidate = clean; clean = NULL;
        if (o->shorten) {
            char *parent = strdup(base);
            if (!parent) { perror("symlinks"); exit(1); }
            const char *tail = candidate;
            while (!strncmp(tail, "../", 3)) {
                char *slash = strrchr(parent, '/');
                if (slash == parent) parent[1] = 0;
                else if (slash) *slash = 0;
                tail += 3;
            }
            char *absolute = join(parent, tail);
            char *shorter = relative(base, absolute);
            if (strlen(shorter) < strlen(candidate)) {
                if (!strcmp(kind, "relative:")) kind = "lengthy:";
                free(candidate); candidate = shorter;
            } else free(shorter);
            free(absolute); free(parent);
        }
    }
    if (o->verbose || strcmp(kind, "relative:")) report(kind, display, text);
    if ((o->change || o->test) && strcmp(text, candidate)) {
        /* Validate resolution before changing the entry, without following it for writes. */
        struct stat proposed;
        if (fstatat(fd, candidate, &proposed, 0)) error(o, display);
        else if (proposed.st_dev != target.st_dev || proposed.st_ino != target.st_ino) {
            errno = ESTALE; error(o, display);
        } else if (!o->test && replace_link(fd, name, st, text, candidate)) error(o, display);
        else report("changed:", display, candidate);
    }
    free(candidate); free(clean); free(text);
}

static void scan(struct options *o, int fd, const char *base, const char *display,
                 dev_t device, unsigned depth) {
    DIR *dir = fdopendir(fd);
    if (!dir) { error(o, display); close(fd); return; }
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) { if (errno) error(o, display); break; }
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        struct stat st;
        char *path = join(display, name);
        if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW)) error(o, path);
        else if (S_ISLNK(st.st_mode)) inspect_link(o, fd, base, name, path, device, &st);
        else if (o->recurse && S_ISDIR(st.st_mode) && (o->other || st.st_dev == device)) {
            if (depth >= 1024) { errno = ELOOP; error(o, path); }
            else {
                int child = openat(fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
                if (child < 0) error(o, path);
                else {
                    struct stat opened;
                    if (fstat(child, &opened) || opened.st_dev != st.st_dev || opened.st_ino != st.st_ino) {
                        errno = ESTALE; error(o, path); close(child);
                    } else {
                        char *nextbase = join(base, name);
                        scan(o, child, nextbase, path, device, depth + 1);
                        free(nextbase);
                    }
                }
            }
        }
        free(path);
    }
    closedir(dir);
}

int bx_symlinks_main(int argc, char **argv) {
    struct options o = {0};
    int ch;
    while ((ch = getopt(argc, argv, "cdorstvh")) != -1) {
        switch (ch) {
        case 'c': o.change = true; break;
        case 'd': o.remove = true; break;
        case 'o': o.other = true; break;
        case 'r': o.recurse = true; break;
        case 's': o.shorten = true; break;
        case 't': o.test = true; break;
        case 'v': o.verbose = true; break;
        case 'h': puts("usage: symlinks [-cdorstv] directory ..."); return 0;
        default: return 1;
        }
    }
    if (optind == argc) { fputs("usage: symlinks [-cdorstv] directory ...\n", stderr); return 1; }
    for (int i = optind; i < argc; i++) {
        char *base = realpath(argv[i], NULL);
        if (!base) { error(&o, argv[i]); continue; }
        int fd = open(base, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0) error(&o, argv[i]);
        else {
            struct stat st;
            if (fstat(fd, &st)) { error(&o, argv[i]); close(fd); }
            else scan(&o, fd, base, argv[i], st.st_dev, 0);
        }
        free(base);
    }
    if (fflush(stdout) || ferror(stdout)) { errno = EIO; error(&o, "stdout"); }
    return o.status;
}
