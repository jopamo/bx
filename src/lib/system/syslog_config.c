#define _GNU_SOURCE

#include "lib/system/syslog_config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void bx_syslog_error(
    char *error, size_t capacity, const char *path,
    size_t line, const char *detail) {
    if (error == NULL || capacity == 0u)
        return;
    if (line != 0u)
        (void)snprintf(error, capacity, "%s:%zu: %s", path, line, detail);
    else
        (void)snprintf(error, capacity, "%s: %s", path, detail);
}

void bx_syslog_config_init(struct bx_syslog_config *config) {
    if (config != NULL)
        memset(config, 0, sizeof(*config));
}

void bx_syslog_config_destroy(struct bx_syslog_config *config) {
    if (config == NULL)
        return;
    for (size_t index = 0u; index < config->sink_count; index++) {
        if (config->sinks[index].fd >= 0)
            close(config->sinks[index].fd);
        free(config->sinks[index].path);
    }
    free(config->sinks);
    free(config->rules);
    memset(config, 0, sizeof(*config));
}

static bool bx_syslog_grow(
    void **items, size_t *capacity, size_t item_size, size_t needed) {
    if (needed <= *capacity)
        return true;
    size_t next = *capacity == 0u ? 8u : *capacity * 2u;
    if (next < needed || next > SIZE_MAX / item_size) {
        errno = ENOMEM;
        return false;
    }
    void *grown = realloc(*items, next * item_size);
    if (grown == NULL)
        return false;
    *items = grown;
    *capacity = next;
    return true;
}

static bool bx_syslog_sink_index(
    struct bx_syslog_config *config,
    const char *path,
    size_t *index_out) {
    for (size_t index = 0u; index < config->sink_count; index++) {
        if (strcmp(config->sinks[index].path, path) == 0) {
            *index_out = index;
            return true;
        }
    }
    if (!bx_syslog_grow((void **)&config->sinks, &config->sink_capacity,
                        sizeof(*config->sinks), config->sink_count + 1u))
        return false;
    struct bx_syslog_sink *sink = &config->sinks[config->sink_count];
    memset(sink, 0, sizeof(*sink));
    sink->fd = -1;
    sink->path = strdup(path);
    if (sink->path == NULL)
        return false;
    sink->kind = strcmp(path, "-") == 0
        ? BX_SYSLOG_SINK_STDOUT : BX_SYSLOG_SINK_FILE;
    *index_out = config->sink_count++;
    return true;
}

static bool bx_syslog_parse_priority(
    const char *text, bool *negated, bool *single, uint8_t *map) {
    *negated = false;
    *single = false;
    if (*text == '!') {
        *negated = true;
        text++;
    }
    if (*text == '=') {
        *single = true;
        text++;
    }
    if (strcmp(text, "*") == 0) {
        *map = 0xffu;
        return true;
    }
    int value;
    if (!bx_syslog_priority_value(text, &value))
        return false;
    if (value == -2) {
        *negated = true;
        *map = 0u;
        return true;
    }
    uint8_t bit = (uint8_t)(1u << (unsigned)value);
    uint8_t result = 0u;
    do {
        result |= bit;
        if (*single)
            break;
        bit >>= 1u;
    } while (bit != 0u);
    if (*negated)
        result = (uint8_t)~result;
    *map = result;
    return true;
}

static bool bx_syslog_apply_selector(
    struct bx_syslog_rule *rule, char *selector) {
    char *dot = strchr(selector, '.');
    if (dot == NULL)
        return false;
    *dot++ = '\0';

    bool negated;
    bool single;
    uint8_t priomap;
    if (!bx_syslog_parse_priority(dot, &negated, &single, &priomap))
        return false;
    (void)single;

    uint32_t facilities = 0u;
    if (strcmp(selector, "*") == 0) {
        facilities = LOG_NFACILITIES >= 32
            ? UINT32_MAX : ((1u << LOG_NFACILITIES) - 1u);
    } else {
        char *cursor = selector;
        char *facility;
        while ((facility = strsep(&cursor, ",")) != NULL) {
            int value;
            if (facility[0] == '\0' ||
                !bx_syslog_facility_value(facility, &value))
                return false;
            facilities |= 1u << (unsigned)LOG_FAC(value);
        }
    }

    for (unsigned facility = 0u; facility < LOG_NFACILITIES; facility++) {
        if ((facilities & (1u << facility)) == 0u)
            continue;
        if (negated)
            rule->facility_priomap[facility] &= priomap;
        else
            rule->facility_priomap[facility] |= priomap;
    }
    return true;
}

static bool bx_syslog_parse_rule(
    struct bx_syslog_config *config,
    char *selectors,
    const char *action) {
    if (!bx_syslog_grow((void **)&config->rules, &config->rule_capacity,
                        sizeof(*config->rules), config->rule_count + 1u))
        return false;
    struct bx_syslog_rule rule;
    memset(&rule, 0, sizeof(rule));

    char *cursor = selectors;
    char *selector;
    while ((selector = strsep(&cursor, ";")) != NULL) {
        if (selector[0] == '\0' ||
            !bx_syslog_apply_selector(&rule, selector)) {
            errno = EINVAL;
            return false;
        }
    }
    if (!bx_syslog_sink_index(config, action, &rule.sink_index))
        return false;
    config->rules[config->rule_count++] = rule;
    return true;
}

bool bx_syslog_config_load(
    struct bx_syslog_config *candidate,
    const char *path,
    bool missing_is_ok,
    char *error,
    size_t error_capacity) {
    if (candidate == NULL || path == NULL) {
        errno = EINVAL;
        return false;
    }
    bx_syslog_config_init(candidate);
    FILE *stream = fopen(path, "r");
    if (stream == NULL) {
        if (missing_is_ok && errno == ENOENT)
            return true;
        bx_syslog_error(error, error_capacity, path, 0u, strerror(errno));
        return false;
    }

    char *line = NULL;
    size_t capacity = 0u;
    size_t line_number = 0u;
    bool ok = true;
    while (getline(&line, &capacity, stream) >= 0) {
        line_number++;
        line[strcspn(line, "\r\n")] = '\0';
        char *start = line + strspn(line, " \t");
        if (*start == '\0' || *start == '#')
            continue;
        char *comment = strchr(start, '#');
        if (comment != NULL)
            *comment = '\0';
        size_t length = strlen(start);
        while (length > 0u &&
               (start[length - 1u] == ' ' || start[length - 1u] == '\t'))
            start[--length] = '\0';
        char *action = start + strcspn(start, " \t");
        if (*action == '\0') {
            errno = EINVAL;
            ok = false;
            break;
        }
        *action++ = '\0';
        action += strspn(action, " \t");
        char *trailing = action + strcspn(action, " \t");
        if (*action == '\0' ||
            (*trailing != '\0' &&
             trailing[strspn(trailing, " \t")] != '\0')) {
            errno = EINVAL;
            ok = false;
            break;
        }
        *trailing = '\0';
        if (!bx_syslog_parse_rule(candidate, start, action)) {
            ok = false;
            break;
        }
    }
    if (ferror(stream))
        ok = false;
    int saved_errno = errno;
    free(line);
    (void)fclose(stream);
    if (!ok) {
        bx_syslog_error(error, error_capacity, path, line_number,
                        saved_errno == ENOMEM
                            ? "out of memory" : "invalid configuration");
        bx_syslog_config_destroy(candidate);
        errno = saved_errno;
        return false;
    }
    return true;
}
