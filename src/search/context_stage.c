#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "context_stage.h"

bool bx_search_staged_record_write(FILE *records,
                                   const struct bx_search_staged_record *record,
                                   const char *text) {
    return fwrite(record, sizeof(*record), 1u, records) == 1u
        && (record->len == 0u || fwrite(text, 1u, record->len, records) == record->len);
}

int bx_search_staged_record_read(FILE *records,
                                 struct bx_search_staged_record *record,
                                 char *text) {
    size_t nread = fread(record, sizeof(*record), 1u, records);

    if (nread == 0u)
        return ferror(records) ? -1 : 0;
    if (text && record->len > 0u && fread(text, 1u, record->len, records) != record->len)
        return -1;
    if (!text && record->len > 0u &&
        fseeko(records, (off_t)record->len, SEEK_CUR) != 0) {
        return -1;
    }
    return 1;
}

bool bx_search_staged_rewind(FILE *stream) {
    return fflush(stream) == 0 && fseeko(stream, 0, SEEK_SET) == 0;
}

static bool bx_search_context_interval_write(
    FILE *intervals,
    const struct bx_search_context_interval *interval
) {
    return fwrite(interval, sizeof(*interval), 1u, intervals) == 1u;
}

int bx_search_context_interval_read(
    FILE *intervals,
    struct bx_search_context_interval *interval
) {
    size_t nread = fread(interval, sizeof(*interval), 1u, intervals);

    if (nread == 1u)
        return 1;
    return ferror(intervals) ? -1 : 0;
}

bool bx_search_context_stage_build_intervals(FILE *records,
                                             FILE *intervals,
                                             size_t record_count,
                                             int before_context,
                                             int after_context) {
    struct bx_search_staged_record record;
    struct bx_search_context_interval pending = {0};
    bool have_pending = false;
    int read_rc;

    if (!bx_search_staged_rewind(records))
        return false;

    while ((read_rc = bx_search_staged_record_read(records, &record, NULL)) > 0) {
        struct bx_search_context_interval next;

        if (!record.selected)
            continue;
        next.first = record.line_number > (size_t)before_context
            ? record.line_number - (size_t)before_context
            : 1u;
        next.last = record.line_number;
        if ((size_t)after_context <= record_count - record.line_number)
            next.last += (size_t)after_context;
        else
            next.last = record_count;

        if (!have_pending) {
            pending = next;
            have_pending = true;
        } else if (next.first <= pending.last ||
                   (pending.last < SIZE_MAX && next.first == pending.last + 1u)) {
            if (next.last > pending.last)
                pending.last = next.last;
        } else {
            if (!bx_search_context_interval_write(intervals, &pending))
                return false;
            pending = next;
        }
    }
    if (read_rc < 0)
        return false;
    if (have_pending && !bx_search_context_interval_write(intervals, &pending))
        return false;
    return bx_search_staged_rewind(intervals);
}
