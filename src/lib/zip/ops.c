/* Ensure POSIX macros (WIFEXITED, etc.) are available */
#define _POSIX_C_SOURCE 200809L

#include "ops.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <errno.h>

#include "lib/argv_packer.h"
#include "lib/child_runner.h"
#include "reader.h"
#include "writer.h"
#include "ziputils.h"

/*
 * Execution dispatcher for zip and unzip front-ends
 *
 * This file owns the high-level operation selection for both commands
 * - zip path: fix, test-only, comment-only validation, modify/create, optional post-write test
 * - unzip path: list, test, extract
 *
 * It intentionally does not implement archive format details
 * - ZIP parsing and validation live in reader/test helpers
 * - Writing and mutation live in writer/modify helpers
 *
 * Error reporting model
 * - Operations return ZU_STATUS_* codes
 * - CLI layers map those to exit codes and decide how to format messages
 * - This file prints only user-facing text that is intrinsic to the operation flow
 */

/* --- Helpers --- */

/*
 * Build a direct-exec argv for archive testing based on a template and a target path
 *
 * Template rules
 * - If the template contains the first occurrence of "{}", that substring is replaced by target
 * - If the template does not contain "{}", target is appended as a separate argument
 *
 * The template accepts shell-like whitespace, quotes, and backslash grouping,
 * but never expansion, redirection, pipelines, or shell execution.
 */
static char** build_test_argv(const char* templ, const char* target) {
    if (!templ || !target)
        return NULL;

    char** argv = NULL;
    if (bx_argv_parse_command(templ, &argv) != 0)
        return NULL;

    size_t argc = 0;
    bool replaced = false;
    for (; argv[argc]; argc++) {
        char* placeholder = replaced ? NULL : strstr(argv[argc], "{}");
        if (!placeholder)
            continue;
        size_t prefix_len = (size_t)(placeholder - argv[argc]);
        size_t suffix_len = strlen(placeholder + 2);
        size_t target_len = strlen(target);
        if (prefix_len > SIZE_MAX - target_len
            || prefix_len + target_len > SIZE_MAX - suffix_len - 1u) {
            bx_argv_free(argv);
            return NULL;
        }
        char* expanded = malloc(prefix_len + target_len + suffix_len + 1u);
        if (!expanded) {
            bx_argv_free(argv);
            return NULL;
        }
        memcpy(expanded, argv[argc], prefix_len);
        memcpy(expanded + prefix_len, target, target_len);
        memcpy(expanded + prefix_len + target_len, placeholder + 2, suffix_len + 1u);
        free(argv[argc]);
        argv[argc] = expanded;
        replaced = true;
    }

    if (!replaced) {
        if (argc == SIZE_MAX / sizeof(*argv) - 1u) {
            bx_argv_free(argv);
            return NULL;
        }
        char** grown = realloc(argv, (argc + 2u) * sizeof(*grown));
        if (!grown) {
            bx_argv_free(argv);
            return NULL;
        }
        argv = grown;
        argv[argc] = strdup(target);
        if (!argv[argc]) {
            argv[argc] = NULL;
            bx_argv_free(argv);
            return NULL;
        }
        argv[argc + 1u] = NULL;
    }
    return argv;
}

struct zu_test_child_result {
    bool reaped;
    bool exec_failed;
    int exec_errno;
    int status;
};

static void record_test_child_status(pid_t pid,
                                     int status,
                                     bool exec_failed,
                                     int exec_errno,
                                     void* user) {
    struct zu_test_child_result* result = user;
    (void)pid;
    result->reaped = true;
    result->exec_failed = exec_failed;
    result->exec_errno = exec_errno;
    result->status = status;
}

/*
 * Run the configured test command against a target archive path
 *
 * Behavior
 * - Builds a bounded argv from ctx->test_command and the target path
 * - Prints progress messages unless quiet
 * - Executes directly through bx child_runner, never through a shell
 *
 * Return codes
 * - ZU_STATUS_OK if the command exits successfully (exit code 0)
 * - ZU_STATUS_OOM if command string allocation fails
 * - ZU_STATUS_IO for spawn/wait failures or non-zero command outcomes
 *
 * Output expectations
 * - This uses "zip:" prefixes to match the invoking tool's UX expectations
 * - The caller chooses when this is invoked (pure test mode vs post-write test)
 */
static int run_test_command(ZContext* ctx, const char* target) {
    char** argv = build_test_argv(ctx->test_command, target);
    if (!argv)
        return ZU_STATUS_OOM;

    size_t argv_bytes = bx_argv_bytes(argv);
    size_t argv_limit = bx_argv_effective_char_limit(0);
    if (argv_bytes == (size_t)-1 || (argv_limit > 0 && argv_bytes > argv_limit)) {
        bx_argv_free(argv);
        zu_context_set_error(ctx, ZU_STATUS_USAGE, "test command exceeds the system argv limit");
        return ZU_STATUS_USAGE;
    }

    if (!ctx->quiet) {
        printf("Testing archive: %s\n", target);
        if (ctx->verbose) {
            printf("Executing:");
            for (size_t i = 0; argv[i]; i++)
                printf(" %s", argv[i]);
            putchar('\n');
        }
    }

    struct bx_child child[1] = {0};
    int running = 0;
    struct bx_child_runner_opts options = bx_child_runner_opts_default();
    struct zu_test_child_result result = {0};
    options.reset_common_signals = true;
    if (bx_child_spawn_argv("zip", argv, &options, 0, child, &running, NULL, NULL) != 0) {
        bx_argv_free(argv);
        return ZU_STATUS_IO;
    }
    bx_argv_free(argv);
    if (bx_child_reap(child, &running, true, true, record_test_child_status, &result) != 0
        || !result.reaped) {
        return ZU_STATUS_IO;
    }

    if (!result.exec_failed && WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0) {
        if (!ctx->quiet)
            printf("Test of %s OK\n", target);
        return ZU_STATUS_OK;
    }

    if (result.exec_failed) {
        fprintf(stderr, "zip: test command failed to execute: %s\n", strerror(result.exec_errno));
    }
    else if (WIFEXITED(result.status)) {
        fprintf(stderr, "zip: test command failed (exit code %d)\n", WEXITSTATUS(result.status));
    }
    else if (WIFSIGNALED(result.status)) {
        fprintf(stderr, "zip: test command terminated by signal %d\n", WTERMSIG(result.status));
    }
    else {
        fprintf(stderr, "zip: test command failed abnormally\n");
    }

    return ZU_STATUS_IO;
}

/* --- Public Operations --- */

int zu_zip_run(ZContext* ctx) {
    if (!ctx)
        return ZU_STATUS_USAGE;

    /*
     * zip operation dispatch order
     *
     * 1) Fix modes (-F / -FF)
     *    - Force archive recovery logic in the writer path
     *    - Treated as a specialized modification run
     *
     * 2) Test-only mode (-T with no file operands)
     *    - Validates an existing archive without writing anything
     *    - Uses an external test command if configured, otherwise internal verifier
     *
     * 3) Input validation for modification modes
     *    - Without operands, zip has nothing to add/remove unless comment input is active
     *    - zip -z archive.zip is handled by the CLI layer by setting zip_comment_specified
     *
     * 4) Modify or create archive
     *    - Central directory update and entry writing handled by zu_modify_archive
     *
     * 5) Optional post-write test (-T with operands)
     *    - Verifies the resulting archive (output path if specified, else archive path)
     */

    // 1) Archive recovery / fix mode
    if (ctx->fix_archive || ctx->fix_fix_archive) {
        return zu_modify_archive(ctx);
    }

    // 2) Pure test mode: -T with no file operands means "verify only"
    if (ctx->test_integrity && ctx->include.len == 0) {
        if (ctx->test_command) {
            return run_test_command(ctx, ctx->archive_path);
        }

        int rc = zu_test_archive(ctx);
        if (rc == ZU_STATUS_OK && !ctx->quiet) {
            printf("No errors detected in compressed data of %s.\n", ctx->archive_path);
        }
        return rc;
    }

    // 3) Modification requires either file operands or an explicit comment read
    if (ctx->include.len == 0 && !ctx->zip_comment_specified) {
        if (ctx->stdin_names_read) {
            fprintf(stderr, "zip: error: no input files specified\n");
            return ZU_STATUS_USAGE;
        }
        printf("zip error: Nothing to do! (%s)\n", ctx->archive_path ? ctx->archive_path : "");
        return ZU_STATUS_NO_FILES;
    }

    // 4) Create/modify archive
    int rc = zu_modify_archive(ctx);

    // 5) Post-write testing when -T was requested for a write run
    if (rc == ZU_STATUS_OK && ctx->test_integrity) {
        const char* target = ctx->output_path ? ctx->output_path : ctx->archive_path;

        if (ctx->test_command) {
            rc = run_test_command(ctx, target);
        }
        else {
            /*
             * Internal verifier reads ctx->archive_path, so temporarily repoint it
             * The writer already produced target, this keeps verifier logic unchanged
             */
            const char* saved_path = ctx->archive_path;
            ctx->archive_path = target;

            rc = zu_test_archive(ctx);

            ctx->archive_path = saved_path;

            if (rc == ZU_STATUS_OK && !ctx->quiet) {
                printf("test of %s OK\n", target);
            }
        }
    }

    return rc;
}

int zu_unzip_run(ZContext* ctx) {
    if (!ctx)
        return ZU_STATUS_USAGE;

    /*
     * unzip operation dispatch
     *
     * The CLI layer sets one of these modes via ctx flags
     * - list_only: list archive contents
     * - test_integrity: verify compressed data without extracting
     * - default: extract matching entries
     */

    if (ctx->list_only) {
        return zu_list_archive(ctx);
    }

    if (ctx->test_integrity) {
        int rc = zu_test_archive(ctx);
        if (rc == ZU_STATUS_OK && !ctx->quiet) {
            printf("No errors detected in compressed data of %s.\n", ctx->archive_path);
        }
        return rc;
    }

    return zu_extract_archive(ctx);
}
