#ifndef MIRA_HTTP_STATUS_H
#define MIRA_HTTP_STATUS_H

/* MIRA_HEADER_OWNER: util */
/* MIRA_HEADER_CONSUMERS: cli, core */

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>

static inline bool mira_http_status_parse_token(const char* token, size_t len, int* status) {
    if (!token || !status)
        return false;

    while (len > 0 && isspace((unsigned char)token[0])) {
        token++;
        len--;
    }
    while (len > 0 && isspace((unsigned char)token[len - 1])) {
        len--;
    }

    if (len != 3)
        return false;

    int code = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)token[i];
        if (!isdigit(ch))
            return false;
        code = (code * 10) + (int)(ch - '0');
    }

    if (code < 100 || code > 599)
        return false;
    *status = code;
    return true;
}

static inline bool mira_http_status_list_contains(const char* list, int status) {
    if (!list)
        return false;

    const char* cursor = list;
    while (*cursor != '\0') {
        const char* token = cursor;
        while (*cursor != '\0' && *cursor != ',')
            cursor++;

        int configured = 0;
        if (mira_http_status_parse_token(token, (size_t)(cursor - token), &configured) && configured == status) {
            return true;
        }

        if (*cursor == ',')
            cursor++;
    }

    return false;
}

#endif
