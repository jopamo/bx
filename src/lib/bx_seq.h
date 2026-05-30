#ifndef BX_LIB_BX_SEQ_H
#define BX_LIB_BX_SEQ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lib/line_writer.h"

/*
 * bx_seq is a small structured-record layer over bx_line_writer.
 *
 * It owns record/field framing only. It does not own output policy, quoting,
 * terminal handling, JSON/table formatting, ordering, or publication. Those
 * adapters should build records here and let bx_line_writer remain the only
 * low-level write aggregation primitive.
 */
struct bx_seq {
    struct bx_line_writer *writer;
    const char *field_separator;
    size_t field_separator_len;
    char record_terminator;
    bool record_open;
    bool first_field;
    int error;
};

void bx_seq_init(struct bx_seq *seq, struct bx_line_writer *writer);
void bx_seq_init_with_separator(struct bx_seq *seq,
                                struct bx_line_writer *writer,
                                const char *field_separator,
                                size_t field_separator_len,
                                char record_terminator);
bool bx_seq_begin_record(struct bx_seq *seq);
bool bx_seq_field_bytes(struct bx_seq *seq, const void *data, size_t length);
bool bx_seq_field_cstr(struct bx_seq *seq, const char *text);
bool bx_seq_field_u64(struct bx_seq *seq, uint64_t value);
bool bx_seq_field_i64(struct bx_seq *seq, int64_t value);
bool bx_seq_end_record(struct bx_seq *seq);
bool bx_seq_flush(struct bx_seq *seq);
int bx_seq_error(const struct bx_seq *seq);

#endif /* BX_LIB_BX_SEQ_H */
