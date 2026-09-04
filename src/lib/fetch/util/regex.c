#include "lib/fetch/regex.h"
#include <regex.h>
#include <strings.h>

int mira_regex_compile_flags_for_type(const char *regex_type, int *flags_out) {
    if (!flags_out) return -1;

    const char *type = regex_type;
    if (!type || type[0] == '\0' ||
        strcasecmp(type, "posix") == 0 ||
        strcasecmp(type, "posix-extended") == 0) {
        *flags_out = REG_EXTENDED | REG_NOSUB;
        return 0;
    }

    if (strcasecmp(type, "posix-basic") == 0) {
        *flags_out = REG_NOSUB;
        return 0;
    }

    return -1;
}
