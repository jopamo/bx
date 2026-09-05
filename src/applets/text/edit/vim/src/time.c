/* vi:set ts=8 sts=4 sw=4 noet:
 *
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 * See README.txt for an overview of the Vim source code.
 */

/*
 * time.c: functions related to time
 */

#include "vim.h"

/*
 * Cache of the current timezone name as retrieved from TZ, or an empty string
 * where unset, up to 64 octets long including trailing null byte.
 */
#if defined(HAVE_LOCALTIME_R) && defined(HAVE_TZSET)
static char	tz_cache[64];
#endif

/*
 * Call either localtime(3) or localtime_r(3) from POSIX libc time.h, with the
 * latter version preferred for reentrancy.
 *
 * If we use localtime_r(3) and we have tzset(3) available, check to see if the
 * environment variable TZ has changed since the last run, and call tzset(3) to
 * update the global timezone variables if it has.  This is because the POSIX
 * standard doesn't require localtime_r(3) implementations to do that as it
 * does with localtime(3), and we don't want to call tzset(3) every time.
 */
    static struct tm *
vim_localtime(
    const time_t	*timep,		// timestamp for local representation
    struct tm		*result UNUSED)	// pointer to caller return buffer
{
#ifdef HAVE_LOCALTIME_R
# ifdef HAVE_TZSET
    char		*tz;		// pointer for TZ environment var

    tz = (char *)mch_getenv((char_u *)"TZ");
    if (tz == NULL)
	tz = "";
    if (STRNCMP(tz_cache, tz, sizeof(tz_cache) - 1) != 0)
    {
	tzset();
	vim_strncpy((char_u *)tz_cache, (char_u *)tz, sizeof(tz_cache) - 1);
    }
# endif	// HAVE_TZSET
    return localtime_r(timep, result);
#else
    return localtime(timep);
#endif	// HAVE_LOCALTIME_R
}

/*
 * Return the current time in seconds.  Calls time(), unless test_settime()
 * was used.
 */
    time_T
vim_time(void)
{
#ifdef FEAT_EVAL
    return time_for_testing == 0 ? time(NULL) : time_for_testing;
#else
    return time(NULL);
#endif
}

/*
 * Replacement for ctime(), which is not safe to use.
 * Requires strftime(), otherwise returns "(unknown)".
 * If "thetime" is invalid returns "(invalid)".  Never returns NULL.
 * When "add_newline" is TRUE add a newline like ctime() does.
 * Uses a static buffer.
 */
    char *
get_ctime(time_t thetime, int add_newline)
{
    static char buf[100];  // hopefully enough for every language
#ifdef HAVE_STRFTIME
    struct tm	tmval;
    struct tm	*curtime;

    curtime = vim_localtime(&thetime, &tmval);
    // MSVC returns NULL for an invalid value of seconds.
    if (curtime == NULL)
	vim_strncpy((char_u *)buf, (char_u *)_("(Invalid)"), sizeof(buf) - 2);
    else
    {
	// xgettext:no-c-format
	if (strftime(buf, sizeof(buf) - 2, _("%a %b %d %H:%M:%S %Y"), curtime)
									  == 0)
	{
	    // Quoting "man strftime":
	    // > If the length of the result string (including the terminating
	    // > null byte) would exceed max bytes, then strftime() returns 0,
	    // > and the contents of the array are undefined.
	    vim_strncpy((char_u *)buf, (char_u *)_("(Invalid)"),
							      sizeof(buf) - 2);
	}
# ifdef MSWIN
	if (enc_codepage >= 0 && (int)GetACP() != enc_codepage)
	{
	    char_u	*to_free = NULL;
	    int		len;

	    acp_to_enc((char_u *)buf, (int)strlen(buf), &to_free, &len);
	    if (to_free != NULL)
	    {
		STRNCPY(buf, to_free, sizeof(buf) - 2);
		vim_free(to_free);
	    }
	}
# endif
    }
#else
    STRCPY(buf, "(unknown)");
#endif
    if (add_newline)
	STRCAT(buf, "\n");
    return buf;
}

#if defined(FEAT_EVAL)

# if defined(MACOS_X)
#  include <time.h>	// for time_t
# endif

/*
 * "localtime()" function
 */
    void
f_localtime(typval_T *argvars UNUSED, typval_T *rettv)
{
    rettv->vval.v_number = (varnumber_T)time(NULL);
}

# if defined(FEAT_RELTIME)
/*
 * Convert a List to proftime_T.
 * Return FAIL when there is something wrong.
 */
    static int
list2proftime(typval_T *arg, proftime_T *tm)
{
    long	n1, n2;
    int	error = FALSE;

    if (arg->v_type != VAR_LIST || arg->vval.v_list == NULL
					     || arg->vval.v_list->lv_len != 2)
	return FAIL;
    n1 = list_find_nr(arg->vval.v_list, 0L, &error);
    n2 = list_find_nr(arg->vval.v_list, 1L, &error);
#  ifdef MSWIN
    tm->HighPart = n1;
    tm->LowPart = n2;
#  else
    tm->tv_sec = n1;
    tm->tv_fsec = n2;
#  endif
    return error ? FAIL : OK;
}
# endif // FEAT_RELTIME

/*
 * "reltime()" function
 */
    void
f_reltime(typval_T *argvars UNUSED, typval_T *rettv UNUSED)
{
# ifdef FEAT_RELTIME
    proftime_T	res;
    proftime_T	start;
    long	n1, n2;

    if (rettv_list_alloc(rettv) == FAIL)
	return;

    if (in_vim9script()
	    && (check_for_opt_list_arg(argvars, 0) == FAIL
		|| (argvars[0].v_type != VAR_UNKNOWN
		    && check_for_opt_list_arg(argvars, 1) == FAIL)))
	return;

    if (argvars[0].v_type == VAR_UNKNOWN)
    {
	// No arguments: get current time.
	profile_start(&res);
    }
    else if (argvars[1].v_type == VAR_UNKNOWN)
    {
	if (list2proftime(&argvars[0], &res) == FAIL)
	{
	    if (in_vim9script())
		emsg(_(e_invalid_argument));
	    return;
	}
	profile_end(&res);
    }
    else
    {
	// Two arguments: compute the difference.
	if (list2proftime(&argvars[0], &start) == FAIL
		|| list2proftime(&argvars[1], &res) == FAIL)
	{
	    if (in_vim9script())
		emsg(_(e_invalid_argument));
	    return;
	}
	profile_sub(&res, &start);
    }

#  ifdef MSWIN
    n1 = res.HighPart;
    n2 = res.LowPart;
#  else
    n1 = res.tv_sec;
    n2 = res.tv_fsec;
#  endif
    list_append_number(rettv->vval.v_list, (varnumber_T)n1);
    list_append_number(rettv->vval.v_list, (varnumber_T)n2);
# endif
}

/*
 * "reltimefloat()" function
 */
    void
f_reltimefloat(typval_T *argvars UNUSED, typval_T *rettv)
{
# ifdef FEAT_RELTIME
    proftime_T	tm;
# endif

    rettv->v_type = VAR_FLOAT;
    rettv->vval.v_float = 0;
# ifdef FEAT_RELTIME
    if (in_vim9script() && check_for_list_arg(argvars, 0) == FAIL)
	return;

    if (list2proftime(&argvars[0], &tm) == OK)
	rettv->vval.v_float = profile_float(&tm);
    else if (in_vim9script())
	emsg(_(e_invalid_argument));
# endif
}

/*
 * "reltimestr()" function
 */
    void
f_reltimestr(typval_T *argvars UNUSED, typval_T *rettv)
{
    rettv->v_type = VAR_STRING;
    rettv->vval.v_string = NULL;
# ifdef FEAT_RELTIME
    if (in_vim9script() && check_for_list_arg(argvars, 0) == FAIL)
	return;

    proftime_T	tm;
    if (list2proftime(&argvars[0], &tm) == OK)
    {
#  ifdef MSWIN
	rettv->vval.v_string = vim_strsave((char_u *)profile_msg(&tm));
#  else
	static char buf[50];
	long usec = tm.tv_fsec / (TV_FSEC_SEC / 1000000);
	vim_snprintf(buf, sizeof(buf), "%3ld.%06ld", (long)tm.tv_sec, usec);
	rettv->vval.v_string = vim_strsave((char_u *)buf);
#  endif
    }
    else if (in_vim9script())
	emsg(_(e_invalid_argument));
# endif
}

# if defined(HAVE_STRFTIME)
/*
 * "strftime({format}[, {time}])" function
 */
    void
f_strftime(typval_T *argvars, typval_T *rettv)
{
    struct tm	tmval;
    struct tm	*curtime;
    time_t	seconds;
    char_u	*p;

    if (in_vim9script()
	    && (check_for_string_arg(argvars, 0) == FAIL
		|| check_for_opt_number_arg(argvars, 1) == FAIL))
	return;

    rettv->v_type = VAR_STRING;

    p = tv_get_string(&argvars[0]);
    if (argvars[1].v_type == VAR_UNKNOWN)
	seconds = time(NULL);
    else
	seconds = (time_t)tv_get_number(&argvars[1]);
    curtime = vim_localtime(&seconds, &tmval);
    // MSVC returns NULL for an invalid value of seconds.
    if (curtime == NULL)
    {
	rettv->vval.v_string = vim_strsave((char_u *)_("(Invalid)"));
	return;
    }

#  ifdef MSWIN
    WCHAR	    result_buf[256];
    WCHAR	    *wp;

    wp = enc_to_utf16(p, NULL);
    if (wp != NULL)
	(void)wcsftime(result_buf, ARRAY_LENGTH(result_buf), wp, curtime);
    else
	result_buf[0] = NUL;
    rettv->vval.v_string = utf16_to_enc(result_buf, NULL);
    vim_free(wp);
#  else
    char_u	    result_buf[256];
    vimconv_T   conv;
    char_u	    *enc;

    conv.vc_type = CONV_NONE;
    enc = enc_locale();
    convert_setup(&conv, p_enc, enc);
    if (conv.vc_type != CONV_NONE)
	p = string_convert(&conv, p, NULL);
    if (p == NULL || strftime((char *)result_buf, sizeof(result_buf),
		(char *)p, curtime) == 0)
	result_buf[0] = NUL;

    if (conv.vc_type != CONV_NONE)
	vim_free(p);
    convert_setup(&conv, enc, p_enc);
    if (conv.vc_type != CONV_NONE)
	rettv->vval.v_string = string_convert(&conv, result_buf, NULL);
    else
	rettv->vval.v_string = vim_strsave(result_buf);

    // Release conversion descriptors
    convert_setup(&conv, NULL, NULL);
    vim_free(enc);
#  endif
}
# endif

# if defined(HAVE_STRPTIME)
/*
 * "strptime({format}, {timestring})" function
 */
    void
f_strptime(typval_T *argvars, typval_T *rettv)
{
    struct tm	tmval;
    char_u	*fmt;
    char_u	*str;
    vimconv_T   conv;
    char_u	*enc;

    if (in_vim9script()
	    && (check_for_string_arg(argvars, 0) == FAIL
		|| check_for_string_arg(argvars, 1) == FAIL))
	return;

    CLEAR_FIELD(tmval);
    tmval.tm_isdst = -1;
    fmt = tv_get_string(&argvars[0]);
    str = tv_get_string(&argvars[1]);

    conv.vc_type = CONV_NONE;
    enc = enc_locale();
    convert_setup(&conv, p_enc, enc);
    if (conv.vc_type != CONV_NONE)
	fmt = string_convert(&conv, fmt, NULL);
#  ifdef HAVE_TZSET
    // Pick up any change to $TZ so mktime() uses the requested timezone.
    tzset();
#  endif
    if (fmt == NULL
	    || strptime((char *)str, (char *)fmt, &tmval) == NULL
	    || (rettv->vval.v_number = mktime(&tmval)) == -1)
	rettv->vval.v_number = 0;

    if (conv.vc_type != CONV_NONE)
	vim_free(fmt);
    convert_setup(&conv, NULL, NULL);
    vim_free(enc);
}
# endif

# if defined(STARTUPTIME)
static struct timeval	prev_timeval;

#  ifdef MSWIN
/*
 * Windows doesn't have gettimeofday(), although it does have struct timeval.
 */
    static int
gettimeofday(struct timeval *tv, char *dummy UNUSED)
{
    long t = clock();
    tv->tv_sec = t / CLOCKS_PER_SEC;
    tv->tv_usec = (t - tv->tv_sec * CLOCKS_PER_SEC) * 1000000 / CLOCKS_PER_SEC;
    return 0;
}
#  endif

/*
 * Save the previous time before doing something that could nest.
 * set "*tv_rel" to the time elapsed so far.
 */
    void
time_push(void *tv_rel, void *tv_start)
{
    *((struct timeval *)tv_rel) = prev_timeval;
    gettimeofday(&prev_timeval, NULL);
    ((struct timeval *)tv_rel)->tv_usec = prev_timeval.tv_usec
					- ((struct timeval *)tv_rel)->tv_usec;
    ((struct timeval *)tv_rel)->tv_sec = prev_timeval.tv_sec
					 - ((struct timeval *)tv_rel)->tv_sec;
    if (((struct timeval *)tv_rel)->tv_usec < 0)
    {
	((struct timeval *)tv_rel)->tv_usec += 1000000;
	--((struct timeval *)tv_rel)->tv_sec;
    }
    *(struct timeval *)tv_start = prev_timeval;
}

/*
 * Compute the previous time after doing something that could nest.
 * Subtract "*tp" from prev_timeval;
 * Note: The arguments are (void *) to avoid trouble with systems that don't
 * have struct timeval.
 */
    void
time_pop(
    void	*tp)	// actually (struct timeval *)
{
    prev_timeval.tv_usec -= ((struct timeval *)tp)->tv_usec;
    prev_timeval.tv_sec -= ((struct timeval *)tp)->tv_sec;
    if (prev_timeval.tv_usec < 0)
    {
	prev_timeval.tv_usec += 1000000;
	--prev_timeval.tv_sec;
    }
}

    static void
time_diff(struct timeval *then, struct timeval *now)
{
    long	usec;
    long	msec;

    usec = now->tv_usec - then->tv_usec;
    msec = (now->tv_sec - then->tv_sec) * 1000L + usec / 1000L,
    usec = usec % 1000L;
    fprintf(time_fd, "%03ld.%03ld", msec, usec >= 0 ? usec : usec + 1000L);
}

    void
time_msg(
    char	*mesg,
    void	*tv_start)  // only for do_source: start time; actually
			    // (struct timeval *)
{
    static struct timeval	start;
    struct timeval		now;

    if (time_fd == NULL)
	return;

    if (strstr(mesg, "STARTING") != NULL)
    {
	gettimeofday(&start, NULL);
	prev_timeval = start;
	fprintf(time_fd, "\n\ntimes in msec\n");
	fprintf(time_fd, " clock   self+sourced   self:  sourced script\n");
	fprintf(time_fd, " clock   elapsed:              other lines\n\n");
    }
    gettimeofday(&now, NULL);
    time_diff(&start, &now);
    if (((struct timeval *)tv_start) != NULL)
    {
	fprintf(time_fd, "  ");
	time_diff(((struct timeval *)tv_start), &now);
    }
    fprintf(time_fd, "  ");
    time_diff(&prev_timeval, &now);
    prev_timeval = now;
    fprintf(time_fd, ": %s\n", mesg);
}
# endif	// STARTUPTIME
#endif // FEAT_EVAL

#if defined(FEAT_SPELL) || defined(FEAT_PERSISTENT_UNDO)
/*
 * Read 8 bytes from "fd" and turn them into a time_T, MSB first.
 * Returns -1 when encountering EOF.
 */
    time_T
get8ctime(FILE *fd)
{
    int		c;
    time_T	n = 0;
    int		i;

    for (i = 0; i < 8; ++i)
    {
	c = getc(fd);
	if (c == EOF) return -1;
	n = (n << 8) + c;
    }
    return n;
}


/*
 * Write time_T to "buf[8]".
 */
    void
time_to_bytes(time_T the_time, char_u *buf)
{
    int		c;
    int		i;
    int		bi = 0;
    time_T	wtime = the_time;

    // time_T can be up to 8 bytes in size, more than long_u, thus we
    // can't use put_bytes() here.
    // Another problem is that ">>" may do an arithmetic shift that keeps the
    // sign.  This happens for large values of wtime.  A cast to long_u may
    // truncate if time_T is 8 bytes.  So only use a cast when it is 4 bytes,
    // it's safe to assume that long_u is 4 bytes or more and when using 8
    // bytes the top bit won't be set.
    for (i = 7; i >= 0; --i)
    {
	if (i + 1 > (int)sizeof(time_T))
	    // ">>" doesn't work well when shifting more bits than avail
	    buf[bi++] = 0;
	else
	{
# if defined(SIZEOF_TIME_T) && SIZEOF_TIME_T > 4
	    c = (int)(wtime >> (i * 8));
# else
	    c = (int)((long_u)wtime >> (i * 8));
# endif
	    buf[bi++] = c;
	}
    }
}

#endif

/*
 * Put timestamp "tt" in "buf[buflen]" in a nice format.
 */
    void
add_time(char_u *buf, size_t buflen, time_t tt)
{
#ifdef HAVE_STRFTIME
    struct tm	tmval;
    struct tm	*curtime;
    size_t	n;

    if (vim_time() - tt >= 100)
    {
	curtime = vim_localtime(&tt, &tmval);
	if (vim_time() - tt < (60L * 60L * 12L))
	    // within 12 hours
	    n = strftime((char *)buf, buflen, "%H:%M:%S", curtime);
	else
	    // longer ago
	    n = strftime((char *)buf, buflen, "%Y/%m/%d %H:%M:%S", curtime);
	if (n == 0)
	    buf[0] = NUL;
    }
    else
#endif
    {
	long seconds = (long)(vim_time() - tt);

	vim_snprintf((char *)buf, buflen,
		NGETTEXT("%ld second ago", "%ld seconds ago", seconds),
		seconds);
    }
}
