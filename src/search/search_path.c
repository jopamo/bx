#define _GNU_SOURCE
#include <dirent.h>
#include <fnmatch.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "filter.h"
#include "dev_counters.h"
#include "fswalk/walk.h"
#include "ignore.h"
#include "lib/cli_common.h"
#include "lib/path_ops.h"
#include "options.h"
#include "rg_output.h"
#include "search_internal.h"
#include "search_plan.h"
#include "sort.h"

static bool progname_uses_os_error_style(const char *progname) {
    if (!progname)
        return false;
    progname = bx_cli_progname(progname, "grep");
    return strcmp(progname, "rg") == 0;
}

bool bx_search_progname_uses_os_error_style(const char *progname) {
    return progname_uses_os_error_style(progname);
}

int bx_search_fprintf_path_error(FILE *stream,
                                 const char *progname,
                                 const char *path,
                                 int errnum) {
    const char *display_progname = progname ? progname : "grep";
    const char *display_path = path ? path : "";

    if (!stream)
        return -1;

    if (progname_uses_os_error_style(progname))
        return fprintf(stream, "%s: %s: %s (os error %d)\n",
                       display_progname, display_path, strerror(errnum), errnum);

    return fprintf(stream, "%s: %s: %s\n",
                   display_progname, display_path, strerror(errnum));
}

int bx_search_fprintf_path_io_error(FILE *stream,
                                    const char *progname,
                                    const char *path,
                                    int errnum) {
    const char *display_progname = progname ? progname : "grep";
    const char *display_path = path ? path : "";

    if (!stream)
        return -1;

    if (!progname_uses_os_error_style(progname))
        return bx_search_fprintf_path_error(stream, progname, path, errnum);

    /*
     * ripgrep uses this expanded wording for a single searched path operand
     * that cannot be opened. Keep this byte-exact for the raw default-literal
     * path, where open failures are reported before scanner/output work is
     * allowed.
     */
    return fprintf(stream,
                   "%s: %s: IO error for operation on %s: %s (os error %d)\n",
                   display_progname, display_path, display_path,
                   strerror(errnum), errnum);
}

int bx_search_snprintf_path_error(char *buf,
                                  size_t cap,
                                  const char *progname,
                                  const char *path,
                                  int errnum) {
    const char *display_progname = progname ? progname : "grep";
    const char *display_path = path ? path : "";

    if (progname_uses_os_error_style(progname))
        return snprintf(buf, cap, "%s: %s: %s (os error %d)\n",
                        display_progname, display_path, strerror(errnum), errnum);

    return snprintf(buf, cap, "%s: %s: %s\n",
                    display_progname, display_path, strerror(errnum));
}

static bool bx_search_loaded_mode_size_exceeds_max_filesize(
    mode_t mode,
    off_t size,
    const struct search_opts *opts
) {
    if (!opts || !opts->max_filesize_set)
        return false;
    return S_ISREG(mode) && size > (off_t)opts->max_filesize;
}

bool bx_search_path_exceeds_max_filesize(const char *path,
                                         const struct search_opts *opts) {
    if (!path || !opts || !opts->max_filesize_set)
        return false;

    struct stat st;
    bx_search_dev_counters_note_walk_stat_call(BX_SEARCH_WALK_STAT_REASON_MAX_FILESIZE);
    if (stat(path, &st) != 0)
        return false;
    return bx_search_loaded_metadata_exceeds_max_filesize(&st, opts);
}

bool bx_search_loaded_metadata_exceeds_max_filesize(const struct stat *st,
                                                    const struct search_opts *opts) {
    if (!st)
        return false;
    return bx_search_loaded_mode_size_exceeds_max_filesize(st->st_mode, st->st_size, opts);
}

bool bx_search_entry_exceeds_max_filesize(struct bx_walk_entry *entry,
                                          const struct search_opts *opts) {
    if (!entry || !opts || !opts->max_filesize_set || entry->is_dir)
        return false;
    if (!entry->metadata_loaded &&
        !bx_walk_entry_load_metadata_for(entry, BX_WALK_METADATA_REASON_MAX_FILESIZE))
        return false;
    return entry->metadata_loaded
        && bx_search_loaded_mode_size_exceeds_max_filesize(entry->mode, entry->size, opts);
}

bool bx_search_entry_can_skip_max_filesize_zero_literal(
    const struct bx_walk_entry *entry,
    const struct bx_search_exec_plan *exec_plan,
    const struct search_opts *opts
) {
    if (!entry || !exec_plan || !opts)
        return false;
    if (exec_plan->max_filesize_zero_policy
        != BX_SEARCH_MAX_FILESIZE_ZERO_SKIP_NON_EMPTY_LITERAL_REGULARS)
        return false;
    if (!opts->max_filesize_set || opts->max_filesize != 0u)
        return false;
    if (entry->is_dir)
        return false;

    /*
     * With --max-filesize 0, the only regular files that pass the size
     * filter are empty files. The exec-plan flag is set only for non-empty
     * literal absence plans and output modes where "no match in an empty
     * regular file" has no observable per-file output. That lets traversal
     * reject d_type-known regular files without an exact size stat.
     */
    if (entry->metadata_loaded)
        return bx_search_mode_can_skip_max_filesize_zero_literal(entry->mode, exec_plan, opts);
    return entry->d_type_known && entry->d_type == DT_REG;
}

bool bx_search_mode_can_skip_max_filesize_zero_literal(
    mode_t mode,
    const struct bx_search_exec_plan *exec_plan,
    const struct search_opts *opts
) {
    if (!exec_plan || !opts)
        return false;
    if (exec_plan->max_filesize_zero_policy
        != BX_SEARCH_MAX_FILESIZE_ZERO_SKIP_NON_EMPTY_LITERAL_REGULARS)
        return false;
    if (!opts->max_filesize_set || opts->max_filesize != 0u)
        return false;
    return S_ISREG(mode);
}

static bool bx_search_personality_is_rg(enum bx_search_personality personality) {
    return personality == BX_SEARCH_RG;
}

static void bx_search_walk_counter_bridge(enum bx_walk_counter counter,
                                          uint64_t count,
                                          void *user) {
    (void)user;

    switch (counter) {
    case BX_WALK_COUNTER_DIRENTS_SEEN:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_DIRENTS_SEEN, count);
        return;
    case BX_WALK_COUNTER_GETDENTS64_CALLS:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_GETDENTS64_CALLS, count);
        return;
    case BX_WALK_COUNTER_GETDENTS64_BYTES:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_GETDENTS64_BYTES, count);
        return;
    case BX_WALK_COUNTER_DIRS_SEEN:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_DIRS_SEEN, count);
        return;
    case BX_WALK_COUNTER_FILES_SEEN:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_FILES_SEEN, count);
        return;
    case BX_WALK_COUNTER_SYMLINKS_SEEN:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_SYMLINKS_SEEN, count);
        return;
    case BX_WALK_COUNTER_UNKNOWN_DTYPE_SEEN:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_UNKNOWN_DTYPE_SEEN, count);
        return;
    case BX_WALK_COUNTER_STAT_CALLS:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_STAT_CALLS, count);
        return;
    case BX_WALK_COUNTER_FSTAT_CALLS:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_FSTAT_CALLS, count);
        return;
    case BX_WALK_COUNTER_LSTAT_CALLS:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_LSTAT_CALLS, count);
        return;
    case BX_WALK_COUNTER_FSTATAT_CALLS:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_FSTATAT_CALLS, count);
        return;
    case BX_WALK_COUNTER_STAT_REASON_UNKNOWN_DTYPE:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_STAT_REASON_UNKNOWN_DTYPE, count);
        return;
    case BX_WALK_COUNTER_STAT_REASON_SYMLINK_POLICY:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_STAT_REASON_SYMLINK_POLICY, count);
        return;
    case BX_WALK_COUNTER_STAT_REASON_TRAVERSAL_POLICY:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_STAT_REASON_TRAVERSAL_POLICY, count);
        return;
    case BX_WALK_COUNTER_STAT_REASON_METADATA_FILTER:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_STAT_REASON_METADATA_FILTER, count);
        return;
    case BX_WALK_COUNTER_STAT_REASON_MAX_FILESIZE:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_STAT_REASON_MAX_FILESIZE, count);
        return;
    case BX_WALK_COUNTER_STAT_REASON_MIN_FILESIZE:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_STAT_REASON_MIN_FILESIZE, count);
        return;
    case BX_WALK_COUNTER_STAT_REASON_TYPE:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_STAT_REASON_TYPE, count);
        return;
    case BX_WALK_COUNTER_STAT_REASON_SORT:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_STAT_REASON_SORT, count);
        return;
    case BX_WALK_COUNTER_STAT_REASON_METADATA_OUTPUT:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_STAT_REASON_METADATA_OUTPUT, count);
        return;
    case BX_WALK_COUNTER_STAT_REASON_EXPLICIT_OPERAND:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_STAT_REASON_EXPLICIT_OPERAND, count);
        return;
    case BX_WALK_COUNTER_OPENAT_CALLS:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_OPENAT_CALLS, count);
        return;
    case BX_WALK_COUNTER_PATH_JOIN_CALLS:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_PATH_JOIN_CALLS, count);
        return;
    case BX_WALK_COUNTER_PATH_PUSH_CALLS:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_PATH_PUSH_CALLS, count);
        return;
    case BX_WALK_COUNTER_PATH_PUSH_NS:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_PATH_PUSH_NS, count);
        return;
    case BX_WALK_COUNTER_PATH_POP_CALLS:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_PATH_POP_CALLS, count);
        return;
    case BX_WALK_COUNTER_PATH_POP_NS:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_PATH_POP_NS, count);
        return;
    case BX_WALK_COUNTER_PATH_ALLOCS:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_PATH_ALLOCS, count);
        return;
    case BX_WALK_COUNTER_PATH_COPIES_BEFORE_MATCH:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_PATH_COPIES_BEFORE_MATCH, count);
        return;
    case BX_WALK_COUNTER_DIR_BUCKET_TINY_DIRS:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_DIR_BUCKET_TINY_DIRS, count);
        return;
    case BX_WALK_COUNTER_DIR_BUCKET_TINY_ENTRIES:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_DIR_BUCKET_TINY_ENTRIES, count);
        return;
    case BX_WALK_COUNTER_DIR_BUCKET_TINY_NS:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_DIR_BUCKET_TINY_NS, count);
        return;
    case BX_WALK_COUNTER_DIR_BUCKET_SMALL_DIRS:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_DIR_BUCKET_SMALL_DIRS, count);
        return;
    case BX_WALK_COUNTER_DIR_BUCKET_SMALL_ENTRIES:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_DIR_BUCKET_SMALL_ENTRIES, count);
        return;
    case BX_WALK_COUNTER_DIR_BUCKET_SMALL_NS:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_DIR_BUCKET_SMALL_NS, count);
        return;
    case BX_WALK_COUNTER_DIR_BUCKET_MEDIUM_DIRS:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_DIR_BUCKET_MEDIUM_DIRS, count);
        return;
    case BX_WALK_COUNTER_DIR_BUCKET_MEDIUM_ENTRIES:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_DIR_BUCKET_MEDIUM_ENTRIES, count);
        return;
    case BX_WALK_COUNTER_DIR_BUCKET_MEDIUM_NS:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_DIR_BUCKET_MEDIUM_NS, count);
        return;
    case BX_WALK_COUNTER_DIR_BUCKET_HUGE_DIRS:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_DIR_BUCKET_HUGE_DIRS, count);
        return;
    case BX_WALK_COUNTER_DIR_BUCKET_HUGE_ENTRIES:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_DIR_BUCKET_HUGE_ENTRIES, count);
        return;
    case BX_WALK_COUNTER_DIR_BUCKET_HUGE_NS:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_DIR_BUCKET_HUGE_NS, count);
        return;
    }
}

static const struct bx_walk_counter_ops bx_search_walk_counter_ops = {
    .note = bx_search_walk_counter_bridge,
    .user = NULL,
};

static bool bx_search_use_rg_sort_policy(enum bx_search_personality personality,
                                         const struct search_opts *opts) {
    return !bx_search_personality_is_rg(personality)
        || bx_search_sort_is_path(opts);
}

static int bx_search_cycle_mode(enum bx_search_personality personality,
                                const struct search_opts *opts) {
    if (!opts->follow_symlinks)
        return BX_WALK_CYCLE_NONE;
    return bx_search_personality_is_rg(personality)
        ? BX_WALK_CYCLE_SYMLINK_REPEAT
        : BX_WALK_CYCLE_DIR_REPEAT;
}

static int bx_search_cycle_report(enum bx_search_personality personality,
                                  const struct search_opts *opts) {
    if (!opts->follow_symlinks)
        return BX_WALK_CYCLE_IGNORE;
    return bx_search_personality_is_rg(personality)
        ? BX_WALK_CYCLE_ERROR
        : BX_WALK_CYCLE_WARN;
}

void bx_search_report_path_error(const char *progname,
                                 const char *path,
                                 int errnum,
                                 const struct search_opts *opts) {
    if (opts && opts->suppress_errors)
        return;

    bx_search_fprintf_path_error(bx_search_error_output_stream(), progname, path, errnum);
}

void bx_search_report_write_error(const char *progname, int errnum) {
    if (progname_uses_os_error_style(progname))
        fprintf(bx_search_error_output_stream(), "%s: write error: %s (os error %d)\n",
                progname, strerror(errnum), errnum);
    else
        fprintf(bx_search_error_output_stream(), "%s: write error: %s\n",
                progname, strerror(errnum));
}

void bx_search_report_record_too_large(const char *progname,
                                       const char *path,
                                       const struct search_opts *opts) {
    if (opts && opts->suppress_errors)
        return;

    fprintf(bx_search_error_output_stream(), "%s: %s: input record too large\n",
            progname, path);
}

static void report_binary_match(const char *progname, const char *path) {
    fprintf(bx_search_error_output_stream(), "%s: %s: binary file matches\n",
            progname, path);
}

void bx_search_report_binary_match(const char *progname, const char *path) {
    report_binary_match(progname, path);
}

static bool bx_search_mode_is_special_input(mode_t mode) {
    return S_ISCHR(mode) || S_ISBLK(mode) || S_ISFIFO(mode) || S_ISSOCK(mode);
}

static bool bx_search_mode_is_explicit_skip_special_input(mode_t mode) {
    return S_ISCHR(mode) || S_ISBLK(mode) || S_ISFIFO(mode);
}

bool bx_search_should_skip_special_input_mode(mode_t mode,
                                              const struct search_opts *opts) {
    return opts && opts->device_mode == BX_GREP_DEVICE_SKIP
        && bx_search_mode_is_explicit_skip_special_input(mode);
}

bool bx_search_entry_should_skip_special_input(struct bx_walk_entry *entry,
                                               const struct search_opts *opts) {
    if (!entry || !opts || opts->device_mode != BX_GREP_DEVICE_SKIP)
        return false;

    if (entry->d_type_known) {
        return entry->d_type == DT_CHR || entry->d_type == DT_BLK ||
               entry->d_type == DT_FIFO || entry->d_type == DT_SOCK;
    }

    if (!entry->metadata_loaded &&
        !bx_walk_entry_load_metadata_for(entry, BX_WALK_METADATA_REASON_TYPE))
        return false;

    return bx_search_mode_is_special_input(entry->mode);
}

static bool bx_search_entry_is_unfollowed_symlink(struct bx_walk_entry *entry,
                                                  const struct search_opts *opts) {
    if (!entry || !opts || opts->follow_symlinks)
        return false;
    if (entry->is_symlink)
        return true;
    if (entry->metadata_tried || entry->metadata_loaded)
        return entry->metadata_loaded && S_ISLNK(entry->mode);
    return false;
}

bool bx_search_entry_should_skip_recursive_special_input(struct bx_walk_entry *entry,
                                                         const struct search_opts *opts) {
    if (!entry || !opts)
        return false;

    if (bx_search_entry_is_unfollowed_symlink(entry, opts))
        return true;

    if (entry->d_type_known) {
        bool is_special = entry->d_type == DT_CHR || entry->d_type == DT_BLK ||
                          entry->d_type == DT_FIFO || entry->d_type == DT_SOCK;
        if (!is_special)
            return false;
        return opts->device_mode == BX_GREP_DEVICE_SKIP || !opts->device_mode_explicit;
    }

    if (!entry->metadata_loaded &&
        !bx_walk_entry_load_metadata_for(entry, BX_WALK_METADATA_REASON_TYPE))
        return false;

    if (!bx_search_mode_is_special_input(entry->mode))
        return false;

    return opts->device_mode == BX_GREP_DEVICE_SKIP || !opts->device_mode_explicit;
}

static char *display_path_for_output(const char *path,
                                     bool strip_dot_prefix,
                                     const struct search_opts *opts) {
    return bx_rg_display_path_dup(path, strip_dot_prefix,
                                  opts ? opts->path_separator : '/');
}

char *bx_search_display_path_for_output(const char *path,
                                        bool strip_dot_prefix,
                                        const struct search_opts *opts) {
    return display_path_for_output(path, strip_dot_prefix, opts);
}

static const char *const rg_ignore_filenames[] = {
    ".gitignore",
    ".ignore",
    ".rgignore",
};

struct bx_walk_opts bx_search_make_walk_opts(const char *progname,
                                             enum bx_search_personality personality,
                                             const struct search_opts *opts,
                                             bool *stop) {
    return (struct bx_walk_opts){
        .sort_entries = bx_search_use_rg_sort_policy(personality, opts),
        .reverse_sort = bx_search_sort_is_path(opts)
            && bx_search_sort_is_descending(opts),
        .follow_symlinks = opts->follow_symlinks,
        .follow_root_symlink = true,
        .post_order = false,
        .stay_on_filesystem = opts->stay_on_filesystem,
        .stop = stop,
        .suppress_eacces = false,
        .suppress_errors = opts->suppress_errors,
        .report_eacces = false,
        .os_error_style = progname_uses_os_error_style(progname),
        .error_prefix = progname,
        .max_depth = opts->max_depth,
        .cycle_mode = bx_search_cycle_mode(personality, opts),
        .cycle_report = bx_search_cycle_report(personality, opts),
        .counter_ops = bx_search_dev_counters_enabled() ? &bx_search_walk_counter_ops : NULL,
    };
}

struct bx_walk_filter_opts bx_search_make_filter_opts(const struct search_opts *opts) {
    return (struct bx_walk_filter_opts){
        .hidden = opts->hidden,
        .type_filter = '\0',
        .glob_case_insensitive = opts->glob_case_insensitive,
        .include_patterns = opts->include_patterns,
        .include_pattern_casefold = opts->include_pattern_casefold,
        .include_pattern_is_type = opts->include_pattern_is_type,
        .num_include_patterns = opts->num_include,
        .exclude_patterns = opts->exclude_patterns,
        .exclude_pattern_is_type = opts->exclude_pattern_is_type,
        .num_exclude_patterns = opts->num_exclude,
        .exclude_dirs = opts->exclude_dir_patterns,
        .num_exclude_dirs = opts->num_exclude_dir,
    };
}

struct bx_walk_ignore_opts bx_search_make_ignore_opts(const char *progname,
                                                      const struct search_opts *opts) {
    return (struct bx_walk_ignore_opts){
        .no_ignore = opts->no_ignore,
        .no_ignore_parent = opts->no_ignore_parent,
        .no_ignore_vcs = opts->no_ignore_vcs,
        .no_ignore_dot = opts->no_ignore_dot,
        .no_ignore_exclude = opts->no_ignore_exclude,
        .no_ignore_files = opts->no_ignore_files,
        .no_ignore_global = opts->no_ignore_global,
        .no_require_git = opts->no_require_git,
        .ignore_file_case_insensitive = opts->ignore_file_case_insensitive,
        .suppress_ignore_messages = opts->suppress_ignore_messages,
        .os_error_style = progname_uses_os_error_style(progname),
        .error_prefix = progname,
        .git_root = NULL,
        .extra_ignore_files = opts->ignore_files,
        .num_extra_ignore_files = opts->num_ignore_files,
        .gitignore_enabled = false,
        .git_root_resolved = false,
        .ignore_filenames = rg_ignore_filenames,
        .num_ignore_filenames = 3,
    };
}

bool bx_search_explicit_entry_selected(const struct search_opts *opts,
                                       const char *path) {
    const char *name = bx_path_basename_ptr(path);

    if (opts->num_include > 0) {
        struct bx_walk_filter_opts filter_opts = {
            .hidden = opts->hidden,
            .glob_case_insensitive = opts->glob_case_insensitive,
            .include_patterns = opts->include_patterns,
            .include_pattern_casefold = opts->include_pattern_casefold,
            .include_pattern_is_type = opts->include_pattern_is_type,
            .num_include_patterns = opts->num_include,
        };
        struct bx_walk_filter_state filter_state;
        bx_walk_filter_init(&filter_state, &filter_opts, path);
        if (!bx_walk_filter_matches_include(&filter_state, name, path))
            return false;
    }

    for (int i = 0; i < opts->num_exclude; i++) {
        int flags = opts->glob_case_insensitive ? FNM_CASEFOLD : 0;
        if (fnmatch(opts->exclude_patterns[i], name, flags) == 0)
            return false;
    }

    return true;
}
