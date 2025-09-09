#define _GNU_SOURCE
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include "argv_packer.h"

extern char **environ;

static size_t bx_argv_pointer_bytes(int argc) {
    if (argc < 0)
        return 0;
    return (size_t)(argc + 1) * sizeof(char *);
}

size_t bx_argv_environment_bytes(void) {
    size_t env_bytes = 0;
    if (!environ)
        return 0;

    for (char **ep = environ; *ep; ep++)
        env_bytes += strlen(*ep) + 1;
    return env_bytes;
}

size_t bx_argv_bytes(char **argv) {
    size_t total = 0;
    if (!argv)
        return 0;

    int argc = 0;
    for (int i = 0; argv[i]; i++) {
        total += strlen(argv[i]) + 1;
        argc++;
    }
    total += bx_argv_pointer_bytes(argc);
    return total;
}

size_t bx_argv_bytes_with_items(char **base_argv, int base_argc,
                                char **items, int start, int count) {
    size_t total = 0;
    for (int i = 0; i < base_argc; i++)
        total += strlen(base_argv[i]) + 1;
    for (int i = 0; i < count; i++)
        total += strlen(items[start + i]) + 1;
    total += bx_argv_pointer_bytes(base_argc + count);
    return total;
}

size_t bx_argv_bytes_with_replacement(char **base_argv, int base_argc,
                                      const char *marker, const char *replacement) {
    size_t total = 0;
    size_t marker_len = marker ? strlen(marker) : 0;
    size_t replacement_len = replacement ? strlen(replacement) : 0;

    for (int i = 0; i < base_argc; i++) {
        const char *arg = base_argv[i];
        size_t arg_bytes = strlen(arg) + 1;

        if (marker_len > 0) {
            const char *p = arg;
            while ((p = strstr(p, marker)) != NULL) {
                arg_bytes += replacement_len;
                arg_bytes -= marker_len;
                p += marker_len;
            }
        }

        total += arg_bytes;
    }

    total += bx_argv_pointer_bytes(base_argc);

    return total;
}

size_t bx_argv_effective_char_limit(int max_chars) {
    long arg_max = sysconf(_SC_ARG_MAX);
    size_t sys_limit = 0;
    if (arg_max > 0) {
        size_t env_bytes = bx_argv_environment_bytes();
        if ((size_t)arg_max > env_bytes)
            sys_limit = (size_t)arg_max - env_bytes;
    }

    if (max_chars > 0 && sys_limit > 0)
        return (size_t)max_chars < sys_limit ? (size_t)max_chars : sys_limit;
    if (max_chars > 0)
        return (size_t)max_chars;
    return sys_limit;
}

int bx_argv_select_batch_count(char **base_argv, int base_argc,
                               char **items, const int *line_groups,
                               int item_count, int start,
                               int max_args, int max_lines,
                               size_t char_limit) {
    int capped_args = max_args > 0 ? max_args : (item_count - start);
    int capped_lines = max_lines > 0 ? max_lines : (item_count - start);
    int take = 0;
    int used_lines = 0;
    int last_group = -1;
    size_t bytes = bx_argv_bytes_with_items(base_argv, base_argc, items, start, 0);

    if (char_limit > 0 && bytes > char_limit)
        return -1;

    while (start + take < item_count && take < capped_args) {
        int group = line_groups[start + take];
        if (take == 0 || group != last_group) {
            if (used_lines >= capped_lines)
                break;
            used_lines++;
            last_group = group;
        }
        if (char_limit > 0) {
            size_t next_bytes = bytes + strlen(items[start + take]) + 1;
            if (next_bytes > char_limit)
                break;
            bytes = next_bytes;
        }
        take++;
    }

    return take > 0 ? take : -1;
}
