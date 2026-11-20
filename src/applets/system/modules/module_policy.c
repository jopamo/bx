#define _GNU_SOURCE
#include "module_policy.h"

#ifdef __linux__
#include <linux/magic.h>
#include <sys/vfs.h>

#include "bx/diag.h"
#include "lib/fd_ops.h"
#include "lib/xreadwrite.h"

enum { POLICY_LIMIT = 64 * 1024, BLACKLIST_LIMIT = 4096 };

void bx_module_policy_close(struct bx_module_policy* policy) {
    free(policy->blocked);
    free(policy->config);
    free(policy->cmdline);
}

static int compare_names(const void* a, const void* b) {
    return bx_module_name_cmp(*(const struct bx_module_path*)a, *(const struct bx_module_path*)b);
}

bool bx_module_policy_blocks(const struct bx_module_policy* policy, struct bx_module_path name) {
    return bsearch(&name, policy->blocked, policy->count, sizeof(name), compare_names) != NULL;
}

static int add_name(struct bx_module_policy* policy, const char* text, size_t len) {
    if (!bx_module_name_span_valid(text, len) || policy->count == BLACKLIST_LIMIT) {
        errno = policy->count == BLACKLIST_LIMIT ? E2BIG : EINVAL;
        return -1;
    }
    policy->blocked[policy->count++] = (struct bx_module_path) {text, len};
    return 0;
}

static bool blank(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

static int config_records(struct bx_module_policy* policy, size_t size) {
    char* cursor = policy->config;
    char* end = cursor + size;
    static const char keyword[] = "blacklist";
    while (cursor < end) {
        char* nl = memchr(cursor, '\n', (size_t)(end - cursor));
        char* stop = nl ? nl : end;
        char* line = cursor;
        cursor = nl ? nl + 1 : end;
        while (line < stop && blank(*line))
            line++;
        if (line == stop || *line == '#')
            continue;
        size_t len = sizeof(keyword) - 1;
        if ((size_t)(stop - line) <= len || memcmp(line, keyword, len) != 0 || !blank(line[len])) {
            errno = ENOTSUP;
            return -1;
        }
        line += len;
        while (line < stop && blank(*line))
            line++;
        char* name = line;
        while (line < stop && !blank(*line) && *line != '#')
            line++;
        len = (size_t)(line - name);
        while (line < stop && blank(*line))
            line++;
        if (line < stop && *line != '#') {
            errno = EINVAL;
            return -1;
        }
        if (add_name(policy, name, len) < 0)
            return -1;
    }
    return 0;
}

static bool arg_space(char c) {
    return blank(c) || c == '\n' || c == '\v' || c == '\f';
}

static int cmdline_records(struct bx_module_policy* policy) {
    static const char key[] = "modprobe.blacklist";
    char* p = policy->cmdline;
    while (*p != '\0') {
        if (arg_space(*p)) {
            p++;
            continue;
        }
        char* token = p;
        char* out = token;
        bool quoted = false;
        while (*p != '\0') {
            if (*p == '"')
                quoted = !quoted;
            else if (!quoted && arg_space(*p))
                break;
            else
                *out++ = *p;
            p++;
        }
        if (quoted) {
            errno = EINVAL;
            return -1;
        }
        if (*p != '\0')
            p++;
        *out = '\0';
        size_t keylen = sizeof(key) - 1;
        if (strncmp(token, key, keylen) != 0 || token[keylen] != '=')
            continue;
        char* value = token + keylen + 1;
        if (*value == '\0')
            continue;
        for (;;) {
            char* comma = strchr(value, ',');
            size_t len = comma ? (size_t)(comma - value) : strlen(value);
            if (add_name(policy, value, len) < 0)
                return -1;
            if (comma == NULL)
                break;
            value = comma + 1;
        }
    }
    return 0;
}

static int read_cmdline(struct bx_module_policy* policy) {
    int fd = bx_fd_open_cloexec("/proc/cmdline", O_RDONLY | O_NOFOLLOW | O_NONBLOCK, 0);
    if (fd < 0)
        return -1;
    int error = 0;
    struct statfs fs;
    if (fstatfs(fd, &fs) < 0)
        error = errno;
    else if (fs.f_type != PROC_SUPER_MAGIC)
        error = EXDEV;
    else {
        policy->cmdline = malloc(POLICY_LIMIT + 1);
        if (policy->cmdline == NULL)
            error = errno;
    }
    size_t pos = 0;
    while (error == 0) {
        ssize_t n = bx_xread(fd, policy->cmdline + pos, POLICY_LIMIT + 1 - pos);
        if (n < 0) {
            error = errno;
            break;
        }
        if (n == 0)
            break;
        pos += (size_t)n;
        if (pos > POLICY_LIMIT)
            error = EFBIG;
    }
    bx_fd_cleanup(&fd);
    if (error == 0 && memchr(policy->cmdline, '\0', pos) != NULL)
        error = EINVAL;
    if (error != 0) {
        errno = error;
        return -1;
    }
    policy->cmdline[pos] = '\0';
    return cmdline_records(policy);
}

int bx_module_policy_open(struct bx_module_policy* policy, const char* path, struct bx_diag_ctx* diag) {
    policy->blocked = calloc(BLACKLIST_LIMIT, sizeof(*policy->blocked));
    if (policy->blocked == NULL) {
        bx_perror_path(diag, "module policy");
        return -1;
    }
    int fd = bx_fd_open_cloexec(path, O_RDONLY | O_NONBLOCK, 0);
    if (fd < 0) {
        bx_perror_path(diag, path);
        return -1;
    }
    size_t size;
    policy->config = bx_module_read_regular(fd, POLICY_LIMIT, &size);
    int error = errno;
    bx_fd_cleanup(&fd);
    if (policy->config == NULL) {
        errno = error;
        bx_perror_path(diag, path);
        return -1;
    }
    if (memchr(policy->config, '\0', size) != NULL || config_records(policy, size) < 0) {
        bx_diag(diag, "'%s': invalid or unsupported policy; only blacklist directives are supported", path);
        return -1;
    }
    if (read_cmdline(policy) < 0) {
        bx_perror_path(diag, "/proc/cmdline");
        return -1;
    }
    qsort(policy->blocked, policy->count, sizeof(*policy->blocked), compare_names);
    return 0;
}
#endif
