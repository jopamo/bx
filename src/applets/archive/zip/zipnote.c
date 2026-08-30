#define _GNU_SOURCE

#include "zipnote.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli_common.h"
#include "comments.h"
#include "ziputils.h"

/* --- Zipnote Helpers --- */

/*
 * Special pseudo-entry label used by zipnote streams to represent the archive comment
 *
 * Zipnote output is an edit script format
 * - Each entry begins with "@ <name>"
 * - Entry comment lines follow until the next "@ ..." marker
 * - The archive comment uses this sentinel name
 */
static const char* zipnote_archive_label = "(zip file comment below this line)";

/*
 * Emit a zipnote comment block to stdout
 *
 * Rules
 * - Empty comment prints a blank line so the marker delimiters remain valid
 * - Lines starting with '@' must be escaped by prefixing an extra '@'
 * - The input may or may not end with '\n', we always emit newline-delimited output
 */
static void zipnote_emit_comment(const char* data, size_t len) {
    if (!data || len == 0) {
        printf("\n");
        return;
    }

    size_t i = 0;
    while (i < len) {
        size_t line_end = i;
        while (line_end < len && data[line_end] != '\n')
            line_end++;

        size_t line_len = line_end - i;

        if (line_len > 0 && data[i] == '@')
            putchar('@');

        fwrite(data + i, 1, line_len, stdout);
        putchar('\n');

        i = line_end + 1;
    }
}

/*
 * Print the zipnote edit-script representation of the archive to stdout
 *
 * Output format
 * - For each entry:
 *   - "@ <entry-name>"
 *   - comment body
 *   - "@"
 * - Then the archive comment using zipnote_archive_label
 *
 * Requires central directory to be loaded so ctx->existing_entries and ctx->zip_comment are valid
 */
int zu_zipnote_list(ZContext* ctx) {
    int rc = zu_comments_load(ctx);
    if (rc != ZU_STATUS_OK)
        return rc;

    for (size_t i = 0; i < ctx->existing_entries.len; ++i) {
        zu_existing_entry* e = (zu_existing_entry*)ctx->existing_entries.items[i];
        printf("@ %s\n", e->name);
        zipnote_emit_comment(e->comment, e->comment_len);
        printf("@\n");
    }

    printf("@ %s\n", zipnote_archive_label);
    zipnote_emit_comment(ctx->zip_comment, ctx->zip_comment_len);
    printf("@\n");

    return ZU_STATUS_OK;
}

/*
 * In-memory representation of one zipnote edit record
 * - name identifies the entry or the archive label
 * - comment is an owned buffer containing the replacement comment text
 * - is_archive marks whether this edit targets the archive comment
 */
typedef struct {
    char* name;
    char* comment;
    size_t comment_len;
    bool is_archive;
} zipnote_edit;

/*
 * Free a zipnote_edit and its owned buffers
 * - Safe to call on partially-populated edits
 */
static void zipnote_edit_free(zipnote_edit* e) {
    if (!e)
        return;
    free(e->name);
    free(e->comment);
    free(e);
}

/*
 * Append a zipnote_edit pointer to a growable vector
 * - edits is a heap array of pointers
 * - len and cap track current length and capacity
 */
static int zipnote_add_edit(zipnote_edit*** edits, size_t* len, size_t* cap, zipnote_edit* e) {
    if (*len == *cap) {
        size_t new_cap = *cap == 0 ? 8 : *cap * 2;
        zipnote_edit** np = realloc(*edits, new_cap * sizeof(zipnote_edit*));
        if (!np)
            return ZU_STATUS_OOM;
        *edits = np;
        *cap = new_cap;
    }
    (*edits)[(*len)++] = e;
    return ZU_STATUS_OK;
}

/*
 * Parse a zipnote edit-script from stdin
 *
 * Input format summary
 * - A marker line starts with "@ " followed by the entry name
 * - Lines after a marker are comment lines for that entry until the next marker
 * - A literal '@' at the start of a comment line is encoded as "@@"
 *
 * Output
 * - Builds an array of zipnote_edit records (edits_out, edits_len_out)
 *
 * Notes
 * - Unknown markers or malformed sections are skipped conservatively
 * - This parser materializes comment bodies with '\n' separators to preserve original formatting
 */
static int zipnote_parse(ZContext* ctx, zipnote_edit*** edits_out, size_t* edits_len_out) {
    (void)ctx;

    char* line = NULL;
    size_t line_cap = 0;
    ssize_t got;

    zipnote_edit** edits = NULL;
    size_t edits_len = 0, edits_cap = 0;

    char* cur_name = NULL;
    char* comment_buf = NULL;
    size_t comment_len = 0, comment_cap = 0;

    while ((got = getline(&line, &line_cap, stdin)) != -1) {
        if (got > 0 && line[got - 1] == '\n') {
            line[got - 1] = '\0';
            got--;
        }

        // Marker line begins a new record, finalize the previous record first
        if (line[0] == '@' && line[1] != '@') {
            if (cur_name) {
                zipnote_edit* e = calloc(1, sizeof(zipnote_edit));
                if (!e)
                    goto oom_error;

                e->name = cur_name;
                e->comment = comment_buf;
                e->comment_len = comment_len;
                e->is_archive = (strcmp(cur_name, zipnote_archive_label) == 0);

                if (zipnote_add_edit(&edits, &edits_len, &edits_cap, e) != ZU_STATUS_OK) {
                    zipnote_edit_free(e);
                    goto oom_error;
                }

                cur_name = NULL;
                comment_buf = NULL;
                comment_len = 0;
                comment_cap = 0;
            }

            const char* name = line + 1;
            while (*name == ' ')
                name++;

            // "@\n" is a section terminator in zipnote output, ignore empty marker names
            if (*name == '\0') {
                free(cur_name);
                cur_name = NULL;
                continue;
            }

            cur_name = strdup(name);
            comment_len = 0;
            comment_cap = 0;
            free(comment_buf);
            comment_buf = NULL;
            continue;
        }

        // Comment data line, unescape "@@" to "@"
        const char* data = line;
        size_t data_len = (size_t)got;
        if (line[0] == '@' && line[1] == '@') {
            data = line + 1;
            data_len -= 1;
        }

        // Ignore comment lines until we have a current marker name
        if (!cur_name)
            continue;

        // Ensure capacity and append data plus newline
        if (comment_len + data_len + 1 > comment_cap) {
            size_t new_cap = comment_cap == 0 ? 256 : comment_cap * 2;
            while (new_cap < comment_len + data_len + 1)
                new_cap *= 2;
            char* nb = realloc(comment_buf, new_cap);
            if (!nb)
                goto oom_error;
            comment_buf = nb;
            comment_cap = new_cap;
        }

        memcpy(comment_buf + comment_len, data, data_len);
        comment_len += data_len;
        comment_buf[comment_len++] = '\n';
    }

    // Finalize trailing record if the stream ended without another marker
    if (cur_name) {
        zipnote_edit* e = calloc(1, sizeof(zipnote_edit));
        if (!e)
            goto oom_error;

        e->name = cur_name;
        e->comment = comment_buf;
        e->comment_len = comment_len;
        e->is_archive = (strcmp(cur_name, zipnote_archive_label) == 0);

        if (zipnote_add_edit(&edits, &edits_len, &edits_cap, e) != ZU_STATUS_OK) {
            zipnote_edit_free(e);
            goto oom_error;
        }

        cur_name = NULL;
        comment_buf = NULL;
    }

    free(line);
    *edits_out = edits;
    *edits_len_out = edits_len;
    return ZU_STATUS_OK;

oom_error:
    free(line);
    free(comment_buf);
    free(cur_name);
    for (size_t i = 0; i < edits_len; ++i)
        zipnote_edit_free(edits[i]);
    free(edits);
    return ZU_STATUS_OOM;
}

/*
 * Apply zipnote edits read from stdin to the archive and rewrite the central directory
 *
 * Flow
 * - Load the central directory
 * - Parse stdin edits into a list
 * - For each edit:
 *   - If it targets the archive comment, replace ctx->zip_comment
 *   - Otherwise locate the entry and replace its comment buffer
 * - Mark changed entries so the writer updates comment fields
 * - Call zu_modify_archive to persist changes
 *
 * Behavior notes
 * - Unknown entry names are warned and ignored
 * - If no archive edit is present, archive comment changes are not applied
 */
int zu_zipnote_apply(ZContext* ctx, const char* tool_name) {
    int rc = zu_comments_load(ctx);
    if (rc != ZU_STATUS_OK)
        return rc;

    zipnote_edit** edits = NULL;
    size_t edits_len = 0;
    rc = zipnote_parse(ctx, &edits, &edits_len);
    if (rc != ZU_STATUS_OK)
        return rc;

    bool seen_archive = false;
    for (size_t i = 0; i < edits_len; ++i) {
        zipnote_edit* e = edits[i];

        if (e->is_archive) {
            rc = zu_comments_replace_archive_owned(
                ctx, &e->comment, e->comment_len);
            if (rc != ZU_STATUS_OK)
                break;
            seen_archive = true;
            continue;
        }

        bool found = false;
        rc = zu_comments_replace_entry_owned(
            ctx, e->name, &e->comment, e->comment_len, &found);
        if (rc != ZU_STATUS_OK)
            break;
        if (!found) {
            zu_cli_warn(tool_name, "zipnote: entry not found: %s", e->name);
            continue;
        }
    }

    for (size_t i = 0; i < edits_len; ++i)
        zipnote_edit_free(edits[i]);
    free(edits);

    if (rc != ZU_STATUS_OK)
        return rc;
    return zu_comments_commit(ctx, seen_archive);
}
