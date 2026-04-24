#include <fnmatch.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "filter.h"
#include "fswalk/walk.h"
#include "ignore.h"
#include "lib/cli_common.h"
#include "lib/path_ops.h"
#include "options.h"
#include "rg_output.h"
#include "search_internal.h"
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

bool bx_search_path_exceeds_max_filesize(const char *path,
                                         const struct search_opts *opts) {
    if (!path || !opts || !opts->max_filesize_set)
        return false;

    struct stat st;
    if (stat(path, &st) != 0)
        return false;
    return S_ISREG(st.st_mode) && st.st_size > (off_t)opts->max_filesize;
}

bool bx_search_entry_exceeds_max_filesize(struct bx_walk_entry *entry,
                                          const struct search_opts *opts) {
    if (!entry || !opts || !opts->max_filesize_set || entry->is_dir)
        return false;
    if (!entry->metadata_loaded && !bx_walk_entry_load_metadata(entry))
        return false;
    return entry->metadata_loaded && S_ISREG(entry->mode)
        && entry->size > (off_t)opts->max_filesize;
}

static bool bx_search_personality_is_rg(enum bx_search_personality personality) {
    return personality == BX_SEARCH_RG;
}

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

    if (progname_uses_os_error_style(progname))
        fprintf(bx_search_error_output_stream(), "%s: %s: %s (os error %d)\n",
                progname, path, strerror(errnum), errnum);
    else
        fprintf(bx_search_error_output_stream(), "%s: %s: %s\n",
                progname, path, strerror(errnum));
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

bool bx_search_should_skip_special_input_mode(mode_t mode,
                                              const struct search_opts *opts) {
    return opts && opts->device_mode == BX_GREP_DEVICE_SKIP
        && bx_search_mode_is_special_input(mode);
}

bool bx_search_entry_should_skip_special_input(struct bx_walk_entry *entry,
                                               const struct search_opts *opts) {
    if (!entry || !opts || opts->device_mode != BX_GREP_DEVICE_SKIP)
        return false;

    if (!entry->metadata_loaded && !bx_walk_entry_load_metadata(entry))
        return false;

    return bx_search_mode_is_special_input(entry->mode);
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
    };
}

struct bx_walk_filter_opts bx_search_make_filter_opts(const struct search_opts *opts) {
    return (struct bx_walk_filter_opts){
        .hidden = opts->hidden,
        .glob_case_insensitive = opts->glob_case_insensitive,
        .include_patterns = opts->include_patterns,
        .include_pattern_casefold = opts->include_pattern_casefold,
        .num_include_patterns = opts->num_include,
        .exclude_patterns = opts->exclude_patterns,
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
