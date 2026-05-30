#include "config.h"

#include "process_parse.h"

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "fileio.h"
#include "misc.h"
#include "process.h"
#include "screen.h"
#include "lib/time_parse.h"

int MsgOk(void)
{
	return display && !*rc_name;
}

int ParseSwitch(struct action *act, bool *var)
{
	if (*act->args == NULL) {
		*var ^= true;
		return 0;
	}
	return ParseOnOff(act, var);
}

int ParseOnOffMapped(struct action *act, int *var, int off_value, int on_value)
{
	bool b;

	if (ParseOnOff(act, &b) != 0)
		return -1;
	*var = b ? on_value : off_value;
	return 0;
}

int ParseOnOff(struct action *act, bool *var)
{
	int num = -1;
	char **args = act->args;

	if (*args && args[1] == NULL) {
		if (strcmp(args[0], "on") == 0)
			num = true;
		else if (strcmp(args[0], "off") == 0)
			num = false;
	}
	if (num < 0) {
		Msg(0, "%s: %s: invalid argument. Give 'on' or 'off'", rc_name, comms[act->nr].name);
		return -1;
	}
	*var = num;
	return 0;
}

int ParseSaveStr(struct action *act, char **var)
{
	char **args = act->args;

	if (*args == NULL || args[1]) {
		Msg(0, "%s: %s: one argument required.", rc_name, comms[act->nr].name);
		return -1;
	}
	if (*var)
		free(*var);
	*var = SaveStr(*args);
	return 0;
}

int ParseNum(struct action *act, int *var)
{
	int i;
	char *p, **args = act->args;

	p = *args;
	if (p == NULL || *p == 0 || args[1]) {
		Msg(0, "%s: %s: invalid argument. Give one argument.", rc_name, comms[act->nr].name);
		return -1;
	}
	i = 0;
	while (*p) {
		if (*p >= '0' && *p <= '9')
			i = 10 * i + (*p - '0');
		else {
			Msg(0, "%s: %s: invalid argument. Give numeric argument.", rc_name, comms[act->nr].name);
			return -1;
		}
		p++;
	}
	*var = i;
	return 0;
}

int ParseNum1000(struct action *act, int *var)
{
	char *p, **args = act->args;
	uintmax_t seconds = 0;
	unsigned int fraction_digits = 0;
	unsigned int fraction_ms = 0;
	bool have_fraction = false;
	bool saturated = false;

	p = *args;
	if (p == NULL || *p == 0 || args[1]) {
		Msg(0, "%s: %s: invalid argument. Give one argument.", rc_name, comms[act->nr].name);
		return -1;
	}
	while (*p) {
		if (*p >= '0' && *p <= '9') {
			unsigned int digit = (unsigned int)(*p - '0');

			if (!have_fraction) {
				if (seconds <= (UINTMAX_MAX - digit) / 10u)
					seconds = (seconds * 10u) + digit;
				else
					saturated = true;
			} else if (fraction_digits < 3u) {
				fraction_ms = (fraction_ms * 10u) + digit;
				fraction_digits++;
			} else if (fraction_digits == 3u) {
				if (digit >= 5u)
					fraction_ms++;
				fraction_digits = 4u;
			}
		} else if (*p == '.' && !have_fraction) {
			have_fraction = true;
		} else {
			Msg(0, "%s: %s: invalid argument. Give floating point argument.", rc_name, comms[act->nr].name);
			return -1;
		}
		p++;
	}
	while (have_fraction && fraction_digits < 3u) {
		fraction_ms *= 10u;
		fraction_digits++;
	}

	int milliseconds = 0;
	if (saturated || !bx_time_seconds_to_milliseconds_int(seconds, &milliseconds)) {
		milliseconds = INT_MAX;
	} else if (fraction_ms > (unsigned int)(INT_MAX - milliseconds)) {
		milliseconds = INT_MAX;
	} else {
		milliseconds += (int)fraction_ms;
	}
	*var = milliseconds;
	return 0;
}

int ParseWinNum(struct action *act, int *var)
{
	char **args = act->args;
	int i = 0;

	if (*args == NULL || args[1]) {
		Msg(0, "%s: %s: one argument required.", rc_name, comms[act->nr].name);
		return -1;
	}

	i = WindowByNoN(*args);
	if (i < 0) {
		Msg(0, "%s: %s: invalid argument. Give window number or name.", rc_name, comms[act->nr].name);
		return -1;
	}
	*var = i;
	return 0;
}

int ParseBase(struct action *act, char *p, int *var, int base, char *bname)
{
	int i = 0;
	int c;

	if (!p || *p == 0) {
		Msg(0, "%s: %s: empty argument.", rc_name, comms[act->nr].name);
		return -1;
	}
	while ((c = *p++)) {
		if (c >= 'a' && c <= 'z')
			c -= 'a' - 'A';
		if (c >= 'A' && c <= 'Z')
			c -= 'A' - ('0' + 10);
		c -= '0';
		if (c < 0 || c >= base) {
			Msg(0, "%s: %s: argument is not %s.", rc_name, comms[act->nr].name, bname);
			return -1;
		}
		i = base * i + c;
	}
	*var = i;
	return 0;
}

bool IsNum(char *s)
{
	for (; *s; ++s)
		if (*s < '0' || *s > '9')
			return false;
	return true;
}
