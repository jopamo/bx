#include "portable_util.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>

int
bx_rsync_humanize_number(char *buf, size_t len, int64_t num,
    const char *suffix, int scale, int flags)
{
	static const char prefixes[] = " KMGTPE";
	long double val = num;
	int res, i = 0, decimal;
	char unit[4] = "";
	const int divisor = flags & HN_DIVISOR_1000 ? 1000 : 1024;

	if (scale != HN_AUTOSCALE && scale != 0) {
		errno = EINVAL;
		return -1;
	}
	if (scale == HN_AUTOSCALE) {
		while ((val <= -divisor || val >= divisor) &&
		    i < (int)sizeof(prefixes) - 1) {
			val /= divisor;
			i++;
		}
	}
	if (i > 0)
		snprintf(unit, sizeof(unit), "%c%s", prefixes[i],
		    flags & HN_IEC_PREFIXES ? "i" : "");
	decimal = (flags & HN_DECIMAL) && i > 0 && val > -10 && val < 10;
	res = snprintf(buf, len, "%.*Lf%s%s%s", decimal ? 1 : 0,
	    val, unit, flags & HN_NOSPACE ? "" : " ", suffix);
	if (res < 0 || (size_t)res >= len) {
		errno = ENOSPC;
		return -1;
	}
	return res;
}
