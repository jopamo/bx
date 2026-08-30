#define _XOPEN_SOURCE 700

#include "fileio.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>
#include <fnmatch.h>

#ifndef FNM_CASEFOLD
#define FNM_CASEFOLD 0
#endif

/*
 * File and path utilities used by zip/unzip execution
 *
 * Responsibilities
 * - Open/close archive input and output streams attached to ZContext
 * - Detect split archive fragments and reject them when unsupported
 * - Expand CLI operands when recursive traversal is enabled
 * - Apply include/exclude pattern rules to produce a final list of inputs
 *
 * Conventions
 * - Paths stored in ctx lists are normalized by stripping leading "./" segments
 * - Directory recursion uses lstat so symlink policy can be enforced by the caller
 * - Pattern matching uses fnmatch with optional case folding when available
 *
 * Notes
 * - This file is intentionally conservative about continuing on filesystem errors
 *   traversal warnings are logged and the run proceeds where possible
 */

/*
 * Close a FILE* and clear the caller's pointer
 *
 * This is used to manage ctx->in_file and ctx->out_file safely
 */
static void close_file(FILE** fp) {
    if (fp && *fp) {
        fclose(*fp);
        *fp = NULL;
    }
}

/*
 * Check whether a path ends with ".zip" using case-insensitive comparison
 *
 * This is used only as a heuristic for split archive detection
 */
static bool has_zip_suffix(const char* path) {
    if (!path)
        return false;

    const char* dot = strrchr(path, '.');
    return dot && strcasecmp(dot, ".zip") == 0;
}

/*
 * Detect a split archive companion segment and reject if found
 *
 * Policy
 * - If the user provides "foo.zip", check for "foo.z01"
 * - If "foo.z01" exists, report NOT_IMPLEMENTED because split archives are unsupported
 *
 * Return values
 * - ZU_STATUS_OK when no split segment is detected
 * - ZU_STATUS_NOT_IMPLEMENTED when split segment exists
 * - ZU_STATUS_IO when a filesystem error occurs other than ENOENT
 */
static int check_for_split_archive(const char* path) {
    if (!has_zip_suffix(path))
        return ZU_STATUS_OK;

    size_t path_len = strlen(path);
    if (path_len < 4)
        return ZU_STATUS_OK;

    size_t base_len = path_len - 4;

    char buf[PATH_MAX];
    int n = snprintf(buf, sizeof(buf), "%.*s.z01", (int)base_len, path);
    if (n <= 0 || n >= (int)sizeof(buf))
        return ZU_STATUS_IO;

    struct stat st;
    if (stat(buf, &st) == 0)
        return ZU_STATUS_NOT_IMPLEMENTED;

    if (errno != ENOENT)
        return ZU_STATUS_IO;

    return ZU_STATUS_OK;
}

/*
 * Open an archive for reading and attach it to ctx->in_file
 *
 * Behavior
 * - Closes any previously open input stream in ctx
 * - Rejects split archives early (foo.zip + foo.z01)
 * - Opens the file in binary mode
 *
 * Error reporting
 * - On failure, ctx->last_error and ctx->error_msg are updated via zu_context_set_error
 */
int zu_open_input(ZContext* ctx, const char* path) {
    if (!ctx || !path)
        return ZU_STATUS_USAGE;

    close_file(&ctx->in_file);

    int rc = check_for_split_archive(path);
    if (rc == ZU_STATUS_NOT_IMPLEMENTED) {
        zu_context_set_error(ctx, rc, "split archives are not supported");
        return rc;
    }
    if (rc != ZU_STATUS_OK) {
        zu_context_set_error(ctx, rc, "split detection failed");
        return rc;
    }

    if (strcmp(path, "-") == 0) {
        FILE* tmp = tmpfile();
        if (!tmp) {
            zu_context_set_error(ctx, ZU_STATUS_IO, "failed to create temp file for stdin");
            return ZU_STATUS_IO;
        }

        uint8_t buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0) {
            if (fwrite(buf, 1, n, tmp) != n) {
                fclose(tmp);
                zu_context_set_error(ctx, ZU_STATUS_IO, "write to temp file failed");
                return ZU_STATUS_IO;
            }
        }

        if (ferror(stdin)) {
            fclose(tmp);
            zu_context_set_error(ctx, ZU_STATUS_IO, "read from stdin failed");
            return ZU_STATUS_IO;
        }

        rewind(tmp);
        ctx->in_file = tmp;
        return ZU_STATUS_OK;
    }

    ctx->in_file = fopen(path, "rb");
    if (!ctx->in_file) {
        char buf[128];
        snprintf(buf, sizeof(buf), "open input '%s': %s", path, strerror(errno));
        zu_context_set_error(ctx, ZU_STATUS_IO, buf);
        return ZU_STATUS_IO;
    }
    setvbuf(ctx->in_file, NULL, _IOFBF, 64 * 1024);

    return ZU_STATUS_OK;
}

/*
 * Open an archive output stream and attach it to ctx->out_file
 *
 * Behavior
 * - Closes any previously open output stream in ctx
 * - Opens the file using the supplied mode, defaulting to "wb"
 *
 * Error reporting
 * - On failure, ctx->last_error and ctx->error_msg are updated via zu_context_set_error
 */
int zu_open_output(ZContext* ctx, const char* path, const char* mode) {
    if (!ctx || !path)
        return ZU_STATUS_USAGE;

    close_file(&ctx->out_file);

    ctx->out_file = fopen(path, mode ? mode : "wb");
    if (!ctx->out_file) {
        char buf[128];
        snprintf(buf, sizeof(buf), "open output '%s': %s", path, strerror(errno));
        zu_context_set_error(ctx, ZU_STATUS_IO, buf);
        return ZU_STATUS_IO;
    }
    setvbuf(ctx->out_file, NULL, _IOFBF, 64 * 1024);

    return ZU_STATUS_OK;
}

/*
 * Close any archive input/output streams associated with a context
 *
 * Safe to call multiple times
 */
void zu_close_files(ZContext* ctx) {
    if (!ctx)
        return;

    close_file(&ctx->in_file);
    close_file(&ctx->out_file);
}
