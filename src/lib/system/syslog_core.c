#define _POSIX_C_SOURCE 200809L

#include "lib/system/syslog_core.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

const struct bx_syslog_name bx_syslog_facility_names[] = {
    {"kern", LOG_KERN},       {"user", LOG_USER},
    {"mail", LOG_MAIL},       {"daemon", LOG_DAEMON},
    {"auth", LOG_AUTH},       {"security", LOG_AUTH},
    {"syslog", LOG_SYSLOG},   {"lpr", LOG_LPR},
    {"news", LOG_NEWS},       {"uucp", LOG_UUCP},
    {"cron", LOG_CRON},
#ifdef LOG_AUTHPRIV
    {"authpriv", LOG_AUTHPRIV},
#endif
#ifdef LOG_FTP
    {"ftp", LOG_FTP},
#endif
    {"local0", LOG_LOCAL0},   {"local1", LOG_LOCAL1},
    {"local2", LOG_LOCAL2},   {"local3", LOG_LOCAL3},
    {"local4", LOG_LOCAL4},   {"local5", LOG_LOCAL5},
    {"local6", LOG_LOCAL6},   {"local7", LOG_LOCAL7},
    {NULL, -1},
};

const struct bx_syslog_name bx_syslog_priority_names[] = {
    {"emerg", LOG_EMERG},     {"panic", LOG_EMERG},
    {"alert", LOG_ALERT},     {"crit", LOG_CRIT},
    {"err", LOG_ERR},         {"error", LOG_ERR},
    {"warn", LOG_WARNING},    {"warning", LOG_WARNING},
    {"notice", LOG_NOTICE},   {"info", LOG_INFO},
    {"debug", LOG_DEBUG},     {"none", -2},
    {NULL, -1},
};

static const struct bx_syslog_name *bx_syslog_find_name(
    const struct bx_syslog_name *table,
    const char *name) {
    for (const struct bx_syslog_name *entry = table;
         entry->name != NULL; entry++) {
        if (strcmp(entry->name, name) == 0)
            return entry;
    }
    return NULL;
}

static const char *bx_syslog_find_value(
    const struct bx_syslog_name *table,
    int value) {
    for (const struct bx_syslog_name *entry = table;
         entry->name != NULL; entry++) {
        if (entry->value == value)
            return entry->name;
    }
    return NULL;
}

const char *bx_syslog_facility_name(int pri) {
    return bx_syslog_find_value(
        bx_syslog_facility_names, LOG_FAC(pri) << 3);
}

const char *bx_syslog_priority_name(int pri) {
    return bx_syslog_find_value(bx_syslog_priority_names, LOG_PRI(pri));
}

bool bx_syslog_facility_value(const char *name, int *value_out) {
    const struct bx_syslog_name *entry =
        bx_syslog_find_name(bx_syslog_facility_names, name);
    if (entry == NULL)
        return false;
    if (value_out != NULL)
        *value_out = entry->value;
    return true;
}

bool bx_syslog_priority_value(const char *name, int *value_out) {
    const struct bx_syslog_name *entry =
        bx_syslog_find_name(bx_syslog_priority_names, name);
    if (entry == NULL)
        return false;
    if (value_out != NULL)
        *value_out = entry->value;
    return true;
}

size_t bx_syslog_trim_datagram(unsigned char *data, size_t length) {
    (void)data;
    while (length > 0u &&
           (data[length - 1u] == '\0' || data[length - 1u] == '\n'))
        length--;
    return length;
}

static bool bx_syslog_has_timestamp(
    const unsigned char *message,
    size_t length) {
    return length >= 16u &&
        message[3] == ' ' && message[6] == ' ' &&
        message[9] == ':' && message[12] == ':' &&
        message[15] == ' ';
}

static size_t bx_syslog_parse_pri(
    const unsigned char *segment,
    size_t length,
    int *pri_out) {
    int pri = BX_SYSLOG_DEFAULT_PRI;
    size_t cursor = 0u;

    if (length > 0u && segment[0] == '<') {
        size_t index = 1u;
        unsigned value = 0u;
        bool any = false;
        bool overflow = false;
        while (index < length && isdigit(segment[index])) {
            any = true;
            unsigned digit = (unsigned)(segment[index] - '0');
            if (value > 1000000u)
                overflow = true;
            else
                value = value * 10u + digit;
            index++;
        }
        /*
         * bb_strtou() is called with the byte after '<' and always publishes
         * that starting position through its end pointer, even when no
         * decimal digit was accepted. Thus a leading '<' is consumed for
         * malformed negative-looking, empty, and nonnumeric prefixes too.
         */
        cursor = index;
        if (any) {
            pri = overflow ? -1 : (int)value;
        }
        if (cursor < length && segment[cursor] == '>')
            cursor++;
        else
            pri = BX_SYSLOG_DEFAULT_PRI;
        if ((pri & ~(LOG_FACMASK | LOG_PRIMASK)) != 0)
            pri = BX_SYSLOG_DEFAULT_PRI;
    }

    *pri_out = pri;
    return cursor;
}

bool bx_syslog_record_parse(
    const unsigned char *segment,
    size_t segment_len,
    unsigned char *normalized,
    size_t normalized_capacity,
    struct bx_syslog_record *record) {
    if (segment == NULL || normalized == NULL || record == NULL ||
        normalized_capacity == 0u)
        return false;

    int pri;
    size_t cursor = bx_syslog_parse_pri(segment, segment_len, &pri);
    size_t out = 0u;
    while (cursor < segment_len) {
        unsigned char byte = segment[cursor++];
        if (byte == '\n')
            byte = ' ';
        if (byte <= 0x1fu && byte != '\t') {
            if (out + 2u >= normalized_capacity)
                return false;
            normalized[out++] = '^';
            normalized[out++] = (unsigned char)(byte + '@');
        } else {
            if (out + 1u >= normalized_capacity)
                return false;
            normalized[out++] = byte;
        }
    }
    normalized[out] = '\0';

    memset(record, 0, sizeof(*record));
    record->pri = pri;
    record->message = normalized;
    record->message_len = out;
    if (bx_syslog_has_timestamp(normalized, out)) {
        memcpy(record->client_timestamp, normalized,
               BX_SYSLOG_TIMESTAMP_LEN);
        record->client_timestamp[BX_SYSLOG_TIMESTAMP_LEN] = '\0';
        record->has_client_timestamp = true;
    }
    return true;
}

bool bx_syslog_format_record(
    const struct bx_syslog_record *record,
    const char *hostname,
    const char generated_timestamp[BX_SYSLOG_TIMESTAMP_LEN + 1u],
    bool compact,
    bool strip_client_timestamp,
    char *output,
    size_t output_capacity,
    size_t *output_len) {
    if (record == NULL || hostname == NULL || generated_timestamp == NULL ||
        output == NULL || output_capacity == 0u)
        return false;

    const char *timestamp = generated_timestamp;
    const unsigned char *message = record->message;
    size_t message_len = record->message_len;
    if (record->has_client_timestamp) {
        if (!strip_client_timestamp)
            timestamp = record->client_timestamp;
        if (message_len >= 16u) {
            message += 16u;
            message_len -= 16u;
        }
    }

    int prefix;
    if (compact) {
        prefix = snprintf(output, output_capacity, "%.15s ", timestamp);
    } else {
        const char *facility = bx_syslog_facility_name(record->pri);
        const char *priority = bx_syslog_priority_name(record->pri);
        char pri_text[20];
        if (facility != NULL && priority != NULL)
            (void)snprintf(pri_text, sizeof(pri_text), "%s.%s",
                           facility, priority);
        else
            (void)snprintf(pri_text, sizeof(pri_text), "<%d>", record->pri);
        prefix = snprintf(output, output_capacity, "%.15s %.64s %s ",
                          timestamp, hostname, pri_text);
    }
    if (prefix < 0 || (size_t)prefix >= output_capacity ||
        message_len + 1u > output_capacity - (size_t)prefix)
        return false;
    memcpy(output + prefix, message, message_len);
    output[(size_t)prefix + message_len] = '\n';
    output[(size_t)prefix + message_len + 1u] = '\0';
    if (output_len != NULL)
        *output_len = (size_t)prefix + message_len + 1u;
    return true;
}

bool bx_syslog_rule_matches(const struct bx_syslog_rule *rule, int pri) {
    unsigned facility = (unsigned)LOG_FAC(pri);
    unsigned priority = (unsigned)LOG_PRI(pri);
    return rule != NULL && facility < LOG_NFACILITIES &&
        (rule->facility_priomap[facility] & (1u << priority)) != 0u;
}
