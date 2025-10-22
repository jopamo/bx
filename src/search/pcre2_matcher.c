#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pcre2_matcher.h"

struct bx_regex {
    pcre2_code *code;
    pcre2_match_data *md;
};

int bx_regex_compile(struct bx_regex **out, const char *pattern, int flags, char **errmsg) {
    uint32_t pcre2_flags = 0;
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

    struct bx_regex *rx = malloc(sizeof(*rx));
    rx->code = code;
    rx->md = pcre2_match_data_create_from_pattern(code, NULL);

    pcre2_jit_compile(code, PCRE2_JIT_COMPLETE);

    *out = rx;
    return 0;
}

int bx_regex_find(struct bx_regex *rx, const unsigned char *buf, size_t len,
                  size_t start, struct bx_match *match) {
    int rc = pcre2_match(rx->code, (PCRE2_SPTR)buf, len, start, 0, rx->md, NULL);
    if (rc < 0)
        return -1;

    PCRE2_SIZE *ov = pcre2_get_ovector_pointer(rx->md);
    match->start = (size_t)ov[0];
    match->end   = (size_t)ov[1];
    return 0;
}

void bx_regex_free(struct bx_regex *rx) {
    if (!rx) return;
    pcre2_match_data_free(rx->md);
    pcre2_code_free(rx->code);
    free(rx);
}

void bx_regex_print_version(void) {
    PCRE2_UCHAR *version = NULL;
    int jit = 0;

    if (pcre2_config(PCRE2_CONFIG_VERSION, &version) != 0 || !version) {
        puts("PCRE2 is available");
        return;
    }
    (void)pcre2_config(PCRE2_CONFIG_JIT, &jit);
    printf("%s is available (JIT is %savailable)\n", (char *)version,
           jit ? "" : "not ");
}
