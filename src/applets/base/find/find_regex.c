#include <stdio.h>
#include <string.h>

#include "find_regex.h"

bool find_parse_regextype(const char *progname, const char *text,
                          enum find_regex_type *out) {
    if (text && strcmp(text, "posix-extended") == 0) {
        *out = FIND_REGEX_TYPE_POSIX_EXTENDED;
        return true;
    }

    fprintf(stderr, "%s: unsupported argument to -regextype: %s\n",
            progname, text ? text : "(null)");
    return false;
}

bool find_compile_regex(const char *progname, const char *optname,
                        enum find_regex_type regex_type,
                        const char *pattern, bool ignore_case,
                        regex_t *out) {
    int flags = 0;
    if (regex_type == FIND_REGEX_TYPE_POSIX_EXTENDED)
        flags |= REG_EXTENDED;
#ifdef REG_ICASE
    if (ignore_case)
        flags |= REG_ICASE;
#else
    (void)ignore_case;
#endif

    int rc = regcomp(out, pattern, flags);
    if (rc == 0)
        return true;

    char errbuf[256];
    if (rc == REG_EBRACK) {
        snprintf(errbuf, sizeof(errbuf), "Missing ']'");
    } else if (rc == REG_BADBR) {
        snprintf(errbuf, sizeof(errbuf), "Invalid contents of {}");
    } else {
        regerror(rc, out, errbuf, sizeof(errbuf));
    }
    fprintf(stderr, "%s: invalid argument to %s: %s (%s)\n",
            progname, optname, pattern ? pattern : "(null)", errbuf);
    return false;
}

bool find_match_regex(regex_t *regex, const char *text) {
    regmatch_t match;
    if (!regex || !text)
        return false;
    if (regexec(regex, text, 1, &match, 0) != 0)
        return false;
    return match.rm_so == 0 && (size_t)match.rm_eo == strlen(text);
}
