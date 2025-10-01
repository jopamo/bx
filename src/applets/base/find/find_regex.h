#ifndef BX_APPLETS_BASE_FIND_REGEX_H
#define BX_APPLETS_BASE_FIND_REGEX_H

#include <regex.h>
#include <stdbool.h>

enum find_regex_type {
    FIND_REGEX_TYPE_DEFAULT = 0,
    FIND_REGEX_TYPE_POSIX_EXTENDED,
};

bool find_parse_regextype(const char *progname, const char *text,
                          enum find_regex_type *out);
bool find_compile_regex(const char *progname, const char *optname,
                        enum find_regex_type regex_type,
                        const char *pattern, bool ignore_case,
                        regex_t *out);
bool find_match_regex(regex_t *regex, const char *text);

#endif
