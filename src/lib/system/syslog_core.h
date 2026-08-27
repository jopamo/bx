#ifndef BX_LIB_SYSTEM_SYSLOG_CORE_H
#define BX_LIB_SYSTEM_SYSLOG_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <syslog.h>

#define BX_SYSLOG_MAX_HOSTNAME 64u
#define BX_SYSLOG_TIMESTAMP_LEN 15u
#define BX_SYSLOG_DEFAULT_PRI (LOG_USER | LOG_NOTICE)

enum bx_syslog_sink_kind {
    BX_SYSLOG_SINK_FILE = 0,
    BX_SYSLOG_SINK_STDOUT,
    BX_SYSLOG_SINK_CONSOLE,
    BX_SYSLOG_SINK_KMSG,
};

struct bx_syslog_record {
    int pri;
    const unsigned char *message;
    size_t message_len;
    bool has_client_timestamp;
    char client_timestamp[BX_SYSLOG_TIMESTAMP_LEN + 1u];
};

struct bx_syslog_sink {
    enum bx_syslog_sink_kind kind;
    char *path;
    int fd;
    dev_t device;
    ino_t inode;
    uint64_t size;
    bool regular;
};

struct bx_syslog_rule {
    uint8_t facility_priomap[LOG_NFACILITIES];
    size_t sink_index;
};

struct bx_syslog_config {
    struct bx_syslog_rule *rules;
    size_t rule_count;
    size_t rule_capacity;
    struct bx_syslog_sink *sinks;
    size_t sink_count;
    size_t sink_capacity;
};

struct bx_syslog_remote {
    char *host;
    char *port;
    int fd;
    void *addresses;
    uint64_t sent;
    uint64_t failed;
    uint64_t unresolved;
    uint64_t retry_after_ms;
};

struct bx_syslog_counters {
    uint64_t received_datagrams;
    uint64_t locally_emitted_records;
    uint64_t duplicate_drops;
    uint64_t truncated_datagrams;
    uint64_t filtered_records;
    uint64_t output_failures;
    uint64_t remote_send_failures;
};

struct bx_syslog_name {
    const char *name;
    int value;
};

extern const struct bx_syslog_name bx_syslog_facility_names[];
extern const struct bx_syslog_name bx_syslog_priority_names[];

const char *bx_syslog_facility_name(int pri);
const char *bx_syslog_priority_name(int pri);
bool bx_syslog_facility_value(const char *name, int *value_out);
bool bx_syslog_priority_value(const char *name, int *value_out);

size_t bx_syslog_trim_datagram(unsigned char *data, size_t length);
bool bx_syslog_record_parse(
    const unsigned char *segment,
    size_t segment_len,
    unsigned char *normalized,
    size_t normalized_capacity,
    struct bx_syslog_record *record);
bool bx_syslog_format_record(
    const struct bx_syslog_record *record,
    const char *hostname,
    const char generated_timestamp[BX_SYSLOG_TIMESTAMP_LEN + 1u],
    bool compact,
    bool strip_client_timestamp,
    char *output,
    size_t output_capacity,
    size_t *output_len);
bool bx_syslog_rule_matches(const struct bx_syslog_rule *rule, int pri);

#endif
