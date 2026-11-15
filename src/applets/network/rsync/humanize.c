#include "portable_util.h"
#include "lib/size_parse.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>

int
bx_rsync_humanize_number(char *buf, size_t len, int64_t num,
    const char *suffix, int scale, int flags)
{
	long double val = num;
	int res, i = 0, decimal;
	const char *unit;
	const int divisor = flags & HN_DIVISOR_1000 ? 1000 : 1024;

	if (scale != HN_AUTOSCALE && scale != 0) {
		errno = EINVAL;
		return -1;
	}
	if (scale == HN_AUTOSCALE) {
		while ((val <= -divisor || val >= divisor) &&
		    i < 6) {
			val /= divisor;
			i++;
		}
	}
	unit = bx_size_unit_label(flags & HN_IEC_PREFIXES ?
	    BX_SIZE_UNIT_LABEL_IEC_I_SUFFIX : BX_SIZE_UNIT_LABEL_SI_UPPER_K,
	    (unsigned int)i);
	decimal = (flags & HN_DECIMAL) && i > 0 && val > -10 && val < 10;
	res = snprintf(buf, len, "%.*Lf%s%s%s", decimal ? 1 : 0,
	    val, unit, flags & HN_NOSPACE ? "" : " ", suffix);
	if (res < 0 || (size_t)res >= len) {
		errno = ENOSPC;
		return -1;
	}
	return res;
}
