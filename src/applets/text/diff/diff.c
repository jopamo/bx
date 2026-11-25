/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets.h"
#include "applets/text/edit/vim/src/xdiff/xdiff.h"
#include "bx/diag.h"
#include "lib/args_common.h"
#include "lib/fd_ops.h"
#include "lib/glob_syntax.h"
#include "lib/option_scan.h"
#include "lib/output_quote.h"
#include "lib/path_ops.h"
#include "lib/text_buffer.h"

/* Bound the input and per-line memory of the interactive editor's engine. */
enum {
    DIFF_BYTES_MAX = 8 * 1024 * 1024,
    DIFF_LINES_MAX = 100000,
    DIFF_MINIMAL_LINES_MAX = 4096,
    DIFF_TREE_ENTRIES_MAX = 65536,
    DIFF_TREE_DEPTH_MAX = 48,
    DIFF_TREE_BYTES_MAX = 256 * 1024 * 1024,
    DIFF_MINIMAL_WORK_MAX = 128 * 1024 * 1024,
    DIFF_EXCLUDES_MAX = 32,
    DIFF_EXCLUDE_BYTES_MAX = 256,
};

struct diff_state {
    struct bx_diag_ctx diag;
    size_t context;
    bool minimal;
    bool recursive;
    char excludes[DIFF_EXCLUDES_MAX][DIFF_EXCLUDE_BYTES_MAX + 2];
    size_t exclude_count;
    size_t entries_left;
    size_t bytes_left;
    uint64_t minimal_work_left;
};

static void usage(FILE* stream) {
    fputs("Usage: diff [-d] [-u | -U LINES] [-r [-x PATTERN]...] [--] LEFT RIGHT\n"
          "Compare two files or, with -r, two directories in unified format (three context lines by default).\n"
          "-x (or --exclude) skips matching basenames in both directory trees.\n"
          "-d (or --minimal) performs a minimal comparison, limited to 4096 text lines.\n"
          "One file operand may be '-' for standard input.\n"
          "Limits per input: 8 MiB and 100000 text lines.\n", stream);
}

static bool read_input(int fd, const struct stat* st, const char* path, struct bx_text_buffer* data,
                       struct diff_state* state) {
    struct bx_diag_ctx* diag = &state->diag;
    if (S_ISREG(st->st_mode) && st->st_size > DIFF_BYTES_MAX) {
        bx_diag(diag, "%s: input exceeds 8 MiB", path);
        return false;
    }
    char block[16384];
    for (;;) {
        ssize_t n = read(fd, block, sizeof(block));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            bx_perror_path(diag, path);
            return false;
        }
        if (n == 0)
            return true;
        if ((size_t)n > DIFF_BYTES_MAX - data->length) {
            bx_diag(diag, "%s: input exceeds 8 MiB", path);
            return false;
        }
        if (state->recursive) {
            if ((size_t)n > state->bytes_left) {
                bx_diag(diag, "directory input exceeds 256 MiB");
                return false;
            }
            state->bytes_left -= (size_t)n;
        }
        if (!bx_text_buffer_append_span(data, block, (size_t)n)) {
            bx_perror_path(diag, path);
            return false;
        }
    }
}

static size_t count_lines(const struct bx_text_buffer* data, size_t limit) {
    size_t lines = data->length != 0 && data->data[data->length - 1] != '\n';
    const char* next = data->data;
    size_t remaining = data->length;
    while (remaining != 0) {
        const char* found = memchr(next, '\n', remaining);
        if (!found)
            break;
        if (++lines > limit)
            break;
        remaining -= (size_t)(found - next) + 1u;
        next = found + 1;
    }
    return lines;
}

struct diff_output {
    const char* left;
    const char* right;
    bool header;
};

static int emit_lines(void* private, mmbuffer_t* lines, int count) {
    struct diff_output* output = private;
    if (!output->header) {
        if (printf("--- %s\n+++ %s\n", output->left, output->right) < 0)
            return -1;
        output->header = true;
    }
    for (int i = 0; i < count; i++) {
        size_t length = (size_t)lines[i].size;
        if (fwrite(lines[i].ptr, 1, length, stdout) != length)
            return -1;
    }
    return 0;
}

static char* quote_path(const char* path) {
    /* Tabs delimit optional timestamps; controls and quotes cannot appear
     * literally in a header that another tool may parse. */
    for (const unsigned char* p = (const unsigned char*)path; *p; p++) {
        if (*p < ' ' || *p >= 127 || *p == '"' || *p == '\\')
            return bx_output_quote_dup(path, BX_OUTPUT_QUOTE_C);
    }
    return strdup(path);
}

static int compare_files(struct diff_state* state, const int* fds, const struct stat* st,
                         const char* left, const char* right) {
    struct bx_diag_ctx* diag = &state->diag;
    struct bx_text_buffer a = {0}, b = {0};
    char* left_label = NULL;
    char* right_label = NULL;
    int status = 2;
    if (!read_input(fds[0], &st[0], left, &a, state) ||
        !read_input(fds[1], &st[1], right, &b, state))
        goto done;
    if (a.length == b.length && (!a.length || memcmp(a.data, b.data, a.length) == 0)) {
        status = 0;
        goto done;
    }
    left_label = quote_path(left);
    right_label = quote_path(right);
    if (!left_label || !right_label) {
        bx_perror_path(diag, "file label");
        goto done;
    }
    if ((a.length && memchr(a.data, '\0', a.length)) ||
        (b.length && memchr(b.data, '\0', b.length))) {
        if (printf("Binary files %s and %s differ\n", left_label, right_label) >= 0)
            status = 1;
    }
    else {
        // Minimal search skips xdiff's early cutoffs, so it needs a lower line cap.
        size_t limit = state->minimal ? DIFF_MINIMAL_LINES_MAX : DIFF_LINES_MAX;
        size_t left_lines = count_lines(&a, limit);
        size_t right_lines = count_lines(&b, limit);
        if (left_lines > limit || right_lines > limit) {
            bx_diag(diag, "input exceeds %zu text lines", limit);
            goto done;
        }
        if (state->recursive && state->minimal) {
            uint64_t work = (uint64_t)left_lines * right_lines;
            if (work > state->minimal_work_left) {
                bx_diag(diag, "directory comparison exceeds minimal search limit");
                goto done;
            }
            state->minimal_work_left -= work;
        }
        mmfile_t old = {.ptr = a.data ? a.data : "", .size = (long)a.length};
        mmfile_t new = {.ptr = b.data ? b.data : "", .size = (long)b.length};
        xpparam_t params = {.flags = state->minimal ? XDF_NEED_MINIMAL : 0};
        xdemitconf_t config = {.ctxlen = (long)state->context};
        struct diff_output output = {.left = left_label, .right = right_label};
        xdemitcb_t callbacks = {.priv = &output, .out_line = emit_lines};
        if (xdl_diff(&old, &new, &params, &config, &callbacks) == 0)
            status = 1;
        else if (!ferror(stdout))
            bx_diag(diag, "could not compute the difference");
    }
done:
    free(left_label);
    free(right_label);
    bx_text_buffer_destroy(&a);
    bx_text_buffer_destroy(&b);
    return status;
}

struct diff_names {
    char** names;
    size_t count;
    size_t capacity;
};

static int name_compare(const void* a, const void* b) {
    return strcmp(*(char* const*)a, *(char* const*)b);
}

static bool excluded(const struct diff_state* state, const char* name) {
    for (size_t i = 0; i < state->exclude_count; i++)
        if (fnmatch(state->excludes[i], name, 0) == 0)
            return true;
    return false;
}

static bool collect_names(struct diff_state* state, DIR* dir, const char* path,
                          struct diff_names* entries) {
    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(dir);
        if (!entry) {
            if (errno)
                break;
            if (entries->count > 1)
                qsort(entries->names, entries->count, sizeof(*entries->names), name_compare);
            return true;
        }
        const char* name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        if (state->entries_left == 0) {
            bx_diag(&state->diag, "directory comparison exceeds %d entries", DIFF_TREE_ENTRIES_MAX);
            return false;
        }
        state->entries_left--;
        if (excluded(state, name))
            continue;
        if (entries->count == entries->capacity) {
            size_t capacity = entries->capacity ? 2 * entries->capacity : 32;
            char** names = realloc(entries->names, capacity * sizeof(*names));
            if (!names)
                break;
            entries->names = names;
            entries->capacity = capacity;
        }
        char* owned = strdup(name);
        if (!owned)
            break;
        entries->names[entries->count++] = owned;
    }
    bx_perror_path(&state->diag, path);
    return false;
}

static int report_paths(struct diff_state* state, const char* format,
                        const char* left, const char* right) {
    char* a = quote_path(left);
    char* b = quote_path(right);
    int status = 2;
    if (!a || !b)
        bx_perror_path(&state->diag, "file label");
    else if (printf(format, a, b) >= 0)
        status = 1;
    free(a);
    free(b);
    return status;
}

static int compare_directories(struct diff_state*, const int*, const char* const*, unsigned);

static int compare_children(struct diff_state* state, const int* parents,
                            const char* const* paths, const char* name, unsigned depth) {
    struct stat st[2];
    for (size_t i = 0; i < 2; i++) {
        if (fstatat(parents[i], name, &st[i], AT_SYMLINK_NOFOLLOW) < 0) {
            bx_perror_path(&state->diag, paths[i]);
            return 2;
        }
    }
    if ((st[0].st_mode & S_IFMT) != (st[1].st_mode & S_IFMT))
        return report_paths(state, "File types differ: %s and %s\n", paths[0], paths[1]);
    if (S_ISLNK(st[0].st_mode)) {
        char targets[2][4096];
        ssize_t lengths[2];
        for (size_t i = 0; i < 2; i++) {
            lengths[i] = readlinkat(parents[i], name, targets[i], sizeof(targets[i]));
            if (lengths[i] < 0) {
                bx_perror_path(&state->diag, paths[i]);
                return 2;
            }
            if ((size_t)lengths[i] == sizeof(targets[i])) {
                bx_diag(&state->diag, "%s: symbolic link target too long", paths[i]);
                return 2;
            }
        }
        return lengths[0] == lengths[1] &&
               memcmp(targets[0], targets[1], (size_t)lengths[0]) == 0 ? 0 :
               report_paths(state, "Symbolic links %s and %s differ\n", paths[0], paths[1]);
    }
    bool directory = S_ISDIR(st[0].st_mode);
    if (!directory && !S_ISREG(st[0].st_mode)) {
        bx_diag(&state->diag, "%s: unsupported file type", paths[0]);
        return 2;
    }
    int fds[] = {-1, -1};
    int status = 2;
    for (size_t i = 0; i < 2; i++) {
        if (directory) {
            fds[i] = bx_fd_openat_child_nofollow(parents[i], name, O_RDONLY | O_DIRECTORY, 0);
            if (fds[i] >= 0 && bx_fd_fstat_expected(fds[i], &st[i], &st[i]) < 0) {
                bx_perror_path(&state->diag, paths[i]);
                goto done;
            }
        } else {
            fds[i] = bx_fd_openat_regular_verified(parents[i], name, &st[i], &st[i]);
        }
        if (fds[i] < 0) {
            bx_perror_path(&state->diag, paths[i]);
            goto done;
        }
    }
    status = directory ? compare_directories(state, fds, paths, depth + 1) :
                         compare_files(state, fds, st, paths[0], paths[1]);
done:
    bx_fd_cleanup(&fds[0]);
    bx_fd_cleanup(&fds[1]);
    return status;
}

static int compare_directories(struct diff_state* state, const int* fds,
                               const char* const* paths, unsigned depth) {
    if (depth >= DIFF_TREE_DEPTH_MAX) {
        bx_diag(&state->diag, "%s: directory comparison exceeds %d levels",
                paths[0], DIFF_TREE_DEPTH_MAX);
        return 2;
    }
    struct diff_names entries[2] = {0};
    int status = 2;
    for (size_t i = 0; i < 2; i++) {
        // Keep the parent fd owned by its caller; scan this directory once.
        int fd = bx_fd_dup_cloexec(fds[i]);
        DIR* dir = fd < 0 ? NULL : fdopendir(fd);
        if (!dir) {
            bx_perror_path(&state->diag, paths[i]);
            bx_fd_cleanup(&fd);
            goto done;
        }
        bool collected = collect_names(state, dir, paths[i], &entries[i]);
        int close_error = closedir(dir);
        if (!collected)
            goto done;
        if (close_error < 0) {
            bx_perror_path(&state->diag, paths[i]);
            goto done;
        }
    }
    status = 0;
    size_t a = 0, b = 0;
    while (a < entries[0].count || b < entries[1].count) {
        int cmp = a == entries[0].count ? 1 : b == entries[1].count ? -1 :
                  strcmp(entries[0].names[a], entries[1].names[b]);
        int child;
        if (cmp < 0)
            child = report_paths(state, "Only in %s: %s\n", paths[0], entries[0].names[a++]);
        else if (cmp > 0)
            child = report_paths(state, "Only in %s: %s\n", paths[1], entries[1].names[b++]);
        else {
            const char* name = entries[0].names[a++];
            b++;
            char* left = bx_path_join(paths[0], name);
            char* right = bx_path_join(paths[1], name);
            const char* children[] = {left, right};
            child = compare_children(state, fds, children, name, depth);
            free(left);
            free(right);
        }
        if (child > status)
            status = child;
        if (status == 2)
            break;
    }
done:
    for (size_t i = 0; i < 2; i++) {
        for (size_t j = 0; j < entries[i].count; j++)
            free(entries[i].names[j]);
        free(entries[i].names);
    }
    return status;
}

int bx_diff_main(int argc, char** argv) {
    struct diff_state state = {
        .diag = {.progname = "diff"}, .context = 3,
        .entries_left = DIFF_TREE_ENTRIES_MAX, .bytes_left = DIFF_TREE_BYTES_MAX,
        .minimal_work_left = DIFF_MINIMAL_WORK_MAX,
    };
    struct bx_diag_ctx* diag = &state.diag;
    static const struct bx_option_spec specs[] = {
        {'x', 'x', "exclude", BX_OPTION_REQUIRED_VALUE}, {'h', 'h', "help", BX_OPTION_NO_VALUE},          {'d', 'd', "minimal", BX_OPTION_NO_VALUE},
        {'r', 'r', "recursive", BX_OPTION_NO_VALUE},     {'U', 'U', "unified", BX_OPTION_REQUIRED_VALUE}, {'u', 'u', NULL, BX_OPTION_NO_VALUE},
    };
    const char* paths[2];
    size_t count = 0;
    bool extra = false;
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, argc > 0 ? 1u : 0u, (size_t)argc, BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE, specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_OPERAND) {
            if (count < 2)
                paths[count++] = option.token;
            else
                extra = true;
            continue;
        }
        if (option.event != BX_OPTION_MATCH) {
            if (option.event == BX_OPTION_MISSING_VALUE && option.short_name)
                bx_diag(diag, "option requires an argument -- '%c'", option.short_name);
            else if (option.event == BX_OPTION_MISSING_VALUE)
                bx_diag(diag, "option requires an argument -- '%s'", option.token);
            else if (option.short_name)
                bx_diag(diag, "invalid option -- '%c'", option.short_name);
            else
                bx_diag(diag, "unrecognized option '%s'", option.token);
            return 2;
        }
        switch (option.spec->id) {
            case 'd':
                state.minimal = true;
                break;
            case 'h':
                usage(stdout);
                return 0;
            case 'u':
                state.context = 3;
                break;
            case 'r':
                state.recursive = true;
                break;
            case 'x': {
                size_t length = strlen(option.value);
                if (state.exclude_count == DIFF_EXCLUDES_MAX || length > DIFF_EXCLUDE_BYTES_MAX) {
                    bx_diag(diag, "limit is %d exclusion patterns of up to %d bytes",
                            DIFF_EXCLUDES_MAX, DIFF_EXCLUDE_BYTES_MAX);
                    return 2;
                }
                char* pattern = state.excludes[state.exclude_count++];
                memcpy(pattern, option.value, length + 1);
                if (strstr(pattern, "[^"))
                    bx_glob_normalize_caret_with_spare(pattern, 1);
                break;
            }
            case 'U':
                if (!bx_args_parse_size_range(option.value, 0, DIFF_LINES_MAX, &state.context)) {
                    bx_diag(diag, "invalid context size: '%s'", option.value);
                    return 2;
                }
                break;
        }
    }
    if (extra || count != 2 || (strcmp(paths[0], "-") == 0 && strcmp(paths[1], "-") == 0)) {
        usage(stderr);
        return 2;
    }
    if (!*paths[0] || !*paths[1]) {
        bx_diag(diag, "empty file operand");
        return 2;
    }

    if (state.exclude_count && !state.recursive) {
        bx_diag(diag, "--exclude requires -r");
        return 2;
    }
    int fds[] = {-1, -1};
    bool owned[2] = {false, false};
    struct stat st[2];
    int status = 2;
    for (size_t i = 0; i < 2; i++) {
        owned[i] = strcmp(paths[i], "-") != 0;
        int flags = O_RDONLY | O_NOCTTY | (state.recursive ? O_NONBLOCK : 0);
        fds[i] = owned[i] ? bx_fd_open_cloexec(paths[i], flags, 0) : STDIN_FILENO;
        if (fds[i] < 0 || fstat(fds[i], &st[i]) < 0) {
            bx_perror_path(diag, paths[i]);
            goto done;
        }
        if (state.recursive && owned[i] && !S_ISREG(st[i].st_mode) && !S_ISDIR(st[i].st_mode)) {
            bx_diag(diag, "%s: unsupported file type", paths[i]);
            goto done;
        }
    }
    bool a_dir = S_ISDIR(st[0].st_mode), b_dir = S_ISDIR(st[1].st_mode);
    if (a_dir || b_dir) {
        if (!state.recursive || !a_dir || !b_dir)
            bx_diag(diag, "directory comparison requires -r and two directories");
        else
            status = compare_directories(&state, fds, paths, 0);
    } else
        status = compare_files(&state, fds, st, paths[0], paths[1]);
    if (fflush(stdout) == EOF || ferror(stdout)) {
        bx_perror_path(diag, "standard output");
        status = 2;
    }
done:
    for (size_t i = 0; i < 2; i++)
        if (owned[i])
            bx_fd_cleanup(&fds[i]);
    return status;
}
