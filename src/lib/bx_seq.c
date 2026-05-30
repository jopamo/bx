#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "lib/bx_seq.h"

static bool bx_seq_fail_errno(struct bx_seq *seq, int errnum) {
    if (errnum == 0) {
        errnum = EIO;
    }
    if (seq) {
        seq->error = errnum;
    }
    errno = errnum;
    return false;
}

static bool bx_seq_failed(struct bx_seq *seq) {
    if (seq && seq->error != 0) {
        errno = seq->error;
    }
    return false;
}

static bool bx_seq_valid(struct bx_seq *seq) {
    if (!seq || !seq->writer) {
        errno = EINVAL;
        return false;
    }
    if (seq->error != 0) {
        return bx_seq_failed(seq);
    }
    if (bx_line_writer_error(seq->writer) != 0) {
        return bx_seq_fail_errno(seq, bx_line_writer_error(seq->writer));
    }
    return true;
}

static bool bx_seq_writer_write(struct bx_seq *seq, const void *data, size_t length) {
    if (length == 0u) {
        return true;
    }
    if (!bx_line_writer_write(seq->writer, data, length)) {
        int errnum = bx_line_writer_error(seq->writer);
        if (errnum == 0) {
            errnum = errno;
        }
        return bx_seq_fail_errno(seq, errnum);
    }
    return true;
}

static bool bx_seq_write_field_prefix(struct bx_seq *seq) {
    if (seq->first_field) {
        seq->first_field = false;
        return true;
    }
    return bx_seq_writer_write(seq, seq->field_separator, seq->field_separator_len);
}

void bx_seq_init_with_separator(struct bx_seq *seq,
                                struct bx_line_writer *writer,
                                const char *field_separator,
                                size_t field_separator_len,
                                char record_terminator) {
    if (!seq) {
        return;
    }

    seq->writer = writer;
    seq->field_separator = field_separator_len == 0u ? "" : field_separator;
    seq->field_separator_len = field_separator_len;
    seq->record_terminator = record_terminator;
    seq->record_open = false;
    seq->first_field = true;
    seq->error = 0;

    if (!writer || (!field_separator && field_separator_len > 0u)) {
        seq->error = EINVAL;
    }
}

void bx_seq_init(struct bx_seq *seq, struct bx_line_writer *writer) {
    bx_seq_init_with_separator(seq, writer, "\t", 1u, '\n');
}

bool bx_seq_begin_record(struct bx_seq *seq) {
    if (!bx_seq_valid(seq)) {
        return false;
    }
    if (seq->record_open) {
        return bx_seq_fail_errno(seq, EINVAL);
    }
    seq->record_open = true;
    seq->first_field = true;
    return true;
}

bool bx_seq_field_bytes(struct bx_seq *seq, const void *data, size_t length) {
    if (!bx_seq_valid(seq)) {
        return false;
    }
    if (!seq->record_open || (!data && length > 0u)) {
        return bx_seq_fail_errno(seq, EINVAL);
    }
    return bx_seq_write_field_prefix(seq) && bx_seq_writer_write(seq, data, length);
}

bool bx_seq_field_cstr(struct bx_seq *seq, const char *text) {
    if (!text) {
        return bx_seq_fail_errno(seq, EINVAL);
    }
    return bx_seq_field_bytes(seq, text, strlen(text));
}

bool bx_seq_field_u64(struct bx_seq *seq, uint64_t value) {
    char buffer[32];
    int len = snprintf(buffer, sizeof(buffer), "%" PRIu64, value);

    if (len < 0 || (size_t)len >= sizeof(buffer)) {
        return bx_seq_fail_errno(seq, EOVERFLOW);
    }
    return bx_seq_field_bytes(seq, buffer, (size_t)len);
}

bool bx_seq_field_i64(struct bx_seq *seq, int64_t value) {
    char buffer[32];
    int len = snprintf(buffer, sizeof(buffer), "%" PRId64, value);

    if (len < 0 || (size_t)len >= sizeof(buffer)) {
        return bx_seq_fail_errno(seq, EOVERFLOW);
    }
    return bx_seq_field_bytes(seq, buffer, (size_t)len);
}

bool bx_seq_end_record(struct bx_seq *seq) {
    if (!bx_seq_valid(seq)) {
        return false;
    }
    if (!seq->record_open) {
        return bx_seq_fail_errno(seq, EINVAL);
    }
    if (!bx_seq_writer_write(seq, &seq->record_terminator, 1u)) {
        return false;
    }
    seq->record_open = false;
    seq->first_field = true;
    return true;
}

bool bx_seq_flush(struct bx_seq *seq) {
    if (!bx_seq_valid(seq)) {
        return false;
    }
    if (seq->record_open) {
        return bx_seq_fail_errno(seq, EINVAL);
    }
    if (!bx_line_writer_flush(seq->writer)) {
        int errnum = bx_line_writer_error(seq->writer);
        if (errnum == 0) {
            errnum = errno;
        }
        return bx_seq_fail_errno(seq, errnum);
    }
    return true;
}

int bx_seq_error(const struct bx_seq *seq) {
    if (!seq) {
        return EINVAL;
    }
    if (seq->error != 0) {
        return seq->error;
    }
    return bx_line_writer_error(seq->writer);
}
