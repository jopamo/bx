#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pcre2_matcher.h"

struct bx_regex {
    pcre2_code *code;
    pcre2_match_data *md;
    int errnum;
};

int bx_regex_compile(struct bx_regex **out, const char *pattern, int flags, char **errmsg) {
    uint32_t pcre2_flags = 0;
    struct bx_regex *rx;

    if (!out || !pattern) {
        errno = EINVAL;
        return -1;
    }
    *out = NULL;
    if (flags & BX_REGEX_ICASE)
        pcre2_flags |= PCRE2_CASELESS;
    if (flags & BX_REGEX_MULTILINE)
        pcre2_flags |= PCRE2_MULTILINE;
    if (flags & BX_REGEX_DOTALL)
        pcre2_flags |= PCRE2_DOTALL;

    int errcode;
    PCRE2_SIZE erroffset;
    pcre2_code *code = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
                                      pcre2_flags, &errcode, &erroffset, NULL);
    if (!code) {
        if (errmsg) {
            PCRE2_UCHAR errbuf[256];
            int msg_rc = pcre2_get_error_message(errcode, errbuf, sizeof(errbuf));
            const char *msg = msg_rc >= 0 ? (const char *)errbuf : "regex compile failed";
            size_t need = snprintf(NULL, 0, "regex parse error at offset %zu: %s",
                                   (size_t)erroffset, msg);
            char *detail = malloc(need + 1);
            if (detail) {
                snprintf(detail, need + 1, "regex parse error at offset %zu: %s",
                         (size_t)erroffset, msg);
            }
            *errmsg = detail;
        }
        return -1;
    }

    rx = calloc(1u, sizeof(*rx));
    if (!rx) {
        pcre2_code_free(code);
        errno = ENOMEM;
        return -1;
    }
    rx->code = code;
    rx->md = pcre2_match_data_create_from_pattern(code, NULL);
    if (!rx->md) {
        pcre2_code_free(code);
        free(rx);
        errno = ENOMEM;
        return -1;
    }

    pcre2_jit_compile(code, PCRE2_JIT_COMPLETE);

    *out = rx;
    return 0;
}

int bx_regex_find(struct bx_regex *rx, const unsigned char *buf, size_t len,
                  size_t start, struct bx_match *match) {
    static const unsigned char empty[] = "";

    if (!rx || !rx->code || !rx->md || (!buf && len != 0u) || !match || start > len) {
        if (rx)
            rx->errnum = EINVAL;
        return -1;
    }
    if (rx->errnum != 0)
        return -1;
    if (!buf)
        buf = empty;

    int rc = pcre2_match(rx->code, (PCRE2_SPTR)buf, len, start, 0, rx->md, NULL);
    if (rc == PCRE2_ERROR_NOMATCH)
        return 1;
    if (rc < 0) {
        switch (rc) {
        case PCRE2_ERROR_NOMEMORY:
            rx->errnum = ENOMEM;
            break;
        case PCRE2_ERROR_MATCHLIMIT:
        case PCRE2_ERROR_DEPTHLIMIT:
        case PCRE2_ERROR_HEAPLIMIT:
        case PCRE2_ERROR_JIT_STACKLIMIT:
            rx->errnum = EOVERFLOW;
            break;
        default:
            rx->errnum = EIO;
            break;
        }
        return -1;
    }

    PCRE2_SIZE *ov = pcre2_get_ovector_pointer(rx->md);
    match->start = (size_t)ov[0];
    match->end   = (size_t)ov[1];
    return 0;
}

int bx_regex_error(const struct bx_regex *rx) {
    return rx ? rx->errnum : EINVAL;
}

void bx_regex_free(struct bx_regex *rx) {
    if (!rx) return;
    pcre2_match_data_free(rx->md);
    pcre2_code_free(rx->code);
    free(rx);
}

void bx_regex_print_version(void) {
    PCRE2_UCHAR version[128];
    int jit = 0;

    memset(version, 0, sizeof(version));
    if (pcre2_config(PCRE2_CONFIG_VERSION, version) != 0 || version[0] == 0) {
        puts("PCRE2 is available");
        return;
    }
    (void)pcre2_config(PCRE2_CONFIG_JIT, &jit);
    printf("%s is available (JIT is %savailable)\n", (char *)version,
           jit ? "" : "not ");
}
