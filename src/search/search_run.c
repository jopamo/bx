#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dev_counters.h"
#include "fswalk/walk.h"
#include "rg_parallel.h"
#include "rg_sched.h"
#include "search_internal.h"
#include "search_plan.h"
#include "search_run.h"
#include "runtime_snapshot.h"
#include "sort.h"
#include "traverse.h"

struct grep_walk_state {
    enum bx_search_personality personality;
    struct bx_matcher *matcher;
    const struct bx_search_exec_plan *exec_plan;
    struct search_opts *opts;
    const char *progname;
    int *match_count;
    struct bx_search_scanner *scanner;
    struct bx_record_stream *record_stream;
    struct bx_search_stats *stats;
    int *exit_status;
    bool *match_seen;
    bool *error_seen;
    bool *stop;
    bool strip_dot_prefix;
};

struct files_walk_state {
    struct search_opts *opts;
    const char *progname;
    bool *error_seen;
    bool strip_dot_prefix;
};

static bool bx_search_run_personality_is_rg(enum bx_search_personality personality) {
    return personality == BX_SEARCH_RG;
}

static int bx_search_operand_ref_compare(const void *left, const void *right) {
    const struct bx_search_operand_ref *a = left;
    const struct bx_search_operand_ref *b = right;
    int cmp = strcmp(a->path, b->path);
    if (cmp != 0)
        return cmp;
    return (a->index > b->index) - (a->index < b->index);
}

static struct bx_search_operand_ref *bx_search_collect_sorted_operands(
    int argc, char **argv, int first_file, int *out_count
) {
    int count = argc - first_file;
    if (out_count)
        *out_count = count;
    if (count <= 0)
        return NULL;

    struct bx_search_operand_ref *refs = calloc((size_t)count, sizeof(*refs));
    if (!refs)
        return NULL;

    for (int i = 0; i < count; i++) {
        refs[i].path = argv[first_file + i];
        refs[i].index = first_file + i;
    }
    qsort(refs, (size_t)count, sizeof(*refs), bx_search_operand_ref_compare);
    return refs;
}

static enum bx_walk_action bx_search_files_walk_cb(struct bx_walk_entry *entry, void *user) {
    struct files_walk_state *state = user;

    if (!entry->is_dir &&
        bx_search_entry_should_skip_recursive_special_input(entry, state ? state->opts : NULL))
        return BX_WALK_CONTINUE;
    if (bx_search_entry_exceeds_max_filesize(entry, state ? state->opts : NULL))
        return BX_WALK_CONTINUE;
    if (!entry->is_dir) {
        char *display = bx_search_display_path_for_output(entry->path,
                                                          state && state->strip_dot_prefix,
                                                          state ? state->opts : NULL);
        bx_search_print_path_record(display ? display : entry->path,
                                    state ? state->opts : NULL);
        free(display);
    }
    return BX_WALK_CONTINUE;
}

static enum bx_walk_action bx_search_grep_walk_error_cb(const char *path, int errnum, void *user) {
    struct grep_walk_state *state = user;

    bx_search_report_path_error(state->progname, path, errnum, state->opts);
    *state->exit_status = 2;
    if (state->error_seen)
        *state->error_seen = true;
    return BX_WALK_CONTINUE;
}

static enum bx_walk_action bx_search_files_walk_error_cb(const char *path, int errnum, void *user) {
    struct files_walk_state *state = user;

    bx_search_report_path_error(state->progname, path, errnum, state->opts);
    if (state->error_seen)
        *state->error_seen = true;
    return BX_WALK_CONTINUE;
}

static enum bx_walk_action bx_search_grep_walk_cb(struct bx_walk_entry *entry, void *user) {
    struct grep_walk_state *state = user;

    if (state->stop && *state->stop)
        return BX_WALK_STOP;
    if (entry->is_dir)
        return BX_WALK_CONTINUE;
    if (bx_search_entry_can_skip_max_filesize_zero_literal(entry, state->exec_plan,
                                                           state->opts))
        return BX_WALK_CONTINUE;
    if (bx_search_entry_should_skip_recursive_special_input(entry, state ? state->opts : NULL))
        return BX_WALK_CONTINUE;
    if (bx_search_entry_exceeds_max_filesize(entry, state ? state->opts : NULL))
        return BX_WALK_CONTINUE;

    int rc = bx_search_search_walk_entry(entry, NULL, state->strip_dot_prefix,
                                         state->progname, state->matcher,
                                         state->exec_plan, state->opts,
                                         state->match_count, state->scanner,
                                         state->record_stream, state->stats);
    if (rc == 2) {
        *state->exit_status = 2;
        if (state->error_seen)
            *state->error_seen = true;
        if (bx_search_matcher_had_error(state->matcher)) {
            if (state->stop)
                *state->stop = true;
            return BX_WALK_STOP;
        }
        return BX_WALK_CONTINUE;
    }
    if (bx_search_status_counts_as_selected(state->personality, state->opts, rc)) {
        *state->exit_status = 0;
        if (state->match_seen)
            *state->match_seen = true;
        if (state->opts->quiet && state->stop)
            return BX_WALK_STOP;
    }
    return BX_WALK_CONTINUE;
}

static char *bx_search_run_build_search_pattern(const char *pattern,
                                                enum bx_search_personality personality,
                                                const struct search_opts *opts) {
    if (!pattern || !opts || opts->extra_patterns.len == 0u)
        return pattern ? strdup(pattern) : NULL;
    if (opts->fixed_strings)
        return strdup(pattern);

    bool use_basic_grouping = !bx_search_run_personality_is_rg(personality) &&
                              !opts->perl_regexp &&
                              !opts->extended_regex &&
                              !opts->fixed_strings;
    const char *group_open = use_basic_grouping ? "\\(" : "(";
    const char *group_close = use_basic_grouping ? "\\)" : ")";
    const char *group_sep = use_basic_grouping ? "\\|" : "|";
    size_t total = strlen(pattern) + strlen(group_open) + strlen(group_close) + 1u;

    for (size_t k = 0u; k < opts->extra_patterns.len; k++) {
        size_t extra = strlen(opts->extra_patterns.items[k]);
        size_t sep = strlen(group_sep);

        if (extra > SIZE_MAX - sep || total > SIZE_MAX - (extra + sep))
            return NULL;
        total += extra + sep;
    }

    char *combined = malloc(total);
    if (!combined)
        return NULL;

    char *p = combined;
    memcpy(p, group_open, strlen(group_open));
    p += strlen(group_open);
    memcpy(p, pattern, strlen(pattern));
    p += strlen(pattern);
    for (size_t k = 0u; k < opts->extra_patterns.len; k++) {
        const char *extra_pattern = opts->extra_patterns.items[k];
        size_t extra_len = strlen(extra_pattern);

        memcpy(p, group_sep, strlen(group_sep));
        p += strlen(group_sep);
        memcpy(p, extra_pattern, extra_len);
        p += extra_len;
    }
    memcpy(p, group_close, strlen(group_close));
    p += strlen(group_close);
    *p = '\0';
    return combined;
}

static bool bx_search_run_default_show_filename(int argc,
                                                char **argv,
                                                int first_file,
                                                enum bx_search_personality personality,
                                                struct search_opts *opts,
                                                bool rg_searches_stdin) {
    int num_files = argc - first_file;
    if (num_files == 0) {
        if (bx_search_run_personality_is_rg(personality))
            return !rg_searches_stdin;
        return opts->recursive;
    }
    if (num_files > 1)
        return true;
    if (!argv[first_file] || strcmp(argv[first_file], "-") == 0)
        return false;

    struct stat st;
    bx_search_dev_counters_note_walk_stat_call(BX_SEARCH_WALK_STAT_REASON_EXPLICIT_OPERAND);
    if (stat(argv[first_file], &st) == 0)
        return S_ISDIR(st.st_mode);
    return false;
}

static bool bx_search_run_default_heading(enum bx_search_personality personality,
                                          const struct search_opts *opts) {
    if (!bx_search_run_personality_is_rg(personality) || !isatty(STDOUT_FILENO))
        return false;
    return opts->show_filename;
}

bool bx_search_run_should_search_stdin(void) {
    if (isatty(STDIN_FILENO))
        return false;

    struct stat st;
    bx_search_dev_counters_note_content_fstat_call();
    if (fstat(STDIN_FILENO, &st) != 0)
        return false;

    if (S_ISCHR(st.st_mode))
        return false;

    if (S_ISREG(st.st_mode) || S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode))
        return true;

    struct pollfd pfd = {
        .fd = STDIN_FILENO,
        .events = POLLIN,
    };
    int rc = poll(&pfd, 1, 0);

    if (rc <= 0)
        return false;
    return (pfd.revents & POLLIN) != 0;
}

static void bx_search_run_apply_output_defaults(const struct bx_search_run_args *args) {
    struct search_opts *opts = args->opts;

    if (!opts->show_filename && !opts->hide_filename) {
        opts->show_filename = bx_search_run_default_show_filename(args->argc, args->argv,
                                                                  args->first_file,
                                                                  args->personality, opts,
                                                                  args->plan->rg_searches_stdin);
    }
    if (opts->hide_filename)
        opts->show_filename = false;
    if (bx_search_run_personality_is_rg(args->personality)
        && !opts->show_line_number && isatty(STDOUT_FILENO)) {
        opts->show_line_number = true;
    }
    if (bx_search_run_personality_is_rg(args->personality) && opts->vimgrep) {
        opts->show_filename = true;
        opts->hide_filename = false;
        opts->show_line_number = true;
        opts->show_column = true;
        opts->only_matching = false;
        opts->heading = false;
        opts->heading_set = true;
    }
    if (!opts->heading_set)
        opts->heading = bx_search_run_default_heading(args->personality, opts);
}

static int bx_search_run_status_from_flags(const struct search_opts *opts,
                                           bool match_seen,
                                           bool error_seen) {
    if (opts && opts->quiet && match_seen)
        return 0;
    if (error_seen)
        return 2;
    return match_seen ? 0 : 1;
}

bool bx_search_status_counts_as_selected(enum bx_search_personality personality,
                                         const struct search_opts *opts,
                                         int status) {
    if (status == 2)
        return false;
    if (personality == BX_SEARCH_RG &&
        opts && opts->files_without_match && !opts->files_with_matches)
        return status == 1;
    return status == 0;
}

static int bx_search_run_files_only(const struct bx_search_run_args *args) {
    int num_files = args->argc - args->first_file;
    bool error_seen = false;

    if (args->plan->orchestrator == BX_SEARCH_PLAN_ORCHESTRATOR_METADATA_SORTED) {
        struct bx_search_sorted_paths sorted_paths = {0};

        if (bx_search_collect_metadata_sorted_paths(args->argc, args->argv, args->first_file,
                                                    args->progname, args->personality,
                                                    args->opts, args->runtime_snapshot,
                                                    &sorted_paths,
                                                    &error_seen) != 0) {
            bx_search_sorted_paths_dispose(&sorted_paths);
            return 2;
        }
        for (size_t i = 0; i < sorted_paths.len; ++i) {
            char *display = bx_search_display_path_for_output(sorted_paths.items[i].path,
                                                              sorted_paths.items[i].strip_dot_prefix,
                                                              args->opts);
            bx_search_print_path_record(display ? display : sorted_paths.items[i].path,
                                        args->opts);
            free(display);
        }
        bx_search_sorted_paths_dispose(&sorted_paths);
        return error_seen ? 2 : 0;
    }

    struct files_walk_state state = {
        .opts = args->opts,
        .progname = args->progname,
        .error_seen = &error_seen,
    };
    struct bx_walk_opts walk_opts =
        bx_search_runtime_snapshot_walk_opts(args->runtime_snapshot, NULL);
    struct bx_search_walk_config walk_config = {
        .walk_opts = &walk_opts,
        .filter_opts = bx_search_runtime_snapshot_filter_opts(args->runtime_snapshot),
        .ignore_opts = bx_search_runtime_snapshot_ignore_opts(args->runtime_snapshot),
        .visit = bx_search_files_walk_cb,
        .error = bx_search_files_walk_error_cb,
    };
    int sorted_operand_count = 0;
    struct bx_search_operand_ref *sorted_operands =
        bx_search_sort_is_path(args->opts)
            ? bx_search_collect_sorted_operands(args->argc, args->argv, args->first_file,
                                                &sorted_operand_count)
            : NULL;

    if (num_files == 0) {
        state.strip_dot_prefix = true;
        if (bx_search_walk(".", &walk_config, &state) != 0)
            error_seen = true;
    } else {
        for (int operand_i = 0; operand_i < num_files; operand_i++) {
            int j = sorted_operands
                        ? sorted_operands[bx_search_sort_is_descending(args->opts)
                                              ? (sorted_operand_count - 1 - operand_i)
                                              : operand_i]
                              .index
                        : (args->first_file + operand_i);
            struct stat st;

            bx_search_dev_counters_note_walk_stat_call(BX_SEARCH_WALK_STAT_REASON_EXPLICIT_OPERAND);
            if (stat(args->argv[j], &st) != 0) {
                bx_search_report_path_error(args->progname, args->argv[j], errno, args->opts);
                error_seen = true;
                continue;
            }
            if (bx_search_loaded_metadata_exceeds_max_filesize(&st, args->opts))
                continue;
            if (S_ISDIR(st.st_mode))
                error_seen |= bx_search_walk(args->argv[j], &walk_config, &state) != 0;
            else
                bx_search_print_path_record(args->argv[j], args->opts);
        }
    }

    free(sorted_operands);
    return error_seen ? 2 : 0;
}

static bool bx_search_run_compile_matcher(const struct bx_search_run_args *args,
                                          const char *search_pattern,
                                          struct bx_matcher **matcher_out) {
    char *compile_error = NULL;
    char *compile_warning = NULL;
    struct bx_matcher *matcher = bx_search_compile_matcher(search_pattern, args->personality,
                                                           args->opts, &compile_error,
                                                           &compile_warning);

    if (compile_warning) {
        const char *cursor = compile_warning;

        while (*cursor != '\0') {
            const char *line_end = strchr(cursor, '\n');
            size_t line_len = line_end ? (size_t)(line_end - cursor) : strlen(cursor);

            if (line_len > 0u)
                fprintf(stderr, "%s: %.*s\n", args->progname, (int)line_len, cursor);
            if (!line_end)
                break;
            cursor = line_end + 1;
        }
        free(compile_warning);
    }

    if (matcher) {
        *matcher_out = matcher;
        return true;
    }

    if (compile_error) {
        if (!bx_search_run_personality_is_rg(args->personality)) {
            const char *message = compile_error;

            if (strncmp(compile_error, "regex parse error at offset ", 28) == 0) {
                const char *detail = strstr(compile_error, ": ");
                if (detail && detail[2] != '\0')
                    message = detail + 2;
            }
            if (strcmp(message, "Missing ']'") == 0) {
                message = "Invalid regular expression";
            }
            else if (strcmp(message, "Invalid character range") == 0) {
                message = "Invalid range end";
            }
            else if (strcmp(message, "Invalid contents of {}") == 0) {
                message = "Invalid content of \\{\\}";
            }
            if (strcmp(compile_error, "the -P option only supports a single pattern") == 0) {
                fprintf(stderr, "%s: %s\n", args->progname, message);
            } else {
                fprintf(stderr, "%s: %s\n", args->progname, message);
            }
        } else {
            fprintf(stderr, "%s: invalid pattern '%s': %s\n",
                    args->progname, args->pattern, compile_error);
        }
        free(compile_error);
    } else {
        fprintf(stderr, "%s: invalid pattern: %s\n", args->progname, args->pattern);
    }
    return false;
}

static void bx_search_run_metadata_sorted(const struct bx_search_run_args *args,
                                          struct bx_matcher *matcher,
                                          const struct bx_search_exec_plan *exec_plan,
                                          struct bx_search_scanner *scanner,
                                          struct bx_record_stream *record_stream,
                                          bool *match_seen,
                                          bool *error_seen) {
    struct bx_search_sorted_paths sorted_paths = {0};
    int global_matches = 0;

    if (bx_search_collect_metadata_sorted_paths(args->argc, args->argv, args->first_file,
                                                args->progname, args->personality,
                                                args->opts, args->runtime_snapshot,
                                                &sorted_paths,
                                                error_seen) != 0) {
        *error_seen = true;
        bx_search_sorted_paths_dispose(&sorted_paths);
        return;
    }

    for (size_t i = 0; i < sorted_paths.len; ++i) {
        int rc = bx_search_search_file(sorted_paths.items[i].path, NULL,
                                       sorted_paths.items[i].strip_dot_prefix,
                                       args->progname, matcher, exec_plan, args->opts,
                                       &global_matches, scanner, record_stream,
                                       args->stats);
        if (rc == 2) {
            *error_seen = true;
            if (bx_search_matcher_had_error(matcher))
                break;
        } else if (bx_search_status_counts_as_selected(args->personality, args->opts, rc)) {
            *match_seen = true;
            if (args->opts->quiet)
                break;
        }
    }

    bx_search_sorted_paths_dispose(&sorted_paths);
}

static void bx_search_run_single_threaded(const struct bx_search_run_args *args,
                                          struct bx_matcher *matcher,
                                          const struct bx_search_exec_plan *exec_plan,
                                          struct bx_search_scanner *scanner,
                                          struct bx_record_stream *record_stream,
                                          struct bx_search_operand_ref *sorted_operands,
                                          int sorted_operand_count,
                                          bool *match_seen,
                                          bool *error_seen) {
    int num_files = args->argc - args->first_file;
    int global_matches = 0;
    int exit_status = 1;

    if (num_files == 0) {
        if ((bx_search_run_personality_is_rg(args->personality) && !args->plan->rg_searches_stdin)
            || (!bx_search_run_personality_is_rg(args->personality) && args->opts->recursive)) {
            bool stop = false;
            struct grep_walk_state state = {
                .personality = args->personality,
                .matcher = matcher,
                .exec_plan = exec_plan,
                .opts = args->opts,
                .progname = args->progname,
                .match_count = &global_matches,
                .scanner = scanner,
                .record_stream = record_stream,
                .stats = args->stats,
                .exit_status = &exit_status,
                .match_seen = match_seen,
                .error_seen = error_seen,
                .stop = &stop,
                .strip_dot_prefix = true,
            };
            struct bx_walk_opts walk_opts =
                bx_search_runtime_snapshot_walk_opts(args->runtime_snapshot, &stop);
            struct bx_search_walk_config walk_config = {
                .walk_opts = &walk_opts,
                .filter_opts = bx_search_runtime_snapshot_filter_opts(args->runtime_snapshot),
                .ignore_opts = bx_search_runtime_snapshot_ignore_opts(args->runtime_snapshot),
                .visit = bx_search_grep_walk_cb,
                .error = bx_search_grep_walk_error_cb,
            };

            if (bx_search_walk(".", &walk_config, &state) != 0) {
                exit_status = 2;
                *error_seen = true;
            }
        } else {
            int rc = bx_search_search_file(NULL, NULL, false, args->progname, matcher, exec_plan,
                                           args->opts,
                                           &global_matches, scanner, record_stream,
                                           args->stats);
            if (bx_search_status_counts_as_selected(args->personality, args->opts, rc))
                *match_seen = true;
            else if (rc == 2)
                *error_seen = true;
        }
        return;
    }

    if (args->opts->recursive) {
        bool stop = false;
        struct grep_walk_state state = {
            .personality = args->personality,
            .matcher = matcher,
            .exec_plan = exec_plan,
            .opts = args->opts,
            .progname = args->progname,
            .match_count = &global_matches,
            .scanner = scanner,
            .record_stream = record_stream,
            .stats = args->stats,
            .exit_status = &exit_status,
            .match_seen = match_seen,
            .error_seen = error_seen,
            .stop = &stop,
            .strip_dot_prefix = false,
        };
        struct bx_walk_opts walk_opts =
            bx_search_runtime_snapshot_walk_opts(args->runtime_snapshot, &stop);
        struct bx_search_walk_config walk_config = {
            .walk_opts = &walk_opts,
            .filter_opts = bx_search_runtime_snapshot_filter_opts(args->runtime_snapshot),
            .ignore_opts = bx_search_runtime_snapshot_ignore_opts(args->runtime_snapshot),
            .visit = bx_search_grep_walk_cb,
            .error = bx_search_grep_walk_error_cb,
        };

        for (int operand_i = 0; operand_i < num_files && !stop; operand_i++) {
            int j = sorted_operands
                        ? sorted_operands[bx_search_sort_is_descending(args->opts)
                                              ? (sorted_operand_count - 1 - operand_i)
                                              : operand_i]
                              .index
                        : (args->first_file + operand_i);
            struct stat st;

            bx_search_dev_counters_note_walk_stat_call(BX_SEARCH_WALK_STAT_REASON_EXPLICIT_OPERAND);
            if (stat(args->argv[j], &st) != 0) {
                bx_search_report_path_error(args->progname, args->argv[j], errno, args->opts);
                exit_status = 2;
                *error_seen = true;
                continue;
            }
            if (S_ISDIR(st.st_mode)) {
                if (bx_search_walk(args->argv[j], &walk_config, &state) != 0) {
                    exit_status = 2;
                    *error_seen = true;
                }
            } else {
                if (bx_search_should_skip_special_input_mode(st.st_mode, args->opts))
                    continue;
                if (bx_search_explicit_entry_selected(args->opts, args->argv[j])) {
                    struct bx_walk_entry entry = {
                        .path = args->argv[j],
                        .follow_metadata = true,
                    };
                    bx_walk_entry_fill_from_stat(&entry, &st);
                    enum bx_walk_action action = bx_search_grep_walk_cb(&entry, &state);
                    if (action == BX_WALK_STOP)
                        stop = true;
                }
            }
        }
        return;
    }

    for (int operand_i = 0; operand_i < num_files; operand_i++) {
        int j = sorted_operands
                    ? sorted_operands[bx_search_sort_is_descending(args->opts)
                                          ? (sorted_operand_count - 1 - operand_i)
                                          : operand_i]
                          .index
                    : (args->first_file + operand_i);

        if (args->argv[j] && strcmp(args->argv[j], "-") != 0) {
            struct stat st;

            bx_search_dev_counters_note_walk_lstat_call(BX_SEARCH_WALK_STAT_REASON_EXPLICIT_OPERAND);
            if (lstat(args->argv[j], &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    if (args->opts->directory_mode == BX_GREP_DIR_SKIP)
                        continue;
                    bx_search_report_path_error(args->progname, args->argv[j],
                                                EISDIR, args->opts);
                    *error_seen = true;
                    continue;
                }
                if (bx_search_should_skip_special_input_mode(st.st_mode, args->opts))
                    continue;
                if (bx_search_mode_can_skip_max_filesize_zero_literal(st.st_mode,
                                                                      exec_plan,
                                                                      args->opts))
                    continue;
            }
            if (bx_search_path_exceeds_max_filesize(args->argv[j], args->opts))
                continue;
        }

        int rc = bx_search_search_file(args->argv[j], NULL, false, args->progname, matcher,
                                       exec_plan,
                                       args->opts, &global_matches, scanner,
                                       record_stream, args->stats);
        if (rc == 2) {
            *error_seen = true;
            if (bx_search_matcher_had_error(matcher))
                break;
        } else if (bx_search_status_counts_as_selected(args->personality, args->opts, rc)) {
            *match_seen = true;
            if (args->opts->quiet)
                break;
        }
    }
}

void bx_search_run(const struct bx_search_run_args *args,
                   struct bx_search_run_result *result) {
    struct bx_search_run_result local_result = {
        .status = 1,
        .ran_search = false,
    };
    struct bx_matcher *matcher = NULL;
    struct bx_search_scanner scanner = {0};
    struct bx_record_stream record_stream = {0};
    struct bx_search_operand_ref *sorted_operands = NULL;
    char *search_pattern = NULL;
    struct bx_search_exec_plan exec_plan = {0};
    int sorted_operand_count = 0;
    bool match_seen = false;
    bool error_seen = false;

    if (!args || !args->opts || !args->plan || !args->runtime_snapshot) {
        if (result)
            *result = local_result;
        return;
    }

    bx_search_run_apply_output_defaults(args);

    if (args->plan->output_kind == BX_SEARCH_PLAN_OUTPUT_FILES_ONLY) {
        local_result.status = bx_search_run_files_only(args);
        local_result.ran_search = true;
        goto done;
    }

    search_pattern = bx_search_run_build_search_pattern(args->pattern, args->personality,
                                                        args->opts);
    if (!search_pattern) {
        fprintf(stderr, "%s: out of memory\n", args->progname);
        local_result.status = 2;
        goto done;
    }
    if (!bx_search_run_compile_matcher(args, search_pattern, &matcher)) {
        local_result.status = 2;
        goto done;
    }
    bx_search_exec_plan_build(&exec_plan, args->plan, matcher, args->opts);

    sorted_operands = bx_search_sort_is_path(args->opts)
        ? bx_search_collect_sorted_operands(args->argc, args->argv, args->first_file,
                                            &sorted_operand_count)
        : NULL;

    if (args->plan->orchestrator == BX_SEARCH_PLAN_ORCHESTRATOR_METADATA_SORTED) {
        bx_search_run_metadata_sorted(args, matcher, &exec_plan, &scanner, &record_stream,
                                      &match_seen, &error_seen);
        local_result.ran_search = true;
        local_result.status = bx_search_run_status_from_flags(args->opts,
                                                              match_seen, error_seen);
        goto done;
    }

    if (args->plan->orchestrator == BX_SEARCH_PLAN_ORCHESTRATOR_PARALLEL_SUBTREE) {
        int parallel_status = bx_rg_sched_run(args->argc, args->argv,
                                              args->first_file, sorted_operands,
                                              sorted_operand_count, args->progname,
                                              search_pattern, args->personality,
                                              &exec_plan, args->opts,
                                              args->runtime_snapshot,
                                              bx_search_rg_thread_count(args->opts),
                                              args->stats,
                                              &match_seen, &error_seen);
        local_result.ran_search = true;
        local_result.status = parallel_status;
        if (!error_seen && !match_seen)
            local_result.status = bx_search_run_status_from_flags(args->opts,
                                                                  match_seen, error_seen);
        goto done;
    }

    if (args->plan->orchestrator == BX_SEARCH_PLAN_ORCHESTRATOR_PARALLEL_GENERIC) {
        int parallel_status = bx_search_run_parallel_rg(args->argc, args->argv,
                                                        args->first_file, sorted_operands,
                                                        sorted_operand_count, args->progname,
                                                        search_pattern, args->personality,
                                                        &exec_plan, args->opts,
                                                        args->runtime_snapshot,
                                                        args->stats,
                                                        &match_seen, &error_seen);
        local_result.ran_search = true;
        local_result.status = parallel_status;
        if (!error_seen && !match_seen)
            local_result.status = bx_search_run_status_from_flags(args->opts,
                                                                  match_seen, error_seen);
        goto done;
    }

    bx_search_run_single_threaded(args, matcher, &exec_plan, &scanner, &record_stream,
                                  sorted_operands, sorted_operand_count,
                                  &match_seen, &error_seen);
    local_result.ran_search = true;
    local_result.status = bx_search_run_status_from_flags(args->opts,
                                                          match_seen, error_seen);

done:
    if (local_result.ran_search) {
        int output_err = bx_search_check_output_error();
        if (output_err != 0) {
            bx_search_report_write_error(args->progname, output_err);
            local_result.status = 2;
        }
    }
    bx_search_matcher_free(matcher);
    bx_search_scanner_dispose(&scanner);
    bx_record_stream_dispose(&record_stream);
    free(search_pattern);
    free(sorted_operands);
    if (result)
        *result = local_result;
}
