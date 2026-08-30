#ifndef ZU_COMMENTS_H
#define ZU_COMMENTS_H

#include <stdbool.h>
#include <stddef.h>

#include "ctx.h"

int zu_comments_load(ZContext* ctx);
int zu_comments_replace_entry_owned(ZContext* ctx,
                                    const char* name,
                                    char** comment,
                                    size_t comment_len,
                                    bool* found);
int zu_comments_replace_archive_owned(ZContext* ctx,
                                      char** comment,
                                      size_t comment_len);
int zu_comments_commit(ZContext* ctx, bool archive_comment_present);

#endif /* ZU_COMMENTS_H */
