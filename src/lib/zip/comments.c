#include "comments.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "reader.h"
#include "writer.h"
#include "ziputils.h"

int zu_comments_load(ZContext* ctx) {
    if (!ctx)
        return ZU_STATUS_USAGE;
    return zu_load_central_directory(ctx);
}

int zu_comments_replace_entry_owned(ZContext* ctx,
                                    const char* name,
                                    char** comment,
                                    size_t comment_len,
                                    bool* found) {
    if (!ctx || !name || !comment || !found)
        return ZU_STATUS_USAGE;
    if (comment_len > UINT16_MAX)
        return ZU_STATUS_USAGE;

    *found = false;
    for (size_t i = 0; i < ctx->existing_entries.len; ++i) {
        zu_existing_entry* existing =
            (zu_existing_entry*)ctx->existing_entries.items[i];
        if (strcmp(existing->name, name) != 0)
            continue;

        free(existing->comment);
        existing->comment = *comment;
        existing->comment_len = (uint16_t)comment_len;
        existing->changed = true;
        *comment = NULL;
        *found = true;
        return ZU_STATUS_OK;
    }
    return ZU_STATUS_OK;
}

int zu_comments_replace_archive_owned(ZContext* ctx,
                                      char** comment,
                                      size_t comment_len) {
    if (!ctx || !comment)
        return ZU_STATUS_USAGE;
    if (comment_len > UINT16_MAX)
        return ZU_STATUS_USAGE;

    free(ctx->zip_comment);
    ctx->zip_comment = *comment;
    ctx->zip_comment_len = comment_len;
    ctx->zip_comment_specified = true;
    *comment = NULL;
    return ZU_STATUS_OK;
}

int zu_comments_commit(ZContext* ctx, bool archive_comment_present) {
    if (!ctx)
        return ZU_STATUS_USAGE;
    if (!archive_comment_present)
        ctx->zip_comment_specified = false;
    ctx->existing_loaded = true;
    return zu_modify_archive(ctx);
}
