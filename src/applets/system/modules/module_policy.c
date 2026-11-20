#define _GNU_SOURCE
#include "module_policy.h"

#ifdef __linux__
#include <dirent.h>
#include <linux/magic.h>
#include <sys/vfs.h>

#include "bx/diag.h"
#include "lib/fd_ops.h"
#include "lib/xreadwrite.h"

enum {
    POLICY_LIMIT = 64 * 1024, POLICY_TOTAL_LIMIT = 1024 * 1024,
    RULE_LIMIT = 4096, POLICY_FILE_LIMIT = 1024, PARAMETER_LIMIT = 128 * 1024,
};

struct bx_module_rule {
    char* text;
    size_t name_len;
    size_t args_len;
    size_t order;
};

void bx_module_policy_close(struct bx_module_policy* policy) {
    for (size_t i = 0; i < policy->count; i++)
        free(policy->rules[i].text);
    free(policy->rules);
}

static struct bx_module_path rule_name(const struct bx_module_rule* rule) {
    return (struct bx_module_path) {rule->text, rule->name_len};
}

static int compare_rules(const void* a, const void* b) {
    const struct bx_module_rule* left = a;
    const struct bx_module_rule* right = b;
    int r = bx_module_name_cmp(rule_name(left), rule_name(right));
    return r ? r : (left->order > right->order) - (left->order < right->order);
}

static size_t first_rule(const struct bx_module_policy* policy, struct bx_module_path name) {
    size_t lo = 0, hi = policy->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (bx_module_name_cmp(rule_name(&policy->rules[mid]), name) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

bool bx_module_policy_blocks(const struct bx_module_policy* policy, struct bx_module_path name) {
    for (size_t i = first_rule(policy, name);
         i < policy->count && bx_module_name_cmp(rule_name(&policy->rules[i]), name) == 0; i++) {
        if (policy->rules[i].args_len == 0)
            return true;
    }
    return false;
}

char* bx_module_policy_parameters(const struct bx_module_policy* policy, struct bx_module_path name,
                                  const char* cli) {
    size_t cli_len = strlen(cli);
    size_t first = first_rule(policy, name), end = first;
    size_t size = cli_len;
    while (end < policy->count && bx_module_name_cmp(rule_name(&policy->rules[end]), name) == 0) {
        size_t len = policy->rules[end++].args_len;
        if (len != 0) {
            if (size > PARAMETER_LIMIT || len + 1 > PARAMETER_LIMIT - size) {
                errno = E2BIG;
                return NULL;
            }
            size += len + 1;
        }
    }
    if (size > PARAMETER_LIMIT) {
        errno = E2BIG;
        return NULL;
    }
    char* result = malloc(size + 1);
    if (result == NULL)
        return NULL;
    char* out = result;
    for (size_t i = first; i < end; i++) {
        const struct bx_module_rule* rule = &policy->rules[i];
        if (rule->args_len != 0) {
            memcpy(out, rule->text + rule->name_len, rule->args_len);
            out += rule->args_len;
            *out++ = ' ';
        }
    }
    if (cli_len == 0 && out != result)
        out--;
    memcpy(out, cli, cli_len + 1);
    return result;
}

/* A rule with no arguments is a blacklist entry. Names and arguments are copied. */
static int add_rule(struct bx_module_policy* policy, const char* name, size_t len,
                    const char* args, size_t args_len) {
    if (!bx_module_name_span_valid(name, len) || policy->count == RULE_LIMIT) {
        errno = policy->count == RULE_LIMIT ? E2BIG : EINVAL;
        return -1;
    }
    char* text = malloc(len + args_len);
    if (text == NULL)
        return -1;
    memcpy(text, name, len);
    if (args_len != 0)
        memcpy(text + len, args, args_len);
    policy->rules[policy->count] = (struct bx_module_rule) {text, len, args_len, policy->count};
    policy->count++;
    return 0;
}

static bool blank(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

static int config_records(struct bx_module_policy* policy, const char* cursor, size_t size) {
    const char* end = cursor + size;
    while (cursor < end) {
        const char* nl = memchr(cursor, '\n', (size_t)(end - cursor));
        const char* stop = nl ? nl : end;
        const char* line = cursor;
        cursor = nl ? nl + 1 : end;
        while (line < stop && blank(*line))
            line++;
        if (line == stop || *line == '#')
            continue;
        const char* keyword = line;
        while (line < stop && !blank(*line))
            line++;
        size_t len = (size_t)(line - keyword);
        bool options = len == 7 && memcmp(keyword, "options", 7) == 0;
        if (!options && !(len == 9 && memcmp(keyword, "blacklist", 9) == 0)) {
            errno = ENOTSUP;
            return -1;
        }
        while (line < stop && blank(*line))
            line++;
        const char* name = line;
        while (line < stop && !blank(*line) && *line != '#')
            line++;
        len = (size_t)(line - name);
        while (line < stop && blank(*line))
            line++;
        if (!options) {
            if (line != stop && *line != '#')
                goto invalid;
            if (add_rule(policy, name, len, NULL, 0) < 0)
                return -1;
            continue;
        }
        const char* args = line;
        bool quote = false;
        for (; line < stop; line++) {
            if (*line == '"')
                quote = !quote;
            else if (*line == '#' && !quote)
                break;
            else if (*line == '\\' || *line == 127 ||
                     ((unsigned char)*line < ' ' && !blank(*line)))
                goto invalid;
        }
        while (line > args && blank(line[-1]))
            line--;
        if (quote || line == args)
            goto invalid;
        if (add_rule(policy, name, len, args, (size_t)(line - args)) < 0)
            return -1;
    }
    return 0;
invalid:
    errno = EINVAL;
    return -1;
}

static bool arg_space(char c) {
    return blank(c) || c == '\n' || c == '\v' || c == '\f';
}

static int cmdline_records(struct bx_module_policy* policy, char* p) {
    static const char key[] = "modprobe.blacklist";
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
            if (add_rule(policy, value, len, NULL, 0) < 0)
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
    char* cmdline = NULL;
    if (fstatfs(fd, &fs) < 0)
        error = errno;
    else if (fs.f_type != PROC_SUPER_MAGIC)
        error = EXDEV;
    else {
        cmdline = malloc(POLICY_LIMIT + 1);
        if (cmdline == NULL)
            error = errno;
    }
    size_t pos = 0;
    while (error == 0) {
        ssize_t n = bx_xread(fd, cmdline + pos, POLICY_LIMIT + 1 - pos);
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
    if (error == 0 && memchr(cmdline, '\0', pos) != NULL)
        error = EINVAL;
    if (error == 0) {
        cmdline[pos] = '\0';
        if (cmdline_records(policy, cmdline) < 0)
            error = errno;
    }
    free(cmdline);
    errno = error;
    return error ? -1 : 0;
}

static int read_config(struct bx_module_policy* policy, int dir, const char* name, const char* path,
                       size_t* total, struct bx_diag_ctx* diag) {
    int flags = O_RDONLY | O_NONBLOCK;
    if (dir != AT_FDCWD)
        flags |= O_NOFOLLOW;
    int fd = bx_fd_openat_cloexec(dir, name, flags, 0);
    if (fd < 0) {
        if (dir != AT_FDCWD && errno == ELOOP) {
            /* A higher-priority .conf symlink to /dev/null masks this basename. */
            char link[sizeof("/dev/null")];
            ssize_t n = readlinkat(dir, name, link, sizeof(link));
            if (n == (ssize_t)sizeof(link) - 1 && memcmp(link, "/dev/null", (size_t)n) == 0)
                return 0;
            errno = ELOOP;
        }
        bx_perror_path(diag, path);
        return -1;
    }
    size_t size;
    size_t limit = *total < POLICY_LIMIT ? *total : POLICY_LIMIT;
    char* data = bx_module_read_regular(fd, limit, &size);
    int error = errno;
    bx_fd_cleanup(&fd);
    if (data == NULL) {
        errno = error;
        bx_perror_path(diag, path);
        return -1;
    }
    *total -= size;
    bool valid = memchr(data, '\0', size) == NULL && config_records(policy, data, size) == 0;
    free(data);
    if (!valid) {
        bx_diag(diag, "'%s': invalid or unsupported policy; only blacklist and options are supported", path);
        return -1;
    }
    return 0;
}

static const char* const config_dirs[] = {
    "/etc/modprobe.d", "/run/modprobe.d", "/usr/local/lib/modprobe.d",
    "/usr/lib/modprobe.d", "/lib/modprobe.d",
};

struct config_file {
    char* name;
    size_t priority;
};

static int compare_files(const void* a, const void* b) {
    const struct config_file* left = a;
    const struct config_file* right = b;
    int r = strcmp(left->name, right->name);
    return r ? r : (left->priority > right->priority) - (left->priority < right->priority);
}

static int collect_config(DIR* dir, struct config_file* files, size_t* count, size_t priority) {
    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(dir);
        if (entry == NULL)
            return errno ? -1 : 0;
        size_t len = strlen(entry->d_name);
        if (len <= 5 || strcmp(entry->d_name + len - 5, ".conf") != 0)
            continue;
        if (*count == POLICY_FILE_LIMIT) {
            errno = E2BIG;
            return -1;
        }
        char* name = strdup(entry->d_name);
        if (name == NULL)
            return -1;
        files[(*count)++] = (struct config_file) {name, priority};
    }
}

static int default_configs(struct bx_module_policy* policy, size_t* total, struct bx_diag_ctx* diag) {
    DIR* dirs[sizeof(config_dirs) / sizeof(config_dirs[0])] = {0};
    struct config_file* files = calloc(POLICY_FILE_LIMIT, sizeof(*files));
    if (files == NULL) {
        bx_perror_path(diag, "module policy");
        return -1;
    }
    int r = -1;
    size_t count = 0;
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        int fd = bx_fd_open_cloexec(config_dirs[i], O_RDONLY | O_DIRECTORY, 0);
        if (fd < 0 && errno == ENOENT)
            continue;
        if (fd < 0 || (dirs[i] = fdopendir(fd)) == NULL) {
            bx_perror_path(diag, config_dirs[i]);
            bx_fd_cleanup(&fd);
            goto out;
        }
        if (collect_config(dirs[i], files, &count, i) < 0) {
            bx_perror_path(diag, config_dirs[i]);
            goto out;
        }
    }
    qsort(files, count, sizeof(*files), compare_files);
    for (size_t i = 0; i < count; i++) {
        if (i != 0 && strcmp(files[i - 1].name, files[i].name) == 0)
            continue;
        const struct config_file* file = &files[i];
        char* path;
        if (asprintf(&path, "%s/%s", config_dirs[file->priority], file->name) < 0) {
            bx_perror_path(diag, "module policy path");
            goto out;
        }
        int read_result = read_config(policy, dirfd(dirs[file->priority]), file->name, path, total, diag);
        free(path);
        if (read_result < 0)
            goto out;
    }
    r = 0;
out:
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        if (dirs[i] != NULL)
            closedir(dirs[i]);
    for (size_t i = 0; i < count; i++)
        free(files[i].name);
    free(files);
    return r;
}

int bx_module_policy_open(struct bx_module_policy* policy, const char* path, struct bx_diag_ctx* diag) {
    policy->rules = calloc(RULE_LIMIT, sizeof(*policy->rules));
    if (policy->rules == NULL) {
        bx_perror_path(diag, "module policy");
        return -1;
    }
    size_t total = POLICY_TOTAL_LIMIT;
    if ((path ? read_config(policy, AT_FDCWD, path, path, &total, diag)
              : default_configs(policy, &total, diag)) < 0)
        return -1;
    if (read_cmdline(policy) < 0) {
        bx_perror_path(diag, "/proc/cmdline");
        return -1;
    }
    qsort(policy->rules, policy->count, sizeof(*policy->rules), compare_rules);
    return 0;
}
#endif
