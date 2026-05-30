/*
    Copyright (c)  2006, 2007		Dmitry Butskoy
                                        <dmitry@butskoy.name>
    License:  GPL v2 or any later

    See COPYING for the status of this software.
*/

#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#include "lib/time_parse.h"
#include "traceroute.h"

/*  Just returns current time as double, with most possible precision...  */

double get_time(void) {
    struct timespec ts;
    double d = 0.0;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0 ||
        !bx_time_timespec_to_seconds_double(&ts, &d))
        return 0.0;

    return d;
}
