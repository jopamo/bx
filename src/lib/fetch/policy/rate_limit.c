#include "lib/fetch/rate_limit.h"
#include <stdbool.h>

static bool decimal(const char* text, int64_t* value) {
    if (!text || !*text)
        return false;
    int64_t result = 0;
    for (; *text; text++) {
        if (*text < '0' || *text > '9' || result > (INT64_MAX - (*text - '0')) / 10)
            return false;
        result = result * 10 + (*text - '0');
    }
    *value = result;
    return true;
}

int64_t bx_fetch_rate_limit_reset(const char* remaining, const char* reset, int64_t now) {
    int64_t left, epoch;
    if (now < 0 || !decimal(remaining, &left) || left != 0 ||
        !decimal(reset, &epoch) || epoch < now)
        return -1;
    return epoch;
}
