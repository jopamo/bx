#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets.h"
#include "lib/cli_common.h"
#include "lib/copy_metadata.h"
#include "lib/copy_tree.h"
#include "lib/option_scan.h"
#include "lib/path_ops.h"
#include "lib/stat_ops.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/args_common.h"

struct bx_cp_options {
    const char* progname;
    bool strip_trailing_slashes;
    bool show_help;
    bool show_version;
    bool no_target_directory;
    enum bx_backup_mode backup_mode;
    const char* suffix;
    const char* target_directory;
    struct bx_copy_options copy;
    const char** operands;
    int operand_count;
};

enum bx_cp_longopt {
    BX_CP_OPT_ATTRIBUTES_ONLY = 256,
    BX_CP_OPT_BACKUP,
    BX_CP_OPT_COPY_CONTENTS,
    BX_CP_OPT_DEBUG,
    BX_CP_OPT_KEEP_DIRECTORY_SYMLINK,
    BX_CP_OPT_PRESERVE,
    BX_CP_OPT_NO_PRESERVE,
    BX_CP_OPT_PARENTS,
    BX_CP_OPT_REFLINK,
    BX_CP_OPT_REMOVE_DESTINATION,
    BX_CP_OPT_SPARSE,
    BX_CP_OPT_STRIP_TRAILING_SLASHES,
    BX_CP_OPT_UPDATE,
    BX_CP_OPT_JOBS,
    BX_CP_OPT_HELP,
    BX_CP_OPT_VERSION,
};

static void bx_cp_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [-T] SOURCE DEST\n", progname);
    fprintf(stream, "  or:  %s [OPTION]... SOURCE... DIRECTORY\n", progname);
    fprintf(stream, "  or:  %s [OPTION]... -t DIRECTORY SOURCE...\n", progname);
    fprintf(stream, "Copy SOURCE to DEST, or multiple SOURCE(s) to DIRECTORY.\n");
    fprintf(stream, "\n");
    fprintf(stream, "Supported options:\n");
    fprintf(stream, "  -a, --archive              same as -dR --preserve=all\n");
    fprintf(stream, "      --attributes-only      don't copy file data, only create destination objects\n");
    fprintf(stream, "      --copy-contents        copy contents of special files when requested\n");
    fprintf(stream, "  -d                         same as --no-dereference --preserve=links\n");
    fprintf(stream, "  -f, --force                remove destination and retry on open/link failures\n");
    fprintf(stream, "  -H                         follow command-line symbolic links in SOURCE\n");
    fprintf(stream, "      --jobs=N               use 1..8 workers for independent recursive file data (default 2)\n");
    fprintf(stream, "  -L, --dereference          always follow symbolic links in SOURCE\n");
    fprintf(stream, "  -P, --no-dereference       never follow symbolic links in SOURCE\n");
    fprintf(stream, "  -l, --link                 hard link files instead of copying\n");
    fprintf(stream, "  -n, --no-clobber           do not overwrite an existing destination\n");
    fprintf(stream, "  -p                         preserve mode, ownership, and timestamps\n");
    fprintf(stream, "      --preserve=ATTR_LIST   preserve mode,ownership,timestamps,links,all\n");
    fprintf(stream, "      --no-preserve=LIST     clear preserved attributes from the active set\n");
    fprintf(stream, "      --parents              use full source path under the target directory\n");
    fprintf(stream, "  -R, -r, --recursive        copy directories recursively\n");
    fprintf(stream, "      --remove-destination   unlink destination before creating/replacing it\n");
    fprintf(stream, "      --strip-trailing-slashes  remove trailing slashes from SOURCE operands\n");
    fprintf(stream, "  -s, --symbolic-link        make symbolic links instead of copying\n");
    fprintf(stream, "  -t, --target-directory=DIR copy all SOURCE arguments into DIR\n");
    fprintf(stream, "  -T, --no-target-directory  treat DEST as a normal path\n");
    fprintf(stream, "      --update[=MODE]        MODE is all, none, none-fail, or older\n");
    fprintf(stream, "  -u                         same as --update=older\n");
    fprintf(stream, "  -v, --verbose              explain what is being done\n");
    fprintf(stream, "  -x, --one-file-system      stay on this file system\n");
    fprintf(stream, "      --help                 display this help and exit\n");
    fprintf(stream, "      --version              output version information and exit\n");
}

static bool bx_cp_parse_preserve_list(struct bx_diag_ctx* diag, const char* arg, unsigned* mask, bool set_bits, bool* mode_mentioned_out) {
    char* invalid_token = NULL;
    if (!bx_args_parse_preserve_list(arg, mask, set_bits, mode_mentioned_out, &invalid_token)) {
        bx_diag(diag, "invalid attribute '%s'", invalid_token);
        free(invalid_token);
        return false;
    }
    return true;
}

static bool bx_cp_parse_update_mode(struct bx_diag_ctx* diag, const char* arg, enum bx_update_mode* mode_out) {
    if (!bx_args_parse_update_mode(arg, mode_out)) {
        bx_diag(diag, "invalid --update mode '%s'", arg);
        return false;
    }
    return true;
}

static bool bx_cp_parse_options(int argc, char** argv, struct bx_cp_options* options, struct bx_diag_ctx* diag) {
    static const struct bx_option_spec specs[] = {
        {'a', 'a', "archive", BX_OPTION_NO_VALUE},
        {BX_CP_OPT_ATTRIBUTES_ONLY, 0, "attributes-only", BX_OPTION_NO_VALUE},
        {BX_CP_OPT_BACKUP, 0, "backup", BX_OPTION_OPTIONAL_VALUE},
        {BX_CP_OPT_COPY_CONTENTS, 0, "copy-contents", BX_OPTION_NO_VALUE},
        {BX_CP_OPT_DEBUG, 0, "debug", BX_OPTION_NO_VALUE},
        {'f', 'f', "force", BX_OPTION_NO_VALUE},
        {'i', 'i', "interactive", BX_OPTION_NO_VALUE},
        {BX_CP_OPT_JOBS, 0, "jobs", BX_OPTION_REQUIRED_VALUE},
        {'L', 'L', "dereference", BX_OPTION_NO_VALUE},
        {'P', 'P', "no-dereference", BX_OPTION_NO_VALUE},
        {BX_CP_OPT_KEEP_DIRECTORY_SYMLINK, 0, "keep-directory-symlink", BX_OPTION_NO_VALUE},
        {'l', 'l', "link", BX_OPTION_NO_VALUE},
        {'n', 'n', "no-clobber", BX_OPTION_NO_VALUE},
        {BX_CP_OPT_PRESERVE, 0, "preserve", BX_OPTION_OPTIONAL_VALUE},
        {BX_CP_OPT_NO_PRESERVE, 0, "no-preserve", BX_OPTION_REQUIRED_VALUE},
        {BX_CP_OPT_PARENTS, 0, "parents", BX_OPTION_NO_VALUE},
        {'R', 'R', "recursive", BX_OPTION_NO_VALUE},
        {BX_CP_OPT_REFLINK, 0, "reflink", BX_OPTION_OPTIONAL_VALUE},
        {BX_CP_OPT_REMOVE_DESTINATION, 0, "remove-destination", BX_OPTION_NO_VALUE},
        {BX_CP_OPT_SPARSE, 0, "sparse", BX_OPTION_REQUIRED_VALUE},
        {BX_CP_OPT_STRIP_TRAILING_SLASHES, 0, "strip-trailing-slashes", BX_OPTION_NO_VALUE},
        {'s', 's', "symbolic-link", BX_OPTION_NO_VALUE},
        {'S', 'S', "suffix", BX_OPTION_REQUIRED_VALUE},
        {'t', 't', "target-directory", BX_OPTION_REQUIRED_VALUE},
        {'T', 'T', "no-target-directory", BX_OPTION_NO_VALUE},
        {BX_CP_OPT_UPDATE, 0, "update", BX_OPTION_OPTIONAL_VALUE},
        {'v', 'v', "verbose", BX_OPTION_NO_VALUE},
        {'x', 'x', "one-file-system", BX_OPTION_NO_VALUE},
        {BX_CP_OPT_HELP, 0, "help", BX_OPTION_NO_VALUE},
        {BX_CP_OPT_VERSION, 0, "version", BX_OPTION_NO_VALUE},
        {'b', 'b', NULL, BX_OPTION_NO_VALUE},
        {'d', 'd', NULL, BX_OPTION_NO_VALUE},
        {'H', 'H', NULL, BX_OPTION_NO_VALUE},
        {'p', 'p', NULL, BX_OPTION_NO_VALUE},
        {'r', 'r', NULL, BX_OPTION_NO_VALUE},
        {'u', 'u', NULL, BX_OPTION_NO_VALUE},
    };
    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "cp");
    options->copy.deref_mode = BX_DEREF_DEFAULT;
    options->copy.sparse_mode = BX_SPARSE_AUTO;
    options->copy.reflink_mode = BX_REFLINK_NEVER;
    options->copy.mode_policy = BX_MODE_POLICY_DEFAULT;
    options->copy.update_mode = BX_UPDATE_ALL;
    options->copy.workers = 2;

    diag->progname = options->progname;

    struct bx_option_cursor cursor;
    size_t end = argc > 0 ? (size_t)argc : 0;
    bx_option_cursor_init(&cursor, argv, end != 0, end, BX_OPTION_ABBREVIATE,
                          specs, sizeof(specs) / sizeof(specs[0]));
    for (;;) {
        struct bx_option_result result = bx_option_next(&cursor);
        if (result.event == BX_OPTION_END)
            break;
        if (result.event == BX_OPTION_OPERAND) {
            if (options->operands == NULL) {
                size_t capacity = end - 1;
                if (capacity > SIZE_MAX / sizeof(*options->operands) ||
                    (options->operands = malloc(capacity * sizeof(*options->operands))) == NULL) {
                    bx_diag(diag, "memory exhausted");
                    return false;
                }
            }
            options->operands[options->operand_count++] = result.token;
            continue;
        }
        if (result.event != BX_OPTION_MATCH) {
            if (result.event == BX_OPTION_MISSING_VALUE) {
                if (result.short_name != 0)
                    bx_diag(diag, "option requires an argument -- '%c'", result.short_name);
                else
                    bx_diag(diag, "option '%s' requires an argument", result.token);
            }
            else if (result.short_name != 0)
                bx_diag(diag, "invalid option -- '%c'", result.short_name);
            else
                bx_diag(diag, "unrecognized option '%s'", result.token);
            return false;
        }

        const char* value = result.value;
        switch (result.spec->id) {
            case BX_CP_OPT_JOBS:
                if (value[0] < '1' || value[0] > '0' + (int)BX_COPY_MAX_WORKERS || value[1] != '\0') {
                    bx_diag(diag, "invalid --jobs value '%s' (expected 1..8)", value);
                    return false;
                }
                options->copy.workers = (unsigned)(value[0] - '0');
                break;
            case BX_CP_OPT_HELP:
                options->show_help = true;
                break;
            case BX_CP_OPT_VERSION:
                options->show_version = true;
                break;
            case 'a':
                options->copy.recursive = true;
                options->copy.deref_mode = BX_DEREF_NEVER;
                options->copy.mode_policy = BX_MODE_POLICY_PRESERVE;
                options->copy.preserve_mask |= BX_PRESERVE_ALL;
                break;
            case BX_CP_OPT_ATTRIBUTES_ONLY:
                options->copy.attributes_only = true;
                break;
            case 'b':
                bx_args_enable_backup_mode(&options->backup_mode);
                break;
            case BX_CP_OPT_BACKUP:
                if (value == NULL) {
                    bx_args_enable_backup_mode(&options->backup_mode);
                }
                else if (!bx_args_parse_backup_mode(value, &options->backup_mode)) {
                    bx_diag(diag, "invalid --backup control value '%s'", value);
                    return false;
                }
                break;
            case BX_CP_OPT_COPY_CONTENTS:
                options->copy.copy_contents = true;
                break;
            case 'S':
                options->suffix = value;
                break;
            case BX_CP_OPT_REFLINK:
                if (value == NULL) {
                    options->copy.reflink_mode = BX_REFLINK_ALWAYS;
                }
                else if (strcmp(value, "always") == 0) {
                    options->copy.reflink_mode = BX_REFLINK_ALWAYS;
                }
                else if (strcmp(value, "auto") == 0) {
                    options->copy.reflink_mode = BX_REFLINK_AUTO;
                }
                else if (strcmp(value, "never") == 0) {
                    options->copy.reflink_mode = BX_REFLINK_NEVER;
                }
                else {
                    bx_diag(diag, "invalid --reflink argument '%s'", value);
                    return false;
                }
                break;
            case BX_CP_OPT_SPARSE:
                if (strcmp(value, "always") == 0) {
                    options->copy.sparse_mode = BX_SPARSE_ALWAYS;
                }
                else if (strcmp(value, "auto") == 0) {
                    options->copy.sparse_mode = BX_SPARSE_AUTO;
                }
                else if (strcmp(value, "never") == 0) {
                    options->copy.sparse_mode = BX_SPARSE_NEVER;
                }
                else {
                    bx_diag(diag, "invalid --sparse argument '%s'", value);
                    return false;
                }
                break;
            case BX_CP_OPT_DEBUG:
                options->copy.debug = true;
                options->copy.verbose = true;
                break;
            case 'd':
                options->copy.deref_mode = BX_DEREF_NEVER;
                options->copy.preserve_mask |= BX_PRESERVE_LINKS;
                break;
            case 'f':
                options->copy.force = true;
                break;
            case 'i':
                options->copy.interactive = true;
                options->copy.no_clobber = false;
                break;
            case 'H':
                options->copy.deref_mode = BX_DEREF_COMMAND_LINE;
                break;
            case 'L':
                options->copy.deref_mode = BX_DEREF_ALWAYS;
                break;
            case 'P':
                options->copy.deref_mode = BX_DEREF_NEVER;
                break;
            case BX_CP_OPT_KEEP_DIRECTORY_SYMLINK:
                /* Accepted as a no-op selector for current destination handling. */
                break;
            case 'l':
                options->copy.hard_link = true;
                break;
            case 'n':
                options->copy.no_clobber = true;
                options->copy.interactive = false;
                break;
            case 'p':
                options->copy.mode_policy = BX_MODE_POLICY_PRESERVE;
                options->copy.preserve_mask |= BX_PRESERVE_MODE | BX_PRESERVE_OWNERSHIP | BX_PRESERVE_TIMESTAMPS;
                break;
            case BX_CP_OPT_PRESERVE:
                if (value == NULL) {
                    options->copy.mode_policy = BX_MODE_POLICY_PRESERVE;
                    options->copy.preserve_mask |= BX_PRESERVE_MODE | BX_PRESERVE_OWNERSHIP | BX_PRESERVE_TIMESTAMPS;
                }
                else {
                    bool mode_mentioned = false;
                    if (!bx_cp_parse_preserve_list(diag, value, &options->copy.preserve_mask, true, &mode_mentioned)) {
                        return false;
                    }
                    if (mode_mentioned) {
                        options->copy.mode_policy = BX_MODE_POLICY_PRESERVE;
                    }
                }
                break;
            case BX_CP_OPT_NO_PRESERVE: {
                bool mode_mentioned = false;
                if (!bx_cp_parse_preserve_list(diag, value, &options->copy.preserve_mask, false, &mode_mentioned)) {
                    return false;
                }
                if (mode_mentioned) {
                    options->copy.mode_policy = BX_MODE_POLICY_NO_PRESERVE;
                }
                break;
            }
            case BX_CP_OPT_PARENTS:
                options->copy.parents = true;
                break;
            case 'R':
            case 'r':
                options->copy.recursive = true;
                break;
            case BX_CP_OPT_REMOVE_DESTINATION:
                options->copy.remove_destination = true;
                break;
            case BX_CP_OPT_STRIP_TRAILING_SLASHES:
                options->strip_trailing_slashes = true;
                break;
            case 's':
                options->copy.symbolic_link = true;
                break;
            case 't':
                options->target_directory = value;
                break;
            case 'T':
                options->no_target_directory = true;
                break;
            case BX_CP_OPT_UPDATE:
                if (!bx_cp_parse_update_mode(diag, value, &options->copy.update_mode)) {
                    return false;
                }
                break;
            case 'u':
                options->copy.update_mode = BX_UPDATE_OLDER;
                break;
            case 'v':
                options->copy.verbose = true;
                break;
            case 'x':
                options->copy.one_file_system = true;
                break;
            default:
                bx_diag(diag, "internal option parsing error");
                return false;
        }
    }

    if (options->copy.hard_link && options->copy.symbolic_link) {
        bx_diag(diag, "cannot combine --link and --symbolic-link");
        return false;
    }
    if (bx_args_backup_mode_requested(options->backup_mode) && (options->copy.no_clobber || options->copy.update_mode == BX_UPDATE_NONE || options->copy.update_mode == BX_UPDATE_NONE_FAIL)) {
        bx_diag(diag, "cannot combine --backup with -n, --update=none, or --update=none-fail");
        return false;
    }
    if (options->target_directory && options->no_target_directory) {
        bx_diag(diag, "cannot combine --target-directory and --no-target-directory");
        return false;
    }
    if (options->copy.parents && options->no_target_directory) {
        bx_diag(diag, "--parents requires a target directory");
        return false;
    }
    return true;
}

static char* bx_cp_build_dest_path(const struct bx_cp_options* options, const char* source_operand, const char* destination_root, bool destination_is_directory) {
    return bx_path_build_dest(source_operand, destination_root, destination_is_directory, options->copy.parents);
}

static void bx_cp_init_copy_context(struct bx_copy_context* ctx, const struct bx_cp_options* options, struct bx_diag_ctx* diag, const char* destination_root, mode_t umask_value) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->options = &options->copy;
    ctx->diag = diag;
    ctx->umask_value = umask_value;
    ctx->target_root = destination_root;
    bx_backup_get_params(options->backup_mode, options->suffix, &ctx->backup_params);
}

static int bx_cp_run(const struct bx_cp_options* options, struct bx_diag_ctx* diag) {
    struct bx_copy_context copy_ctx;

    diag->verbose = options->copy.verbose;
    diag->debug = options->copy.debug;

    if (options->show_help) {
        bx_cp_print_help(stdout, options->progname);
        return 0;
    }
    if (options->show_version) {
        bx_cli_print_version(options->progname);
        return 0;
    }

    int operand_count = options->operand_count;
    if (operand_count <= 0) {
        bx_diag(diag, "missing file operand");
        return 1;
    }

    const char* destination_root = NULL;
    int source_count = 0;
    const char* const* source_operands = options->operands;
    bool destination_is_directory = false;

    if (options->target_directory != NULL) {
        destination_root = options->target_directory;
        source_count = operand_count;
        if (!bx_stat_is_dir_path(destination_root)) {
            bx_diag(diag, "target '%s' is not a directory", destination_root);
            return 1;
        }
        destination_is_directory = true;
    }
    else if (options->no_target_directory) {
        if (operand_count < 2) {
            bx_diag(diag, "missing destination file operand after '%s'", source_operands[0]);
            return 1;
        }
        if (operand_count > 2) {
            bx_diag(diag, "extra operand '%s'", source_operands[2]);
            return 1;
        }

        destination_root = source_operands[1];
        source_count = 1;
    }
    else {
        if (operand_count < 2) {
            bx_diag(diag, "missing destination file operand after '%s'", source_operands[0]);
            return 1;
        }
        destination_root = source_operands[operand_count - 1];
        source_count = operand_count - 1;

        if (source_count > 1) {
            if (!bx_stat_is_dir_path(destination_root)) {
                bx_diag(diag, "target '%s' is not a directory", destination_root);
                return 1;
            }
            destination_is_directory = true;
        }
        else if (bx_stat_is_dir_path(destination_root)) {
            destination_is_directory = true;
        }

        if (options->copy.parents && !destination_is_directory) {
            bx_diag(diag, "--parents requires a directory destination");
            return 1;
        }
    }

    mode_t old_umask = umask(0);
    umask(old_umask);

    bx_cp_init_copy_context(&copy_ctx, options, diag, destination_root, old_umask);

    for (int i = 0; i < source_count; i++) {
        char* lookup_path = xstrdup(source_operands[i]);
        char* source_operand = options->strip_trailing_slashes ? bx_path_strip_trailing_slashes_dup(source_operands[i]) : xstrdup(source_operands[i]);
        char* dest_path = bx_cp_build_dest_path(options, source_operand, destination_root, destination_is_directory);

        copy_ctx.stop_current_source = false;
        copy_ctx.current_source_root = lookup_path;
        copy_ctx.current_dest_root = dest_path;

        bx_copy_path(&copy_ctx, lookup_path, source_operand, dest_path, true);

        free(copy_ctx.current_dest_root_realpath);
        copy_ctx.current_dest_root_realpath = NULL;
        copy_ctx.current_dest_root = NULL;
        copy_ctx.current_source_root = NULL;
        free(dest_path);
        free(source_operand);
        free(lookup_path);
    }

    bx_copy_free_links(&copy_ctx);
    bx_copy_free_source_dirs(&copy_ctx);
    bx_copy_free_parent_attrs(&copy_ctx);
    bx_copy_buffer_release(&copy_ctx.buffer);
    bx_overwrite_free_written(&copy_ctx.written);
    return diag->exit_status;
}

int bx_cp_main(int argc, char** argv) {
    struct bx_cp_options options;
    struct bx_diag_ctx diag = {0};
    int status = bx_cp_parse_options(argc, argv, &options, &diag) ? bx_cp_run(&options, &diag) : 1;
    free(options.operands);
    return status;
}
