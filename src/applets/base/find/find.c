#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "applets.h"
#include "bx/diag.h"
#include "search/walk.h"

struct find_opts {
    bool action_print;
    bool action_print0;
    bool action_delete;
    bool action_quit;
    bool depth_first;
    bool has_name;
    bool name_ignore_case;
    bool has_type;
    bool has_path;
    bool path_ignore_case;
    bool test_empty;
    bool test_readable;
    bool test_writable;
    bool test_executable;
    bool constant_predicate_set;
    bool constant_predicate_value;
    bool follow_root_symlink;
    const char *name_pattern;
    const char *path_pattern;
    char type_filter;
    int max_depth;
    int min_depth;
    bool follow_symlinks;
};

struct find_state {
    const char *progname;
    struct find_opts *opts;
    bool *stop;
    int status;
};

static void find_report_error(const char *progname, const char *path, int errnum) {
    fprintf(stderr, "%s: %s: %s\n", progname, path, strerror(errnum));
}

static void find_print_help(const char *progname) {
    printf("Usage: %s [PATH]... [EXPRESSION]\n", progname);
    puts("Search for files in a directory hierarchy.");
    puts("");
    puts("  -H            follow command-line symlinks");
    puts("  -L            follow all symlinks");
    puts("  -P            never follow symlinks");
    puts("  -depth        process directory contents before the directory");
    puts("  -maxdepth N   descend at most N levels below the roots");
    puts("  -mindepth N   do not act on levels less than N");
    puts("  -name PATTERN match basename against PATTERN");
    puts("  -type [fd]    match file type");
    puts("  -true         always true");
    puts("  -false        always false");
    puts("  -print        print path");
    puts("  -print0       print path followed by NUL");
    puts("  -delete       delete matched entries");
    puts("  -quit         stop after the first deciding result");
    puts("      --help    display this help and exit");
    puts("      --version output version information and exit");
}

static void find_print_version(const char *progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool parse_int_arg(const char *progname, const char *optname, const char *text, int *out) {
    char *end = NULL;
    long v = strtol(text, &end, 10);
    if (!text || *text == '\0' || (end && *end != '\0') || v < 0 || v > 1<<20) {
        fprintf(stderr, "%s: invalid argument to %s: %s\n", progname, optname, text ? text : "(null)");
        return false;
    }
    *out = (int)v;
    return true;
}

static bool find_matches_type(const struct walk_entry *entry, struct find_opts *opts, char type_filter) {
    switch (type_filter) {
    case 'f':
        return !entry->is_dir;
    case 'd':
        return entry->is_dir;
    case 'l': {
        if (opts->follow_symlinks)
            return false;
        struct stat st;
        return lstat(entry->path, &st) == 0 && S_ISLNK(st.st_mode);
    }
    default:
        return false;
    }
}

static bool find_match_pattern(const char *pattern, const char *text, bool ignore_case) {
    return fnmatch(pattern, text, ignore_case ? FNM_CASEFOLD : 0) == 0;
}

static bool find_is_empty(const struct walk_entry *entry) {
    struct stat st;
    if (lstat(entry->path, &st) != 0)
        return false;
    if (S_ISREG(st.st_mode))
        return st.st_size == 0;
    if (!S_ISDIR(st.st_mode))
        return false;

    DIR *dir = opendir(entry->path);
    if (!dir)
        return false;
    struct dirent *ent;
    bool empty = true;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
            empty = false;
            break;
        }
    }
    closedir(dir);
    return empty;
}

static const char *find_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void find_walk_cb(const struct walk_entry *entry, void *user) {
    struct find_state *st = user;
    struct find_opts *opts = st->opts;

    if (st->stop && *st->stop)
        return;

    if (entry->depth < opts->min_depth)
        return;
    if (opts->max_depth >= 0 && entry->depth > opts->max_depth)
        return;

    if (opts->has_name &&
        !find_match_pattern(opts->name_pattern, find_basename(entry->path), opts->name_ignore_case))
        return;
    if (opts->has_path &&
        !find_match_pattern(opts->path_pattern, entry->path, opts->path_ignore_case))
        return;
    if (opts->has_type && !find_matches_type(entry, opts, opts->type_filter))
        return;
    if (opts->test_empty && !find_is_empty(entry))
        return;
    if (opts->test_readable && access(entry->path, R_OK) != 0)
        return;
    if (opts->test_writable && access(entry->path, W_OK) != 0)
        return;
    if (opts->test_executable && access(entry->path, X_OK) != 0)
        return;
    if (opts->constant_predicate_set && !opts->constant_predicate_value)
        return;

    if (opts->action_delete) {
        if (strcmp(entry->path, ".") == 0) {
            errno = EBUSY;
            find_report_error(st->progname, entry->path, errno);
            st->status = 1;
            if (st->stop) *st->stop = true;
            return;
        }
        int rc = entry->is_dir ? rmdir(entry->path) : unlink(entry->path);
        if (rc != 0) {
            find_report_error(st->progname, entry->path, errno);
            st->status = 1;
            if (st->stop) *st->stop = true;
            return;
        }
    }

    if (opts->action_print) {
        printf("%s\n", entry->path);
    }
    if (opts->action_print0) {
        printf("%s%c", entry->path, '\0');
    }

    if (opts->action_quit && st->stop)
        *st->stop = true;
}

static bool token_starts_expression(const char *arg) {
    if (!arg || arg[0] == '\0')
        return false;
    return arg[0] == '-' || arg[0] == '!' || arg[0] == '(' || arg[0] == ')';
}

int bx_find_main(int argc, char **argv) {
    const char *progname = argv[0] ? argv[0] : "find";
    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        find_print_help(progname);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        find_print_version(progname);
        return 0;
    }

    struct find_opts opts = {
        .max_depth = -1,
        .min_depth = 0,
    };

    int argi = 1;
    while (argi < argc) {
        if (strcmp(argv[argi], "-L") == 0) {
            opts.follow_symlinks = true;
            opts.follow_root_symlink = true;
            argi++;
        } else if (strcmp(argv[argi], "-H") == 0) {
            opts.follow_symlinks = false;
            opts.follow_root_symlink = true;
            argi++;
        } else if (strcmp(argv[argi], "-P") == 0) {
            opts.follow_symlinks = false;
            opts.follow_root_symlink = false;
            argi++;
        } else {
            break;
        }
    }

    int expr_index = argi;
    while (expr_index < argc && !token_starts_expression(argv[expr_index]))
        expr_index++;

    int root_count = expr_index - argi;
    char **roots = argv + argi;
    if (root_count == 0) {
        static char *default_root[] = { "." };
        roots = default_root;
        root_count = 1;
    }

    bool explicit_action = false;
    for (int i = expr_index; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-print") == 0) {
            opts.action_print = true;
            explicit_action = true;
        } else if (strcmp(arg, "-print0") == 0) {
            opts.action_print0 = true;
            explicit_action = true;
        } else if (strcmp(arg, "-quit") == 0) {
            opts.action_quit = true;
            explicit_action = true;
        } else if (strcmp(arg, "-delete") == 0) {
            opts.action_delete = true;
            opts.depth_first = true;
            explicit_action = true;
        } else if (strcmp(arg, "-depth") == 0) {
            opts.depth_first = true;
        } else if (strcmp(arg, "-L") == 0) {
            opts.follow_symlinks = true;
            opts.follow_root_symlink = true;
        } else if (strcmp(arg, "-H") == 0) {
            opts.follow_symlinks = false;
            opts.follow_root_symlink = true;
        } else if (strcmp(arg, "-P") == 0) {
            opts.follow_symlinks = false;
            opts.follow_root_symlink = false;
        } else if (strcmp(arg, "-name") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "%s: missing argument to `-name'\n", progname);
                return 1;
            }
            opts.has_name = true;
            opts.name_ignore_case = false;
            opts.name_pattern = argv[i];
        } else if (strcmp(arg, "-iname") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "%s: missing argument to `-iname'\n", progname);
                return 1;
            }
            opts.has_name = true;
            opts.name_ignore_case = true;
            opts.name_pattern = argv[i];
        } else if (strcmp(arg, "-path") == 0 || strcmp(arg, "-wholename") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "%s: missing argument to `%s'\n", progname, arg);
                return 1;
            }
            opts.has_path = true;
            opts.path_ignore_case = false;
            opts.path_pattern = argv[i];
        } else if (strcmp(arg, "-iwholename") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "%s: missing argument to `-iwholename'\n", progname);
                return 1;
            }
            opts.has_path = true;
            opts.path_ignore_case = true;
            opts.path_pattern = argv[i];
        } else if (strcmp(arg, "-type") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "%s: missing argument to `-type'\n", progname);
                return 1;
            }
            if (argv[i][0] == '\0' || argv[i][1] != '\0' ||
                (argv[i][0] != 'f' && argv[i][0] != 'd' && argv[i][0] != 'l')) {
                fprintf(stderr, "%s: unknown argument to -type: %s\n", progname, argv[i]);
                return 1;
            }
            opts.has_type = true;
            opts.type_filter = argv[i][0];
        } else if (strcmp(arg, "-empty") == 0) {
            opts.test_empty = true;
        } else if (strcmp(arg, "-readable") == 0) {
            opts.test_readable = true;
        } else if (strcmp(arg, "-writable") == 0) {
            opts.test_writable = true;
        } else if (strcmp(arg, "-executable") == 0) {
            opts.test_executable = true;
        } else if (strcmp(arg, "-true") == 0) {
            opts.constant_predicate_set = true;
            opts.constant_predicate_value = true;
        } else if (strcmp(arg, "-false") == 0) {
            opts.constant_predicate_set = true;
            opts.constant_predicate_value = false;
        } else if (strcmp(arg, "-maxdepth") == 0) {
            if (++i >= argc || !parse_int_arg(progname, "-maxdepth", argv[i], &opts.max_depth))
                return 1;
        } else if (strcmp(arg, "-mindepth") == 0) {
            if (++i >= argc || !parse_int_arg(progname, "-mindepth", argv[i], &opts.min_depth))
                return 1;
        } else {
            fprintf(stderr, "%s: unknown predicate `%s'\n", progname, arg);
            return 1;
        }
    }

    if (!explicit_action)
        opts.action_print = true;

    bool stop = false;
    struct find_state st = {
        .progname = progname,
        .opts = &opts,
        .stop = &stop,
        .status = 0,
    };

    struct walk_opts wopts = {
        .hidden = true,
        .no_ignore = true,
        .follow_symlinks = opts.follow_symlinks,
        .follow_root_symlink = opts.follow_root_symlink,
        .post_order = opts.depth_first,
        .stop = &stop,
        .suppress_eacces = false,
        .os_error_style = false,
        .error_prefix = progname,
        .max_depth = opts.max_depth,
    };

    for (int i = 0; i < root_count && !stop; i++) {
        if (walk_dir(roots[i], &wopts, find_walk_cb, &st) != 0)
            st.status = 1;
    }

    return st.status;
}
