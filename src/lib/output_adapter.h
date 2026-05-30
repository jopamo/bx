#ifndef BX_LIB_OUTPUT_ADAPTER_H
#define BX_LIB_OUTPUT_ADAPTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lib/bx_seq.h"
#include "lib/line_writer.h"
#include "lib/output_publication.h"

struct bx_output_profile_sink;

struct bx_output_raw_adapter {
    struct bx_line_writer *writer;
    struct bx_output_profile_sink *profile;
    int error;
};

void bx_output_raw_adapter_init(struct bx_output_raw_adapter *adapter,
                                struct bx_line_writer *writer);
void bx_output_raw_adapter_set_profile(struct bx_output_raw_adapter *adapter,
                                       struct bx_output_profile_sink *profile);
bool bx_output_raw_write(struct bx_output_raw_adapter *adapter, const void *data, size_t length);
bool bx_output_raw_flush(struct bx_output_raw_adapter *adapter);
int bx_output_raw_error(const struct bx_output_raw_adapter *adapter);

struct bx_output_line_adapter {
    struct bx_seq seq;
    struct bx_output_profile_sink *profile;
};

void bx_output_line_adapter_init(struct bx_output_line_adapter *adapter,
                                 struct bx_line_writer *writer);
void bx_output_nul_adapter_init(struct bx_output_line_adapter *adapter,
                                struct bx_line_writer *writer);
void bx_output_line_adapter_set_profile(struct bx_output_line_adapter *adapter,
                                        struct bx_output_profile_sink *profile);
bool bx_output_line_bytes(struct bx_output_line_adapter *adapter, const void *data, size_t length);
bool bx_output_line_cstr(struct bx_output_line_adapter *adapter, const char *text);
bool bx_output_line_flush(struct bx_output_line_adapter *adapter);
int bx_output_line_error(const struct bx_output_line_adapter *adapter);

struct bx_output_table_adapter {
    struct bx_seq seq;
    struct bx_output_profile_sink *profile;
};

void bx_output_table_adapter_init(struct bx_output_table_adapter *adapter,
                                  struct bx_line_writer *writer);
void bx_output_table_adapter_set_profile(struct bx_output_table_adapter *adapter,
                                         struct bx_output_profile_sink *profile);
bool bx_output_table_begin_row(struct bx_output_table_adapter *adapter);
bool bx_output_table_field_bytes(struct bx_output_table_adapter *adapter,
                                 const void *data,
                                 size_t length);
bool bx_output_table_field_cstr(struct bx_output_table_adapter *adapter, const char *text);
bool bx_output_table_field_u64(struct bx_output_table_adapter *adapter, uint64_t value);
bool bx_output_table_field_i64(struct bx_output_table_adapter *adapter, int64_t value);
bool bx_output_table_end_row(struct bx_output_table_adapter *adapter);
bool bx_output_table_flush(struct bx_output_table_adapter *adapter);
int bx_output_table_error(const struct bx_output_table_adapter *adapter);

struct bx_output_json_adapter {
    struct bx_line_writer *writer;
    struct bx_output_profile_sink *profile;
    bool object_open;
    bool first_field;
    int error;
};

void bx_output_json_adapter_init(struct bx_output_json_adapter *adapter,
                                 struct bx_line_writer *writer);
void bx_output_json_adapter_set_profile(struct bx_output_json_adapter *adapter,
                                        struct bx_output_profile_sink *profile);
bool bx_output_json_begin_object(struct bx_output_json_adapter *adapter);
bool bx_output_json_field_string(struct bx_output_json_adapter *adapter,
                                 const char *key,
                                 const char *value);
bool bx_output_json_field_string_len(struct bx_output_json_adapter *adapter,
                                     const char *key,
                                     const void *value,
                                     size_t value_len);
bool bx_output_json_field_u64(struct bx_output_json_adapter *adapter,
                              const char *key,
                              uint64_t value);
bool bx_output_json_field_bool(struct bx_output_json_adapter *adapter,
                               const char *key,
                               bool value);
bool bx_output_json_field_null(struct bx_output_json_adapter *adapter, const char *key);
bool bx_output_json_end_object(struct bx_output_json_adapter *adapter);
bool bx_output_json_flush(struct bx_output_json_adapter *adapter);
int bx_output_json_error(const struct bx_output_json_adapter *adapter);

struct bx_output_color_adapter {
    struct bx_line_writer *writer;
    struct bx_output_profile_sink *profile;
    const char *prefix;
    size_t prefix_len;
    const char *reset;
    size_t reset_len;
    char record_terminator;
    bool enabled;
    int error;
};

void bx_output_color_adapter_init(struct bx_output_color_adapter *adapter,
                                  struct bx_line_writer *writer,
                                  bool enabled,
                                  const char *prefix,
                                  size_t prefix_len);
void bx_output_color_adapter_init_full(struct bx_output_color_adapter *adapter,
                                       struct bx_line_writer *writer,
                                       bool enabled,
                                       const char *prefix,
                                       size_t prefix_len,
                                       const char *reset,
                                       size_t reset_len,
                                       char record_terminator);
void bx_output_color_adapter_set_profile(struct bx_output_color_adapter *adapter,
                                         struct bx_output_profile_sink *profile);
bool bx_output_color_record_bytes(struct bx_output_color_adapter *adapter,
                                  const void *data,
                                  size_t length);
bool bx_output_color_record_cstr(struct bx_output_color_adapter *adapter, const char *text);
bool bx_output_color_flush(struct bx_output_color_adapter *adapter);
int bx_output_color_error(const struct bx_output_color_adapter *adapter);

struct bx_output_ordered_adapter_opts {
    size_t max_pending;
    uint64_t first_seq;
    struct bx_output_profile_sink *profile;
    void *user;
    void (*emit_payload)(void *user, void *payload);
    void (*dispose_payload)(void *user, void *payload);
};

struct bx_output_ordered_adapter {
    struct bx_output_publication publication;
    struct bx_output_ordered_adapter_opts opts;
    struct bx_output_profile_sink *profile;
    bool initialized;
    int error;
};

bool bx_output_ordered_adapter_init(struct bx_output_ordered_adapter *adapter,
                                    const struct bx_output_ordered_adapter_opts *opts);
bool bx_output_ordered_submit(struct bx_output_ordered_adapter *adapter,
                              uint64_t seq,
                              void *payload);
bool bx_output_ordered_skip(struct bx_output_ordered_adapter *adapter, uint64_t seq);
void bx_output_ordered_close(struct bx_output_ordered_adapter *adapter);
bool bx_output_ordered_join(struct bx_output_ordered_adapter *adapter);
void bx_output_ordered_dispose(struct bx_output_ordered_adapter *adapter);
int bx_output_ordered_error(const struct bx_output_ordered_adapter *adapter);

#endif /* BX_LIB_OUTPUT_ADAPTER_H */
