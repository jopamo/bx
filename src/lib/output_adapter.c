#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib/output_adapter.h"
#include "lib/output_profile_counter.h"

#define BX_OUTPUT_COLOR_RESET "\033[0m"

struct bx_output_ordered_record {
    uint64_t seq;
    void *payload;
};

static bool bx_output_fail_errno(int *error_slot, int errnum) {
    if (errnum == 0) {
        errnum = EIO;
    }
    if (error_slot) {
        *error_slot = errnum;
    }
    errno = errnum;
    return false;
}

static bool bx_output_failed(const int *error_slot) {
    if (error_slot && *error_slot != 0) {
        errno = *error_slot;
    }
    return false;
}

static int bx_output_writer_error(struct bx_line_writer *writer) {
    int errnum = writer ? bx_line_writer_error(writer) : EINVAL;

    if (errnum == 0) {
        errnum = errno;
    }
    return errnum == 0 ? EIO : errnum;
}

static bool bx_output_writer_valid(struct bx_line_writer *writer, int *error_slot) {
    if (!writer) {
        return bx_output_fail_errno(error_slot, EINVAL);
    }
    if (error_slot && *error_slot != 0) {
        return bx_output_failed(error_slot);
    }
    if (bx_line_writer_error(writer) != 0) {
        return bx_output_fail_errno(error_slot, bx_line_writer_error(writer));
    }
    return true;
}

static bool bx_output_writer_write(struct bx_line_writer *writer,
                                   int *error_slot,
                                   const void *data,
                                   size_t length) {
    if (length == 0u) {
        return true;
    }
    if (!bx_line_writer_write(writer, data, length)) {
        return bx_output_fail_errno(error_slot, bx_output_writer_error(writer));
    }
    return true;
}

static bool bx_output_writer_putc(struct bx_line_writer *writer, int *error_slot, char ch) {
    return bx_output_writer_write(writer, error_slot, &ch, 1u);
}

static bool bx_output_writer_puts(struct bx_line_writer *writer, int *error_slot, const char *text) {
    if (!text) {
        return bx_output_fail_errno(error_slot, EINVAL);
    }
    return bx_output_writer_write(writer, error_slot, text, strlen(text));
}

void bx_output_raw_adapter_init(struct bx_output_raw_adapter *adapter,
                                struct bx_line_writer *writer) {
    if (!adapter) {
        return;
    }
    adapter->writer = writer;
    adapter->profile = NULL;
    adapter->error = writer ? 0 : EINVAL;
}

void bx_output_raw_adapter_set_profile(struct bx_output_raw_adapter *adapter,
                                       struct bx_output_profile_sink *profile) {
    if (!adapter) {
        return;
    }
    adapter->profile = profile;
    bx_line_writer_set_profile(adapter->writer, profile);
}

bool bx_output_raw_write(struct bx_output_raw_adapter *adapter, const void *data, size_t length) {
    if (!adapter || (!data && length > 0u)) {
        return bx_output_fail_errno(adapter ? &adapter->error : NULL, EINVAL);
    }
    if (!bx_output_writer_valid(adapter->writer, &adapter->error)) {
        return false;
    }
    return bx_output_writer_write(adapter->writer, &adapter->error, data, length);
}

bool bx_output_raw_flush(struct bx_output_raw_adapter *adapter) {
    if (!adapter || !bx_output_writer_valid(adapter->writer, adapter ? &adapter->error : NULL)) {
        return false;
    }
    if (!bx_line_writer_flush(adapter->writer)) {
        return bx_output_fail_errno(&adapter->error, bx_output_writer_error(adapter->writer));
    }
    return true;
}

int bx_output_raw_error(const struct bx_output_raw_adapter *adapter) {
    if (!adapter) {
        return EINVAL;
    }
    if (adapter->error != 0) {
        return adapter->error;
    }
    return bx_line_writer_error(adapter->writer);
}

void bx_output_line_adapter_init(struct bx_output_line_adapter *adapter,
                                 struct bx_line_writer *writer) {
    if (!adapter) {
        return;
    }
    bx_seq_init_with_separator(&adapter->seq, writer, "", 0u, '\n');
    adapter->profile = NULL;
}

void bx_output_nul_adapter_init(struct bx_output_line_adapter *adapter,
                                struct bx_line_writer *writer) {
    if (!adapter) {
        return;
    }
    bx_seq_init_with_separator(&adapter->seq, writer, "", 0u, '\0');
    adapter->profile = NULL;
}

void bx_output_line_adapter_set_profile(struct bx_output_line_adapter *adapter,
                                        struct bx_output_profile_sink *profile) {
    if (!adapter) {
        return;
    }
    adapter->profile = profile;
    bx_line_writer_set_profile(adapter->seq.writer, profile);
}

bool bx_output_line_bytes(struct bx_output_line_adapter *adapter, const void *data, size_t length) {
    bool ok = adapter
        && bx_seq_begin_record(&adapter->seq)
        && bx_seq_field_bytes(&adapter->seq, data, length)
        && bx_seq_end_record(&adapter->seq);

    if (ok && adapter->profile != NULL) {
        bx_output_profile_note_record(adapter->profile);
    }
    return ok;
}

bool bx_output_line_cstr(struct bx_output_line_adapter *adapter, const char *text) {
    bool ok = adapter
        && bx_seq_begin_record(&adapter->seq)
        && bx_seq_field_cstr(&adapter->seq, text)
        && bx_seq_end_record(&adapter->seq);

    if (ok && adapter->profile != NULL) {
        bx_output_profile_note_record(adapter->profile);
    }
    return ok;
}

bool bx_output_line_flush(struct bx_output_line_adapter *adapter) {
    return adapter && bx_seq_flush(&adapter->seq);
}

int bx_output_line_error(const struct bx_output_line_adapter *adapter) {
    return adapter ? bx_seq_error(&adapter->seq) : EINVAL;
}

void bx_output_table_adapter_init(struct bx_output_table_adapter *adapter,
                                  struct bx_line_writer *writer) {
    if (!adapter) {
        return;
    }
    bx_seq_init(&adapter->seq, writer);
    adapter->profile = NULL;
}

void bx_output_table_adapter_set_profile(struct bx_output_table_adapter *adapter,
                                         struct bx_output_profile_sink *profile) {
    if (!adapter) {
        return;
    }
    adapter->profile = profile;
    bx_line_writer_set_profile(adapter->seq.writer, profile);
}

bool bx_output_table_begin_row(struct bx_output_table_adapter *adapter) {
    return adapter && bx_seq_begin_record(&adapter->seq);
}

bool bx_output_table_field_bytes(struct bx_output_table_adapter *adapter,
                                 const void *data,
                                 size_t length) {
    return adapter && bx_seq_field_bytes(&adapter->seq, data, length);
}

bool bx_output_table_field_cstr(struct bx_output_table_adapter *adapter, const char *text) {
    return adapter && bx_seq_field_cstr(&adapter->seq, text);
}

bool bx_output_table_field_u64(struct bx_output_table_adapter *adapter, uint64_t value) {
    uint_fast64_t start_ns = (adapter && adapter->profile != NULL)
        ? bx_output_profile_format_begin(adapter->profile)
        : 0u;
    bool ok = adapter && bx_seq_field_u64(&adapter->seq, value);

    if (adapter && adapter->profile != NULL) {
        bx_output_profile_format_end(adapter->profile, start_ns);
    }
    return ok;
}

bool bx_output_table_field_i64(struct bx_output_table_adapter *adapter, int64_t value) {
    uint_fast64_t start_ns = (adapter && adapter->profile != NULL)
        ? bx_output_profile_format_begin(adapter->profile)
        : 0u;
    bool ok = adapter && bx_seq_field_i64(&adapter->seq, value);

    if (adapter && adapter->profile != NULL) {
        bx_output_profile_format_end(adapter->profile, start_ns);
    }
    return ok;
}

bool bx_output_table_end_row(struct bx_output_table_adapter *adapter) {
    bool ok = adapter && bx_seq_end_record(&adapter->seq);

    if (ok && adapter->profile != NULL) {
        bx_output_profile_note_record(adapter->profile);
    }
    return ok;
}

bool bx_output_table_flush(struct bx_output_table_adapter *adapter) {
    return adapter && bx_seq_flush(&adapter->seq);
}

int bx_output_table_error(const struct bx_output_table_adapter *adapter) {
    return adapter ? bx_seq_error(&adapter->seq) : EINVAL;
}

void bx_output_json_adapter_init(struct bx_output_json_adapter *adapter,
                                 struct bx_line_writer *writer) {
    if (!adapter) {
        return;
    }
    adapter->writer = writer;
    adapter->profile = NULL;
    adapter->object_open = false;
    adapter->first_field = true;
    adapter->error = writer ? 0 : EINVAL;
}

void bx_output_json_adapter_set_profile(struct bx_output_json_adapter *adapter,
                                        struct bx_output_profile_sink *profile) {
    if (!adapter) {
        return;
    }
    adapter->profile = profile;
    bx_line_writer_set_profile(adapter->writer, profile);
}

static bool bx_output_json_valid(struct bx_output_json_adapter *adapter) {
    if (!adapter) {
        return bx_output_fail_errno(NULL, EINVAL);
    }
    return bx_output_writer_valid(adapter->writer, &adapter->error);
}

static bool bx_output_json_write_hex_escape(struct bx_output_json_adapter *adapter, unsigned char ch) {
    static const char hex[] = "0123456789abcdef";
    char escape[] = {'\\', 'u', '0', '0', hex[(ch >> 4u) & 0xfu], hex[ch & 0xfu]};

    return bx_output_writer_write(adapter->writer, &adapter->error, escape, sizeof(escape));
}

static bool bx_output_json_write_string_len(struct bx_output_json_adapter *adapter,
                                            const void *data,
                                            size_t length) {
    const unsigned char *bytes = data;
    size_t start = 0u;

    if (!data && length > 0u) {
        return bx_output_fail_errno(&adapter->error, EINVAL);
    }
    if (!bx_output_writer_putc(adapter->writer, &adapter->error, '"')) {
        return false;
    }
    for (size_t index = 0u; index < length; index++) {
        const unsigned char ch = bytes[index];
        const char *escape = NULL;
        size_t escape_len = 0u;

        switch (ch) {
            case '"':
                escape = "\\\"";
                escape_len = 2u;
                break;
            case '\\':
                escape = "\\\\";
                escape_len = 2u;
                break;
            case '\b':
                escape = "\\b";
                escape_len = 2u;
                break;
            case '\f':
                escape = "\\f";
                escape_len = 2u;
                break;
            case '\n':
                escape = "\\n";
                escape_len = 2u;
                break;
            case '\r':
                escape = "\\r";
                escape_len = 2u;
                break;
            case '\t':
                escape = "\\t";
                escape_len = 2u;
                break;
            default:
                if (ch < 0x20u) {
                    if (index > start
                        && !bx_output_writer_write(adapter->writer,
                                                   &adapter->error,
                                                   bytes + start,
                                                   index - start)) {
                        return false;
                    }
                    if (!bx_output_json_write_hex_escape(adapter, ch)) {
                        return false;
                    }
                    start = index + 1u;
                }
                continue;
        }

        if (index > start
            && !bx_output_writer_write(adapter->writer, &adapter->error, bytes + start, index - start)) {
            return false;
        }
        if (!bx_output_writer_write(adapter->writer, &adapter->error, escape, escape_len)) {
            return false;
        }
        start = index + 1u;
    }
    if (length > start
        && !bx_output_writer_write(adapter->writer, &adapter->error, bytes + start, length - start)) {
        return false;
    }
    return bx_output_writer_putc(adapter->writer, &adapter->error, '"');
}

bool bx_output_json_begin_object(struct bx_output_json_adapter *adapter) {
    if (!bx_output_json_valid(adapter)) {
        return false;
    }
    if (adapter->object_open) {
        return bx_output_fail_errno(&adapter->error, EINVAL);
    }
    if (!bx_output_writer_putc(adapter->writer, &adapter->error, '{')) {
        return false;
    }
    adapter->object_open = true;
    adapter->first_field = true;
    return true;
}

static bool bx_output_json_field_prefix(struct bx_output_json_adapter *adapter, const char *key) {
    if (!bx_output_json_valid(adapter)) {
        return false;
    }
    if (!adapter->object_open || !key) {
        return bx_output_fail_errno(&adapter->error, EINVAL);
    }
    if (!adapter->first_field && !bx_output_writer_putc(adapter->writer, &adapter->error, ',')) {
        return false;
    }
    adapter->first_field = false;
    return bx_output_json_write_string_len(adapter, key, strlen(key))
        && bx_output_writer_putc(adapter->writer, &adapter->error, ':');
}

bool bx_output_json_field_string_len(struct bx_output_json_adapter *adapter,
                                     const char *key,
                                     const void *value,
                                     size_t value_len) {
    uint_fast64_t start_ns = (adapter && adapter->profile != NULL)
        ? bx_output_profile_format_begin(adapter->profile)
        : 0u;
    bool ok = bx_output_json_field_prefix(adapter, key)
        && bx_output_json_write_string_len(adapter, value, value_len);

    if (adapter && adapter->profile != NULL) {
        bx_output_profile_format_end(adapter->profile, start_ns);
    }
    return ok;
}

bool bx_output_json_field_string(struct bx_output_json_adapter *adapter,
                                 const char *key,
                                 const char *value) {
    if (!value) {
        return bx_output_fail_errno(adapter ? &adapter->error : NULL, EINVAL);
    }
    return bx_output_json_field_string_len(adapter, key, value, strlen(value));
}

bool bx_output_json_field_u64(struct bx_output_json_adapter *adapter,
                              const char *key,
                              uint64_t value) {
    char buffer[32];
    uint_fast64_t start_ns = (adapter && adapter->profile != NULL)
        ? bx_output_profile_format_begin(adapter->profile)
        : 0u;
    int len = snprintf(buffer, sizeof(buffer), "%" PRIu64, value);
    bool ok;

    if (len < 0 || (size_t)len >= sizeof(buffer)) {
        return bx_output_fail_errno(adapter ? &adapter->error : NULL, EOVERFLOW);
    }
    ok = bx_output_json_field_prefix(adapter, key)
        && bx_output_writer_write(adapter->writer, &adapter->error, buffer, (size_t)len);
    if (adapter && adapter->profile != NULL) {
        bx_output_profile_format_end(adapter->profile, start_ns);
    }
    return ok;
}

bool bx_output_json_field_bool(struct bx_output_json_adapter *adapter,
                               const char *key,
                               bool value) {
    uint_fast64_t start_ns = (adapter && adapter->profile != NULL)
        ? bx_output_profile_format_begin(adapter->profile)
        : 0u;
    bool ok = bx_output_json_field_prefix(adapter, key)
        && bx_output_writer_puts(adapter->writer, &adapter->error, value ? "true" : "false");

    if (adapter && adapter->profile != NULL) {
        bx_output_profile_format_end(adapter->profile, start_ns);
    }
    return ok;
}

bool bx_output_json_field_null(struct bx_output_json_adapter *adapter, const char *key) {
    uint_fast64_t start_ns = (adapter && adapter->profile != NULL)
        ? bx_output_profile_format_begin(adapter->profile)
        : 0u;
    bool ok = bx_output_json_field_prefix(adapter, key)
        && bx_output_writer_puts(adapter->writer, &adapter->error, "null");

    if (adapter && adapter->profile != NULL) {
        bx_output_profile_format_end(adapter->profile, start_ns);
    }
    return ok;
}

bool bx_output_json_end_object(struct bx_output_json_adapter *adapter) {
    if (!bx_output_json_valid(adapter)) {
        return false;
    }
    if (!adapter->object_open) {
        return bx_output_fail_errno(&adapter->error, EINVAL);
    }
    if (!bx_output_writer_putc(adapter->writer, &adapter->error, '}')
        || !bx_output_writer_putc(adapter->writer, &adapter->error, '\n')) {
        return false;
    }
    adapter->object_open = false;
    adapter->first_field = true;
    if (adapter->profile != NULL) {
        bx_output_profile_note_record(adapter->profile);
    }
    return true;
}

bool bx_output_json_flush(struct bx_output_json_adapter *adapter) {
    if (!bx_output_json_valid(adapter)) {
        return false;
    }
    if (adapter->object_open) {
        return bx_output_fail_errno(&adapter->error, EINVAL);
    }
    if (!bx_line_writer_flush(adapter->writer)) {
        return bx_output_fail_errno(&adapter->error, bx_output_writer_error(adapter->writer));
    }
    return true;
}

int bx_output_json_error(const struct bx_output_json_adapter *adapter) {
    if (!adapter) {
        return EINVAL;
    }
    if (adapter->error != 0) {
        return adapter->error;
    }
    return bx_line_writer_error(adapter->writer);
}

void bx_output_color_adapter_init_full(struct bx_output_color_adapter *adapter,
                                       struct bx_line_writer *writer,
                                       bool enabled,
                                       const char *prefix,
                                       size_t prefix_len,
                                       const char *reset,
                                       size_t reset_len,
                                       char record_terminator) {
    if (!adapter) {
        return;
    }
    adapter->writer = writer;
    adapter->profile = NULL;
    adapter->enabled = enabled;
    adapter->prefix = prefix_len == 0u ? "" : prefix;
    adapter->prefix_len = prefix_len;
    adapter->reset = reset_len == 0u ? "" : reset;
    adapter->reset_len = reset_len;
    adapter->record_terminator = record_terminator;
    adapter->error = 0;
    if (!writer || (!prefix && prefix_len > 0u) || (!reset && reset_len > 0u)) {
        adapter->error = EINVAL;
    }
}

void bx_output_color_adapter_init(struct bx_output_color_adapter *adapter,
                                  struct bx_line_writer *writer,
                                  bool enabled,
                                  const char *prefix,
                                  size_t prefix_len) {
    bx_output_color_adapter_init_full(adapter,
                                      writer,
                                      enabled,
                                      prefix,
                                      prefix_len,
                                      BX_OUTPUT_COLOR_RESET,
                                      sizeof(BX_OUTPUT_COLOR_RESET) - 1u,
                                      '\n');
}

void bx_output_color_adapter_set_profile(struct bx_output_color_adapter *adapter,
                                         struct bx_output_profile_sink *profile) {
    if (!adapter) {
        return;
    }
    adapter->profile = profile;
    bx_line_writer_set_profile(adapter->writer, profile);
}

bool bx_output_color_record_bytes(struct bx_output_color_adapter *adapter,
                                  const void *data,
                                  size_t length) {
    if (!adapter || (!data && length > 0u)) {
        return bx_output_fail_errno(adapter ? &adapter->error : NULL, EINVAL);
    }
    if (!bx_output_writer_valid(adapter->writer, &adapter->error)) {
        return false;
    }
    bool ok = (!adapter->enabled
            || bx_output_writer_write(adapter->writer, &adapter->error, adapter->prefix, adapter->prefix_len))
        && bx_output_writer_write(adapter->writer, &adapter->error, data, length)
        && (!adapter->enabled
            || bx_output_writer_write(adapter->writer, &adapter->error, adapter->reset, adapter->reset_len))
        && bx_output_writer_putc(adapter->writer, &adapter->error, adapter->record_terminator);

    if (ok && adapter->profile != NULL) {
        bx_output_profile_note_record(adapter->profile);
    }
    return ok;
}

bool bx_output_color_record_cstr(struct bx_output_color_adapter *adapter, const char *text) {
    if (!text) {
        return bx_output_fail_errno(adapter ? &adapter->error : NULL, EINVAL);
    }
    return bx_output_color_record_bytes(adapter, text, strlen(text));
}

bool bx_output_color_flush(struct bx_output_color_adapter *adapter) {
    if (!adapter || !bx_output_writer_valid(adapter->writer, adapter ? &adapter->error : NULL)) {
        return false;
    }
    if (!bx_line_writer_flush(adapter->writer)) {
        return bx_output_fail_errno(&adapter->error, bx_output_writer_error(adapter->writer));
    }
    return true;
}

int bx_output_color_error(const struct bx_output_color_adapter *adapter) {
    if (!adapter) {
        return EINVAL;
    }
    if (adapter->error != 0) {
        return adapter->error;
    }
    return bx_line_writer_error(adapter->writer);
}

static uint64_t bx_output_ordered_record_seq(const void *record, void *user) {
    const struct bx_output_ordered_record *ordered_record = record;

    (void)user;
    return ordered_record->seq;
}

static void bx_output_ordered_emit_record(void *user, void *record) {
    struct bx_output_ordered_adapter *adapter = user;
    struct bx_output_ordered_record *ordered_record = record;

    adapter->opts.emit_payload(adapter->opts.user, ordered_record->payload);
}

static void bx_output_ordered_dispose_record(void *user, void *record) {
    struct bx_output_ordered_adapter *adapter = user;
    struct bx_output_ordered_record *ordered_record = record;

    if (adapter->opts.dispose_payload) {
        adapter->opts.dispose_payload(adapter->opts.user, ordered_record->payload);
    }
    free(ordered_record);
}

bool bx_output_ordered_adapter_init(struct bx_output_ordered_adapter *adapter,
                                    const struct bx_output_ordered_adapter_opts *opts) {
    if (!adapter) {
        return bx_output_fail_errno(NULL, EINVAL);
    }

    memset(adapter, 0, sizeof(*adapter));
    if (!opts || !opts->emit_payload || opts->max_pending == 0u) {
        return bx_output_fail_errno(&adapter->error, EINVAL);
    }

    adapter->opts = *opts;
    adapter->profile = opts->profile;
    if (!bx_output_publication_init(&adapter->publication,
        &(struct bx_output_publication_opts){
            .mode = BX_OUTPUT_PUBLICATION_ORDERED,
            .max_pending = opts->max_pending,
            .first_seq = opts->first_seq,
            .user = adapter,
            .record_seq = bx_output_ordered_record_seq,
            .emit_record = bx_output_ordered_emit_record,
            .dispose_record = bx_output_ordered_dispose_record,
        })) {
        return bx_output_fail_errno(&adapter->error, errno == 0 ? EIO : errno);
    }
    adapter->initialized = true;
    return true;
}

bool bx_output_ordered_submit(struct bx_output_ordered_adapter *adapter,
                              uint64_t seq,
                              void *payload) {
    struct bx_output_ordered_record *record;

    if (!adapter || !adapter->initialized) {
        return bx_output_fail_errno(adapter ? &adapter->error : NULL, EINVAL);
    }
    if (adapter->error != 0) {
        return bx_output_failed(&adapter->error);
    }

    record = malloc(sizeof(*record));
    if (!record) {
        return bx_output_fail_errno(&adapter->error, errno == 0 ? ENOMEM : errno);
    }
    if (adapter->profile != NULL) {
        bx_output_profile_note_allocation(adapter->profile, sizeof(*record));
    }
    record->seq = seq;
    record->payload = payload;
    if (!bx_output_publication_submit(&adapter->publication, record)) {
        free(record);
        return bx_output_fail_errno(&adapter->error, errno == 0 ? EIO : errno);
    }
    return true;
}

bool bx_output_ordered_skip(struct bx_output_ordered_adapter *adapter, uint64_t seq) {
    if (!adapter || !adapter->initialized) {
        return bx_output_fail_errno(adapter ? &adapter->error : NULL, EINVAL);
    }
    if (!bx_output_publication_skip_seq(&adapter->publication, seq)) {
        return bx_output_fail_errno(&adapter->error, errno == 0 ? EIO : errno);
    }
    return true;
}

void bx_output_ordered_close(struct bx_output_ordered_adapter *adapter) {
    if (!adapter || !adapter->initialized) {
        return;
    }
    bx_output_publication_close(&adapter->publication);
}

bool bx_output_ordered_join(struct bx_output_ordered_adapter *adapter) {
    if (!adapter || !adapter->initialized) {
        return bx_output_fail_errno(adapter ? &adapter->error : NULL, EINVAL);
    }
    if (!bx_output_publication_join(&adapter->publication)) {
        return bx_output_fail_errno(&adapter->error, errno == 0 ? EIO : errno);
    }
    return true;
}

void bx_output_ordered_dispose(struct bx_output_ordered_adapter *adapter) {
    if (!adapter || !adapter->initialized) {
        return;
    }
    bx_output_publication_dispose(&adapter->publication);
    adapter->initialized = false;
}

int bx_output_ordered_error(const struct bx_output_ordered_adapter *adapter) {
    if (!adapter) {
        return EINVAL;
    }
    return adapter->error;
}
