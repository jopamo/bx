#include "config.h"

#include "process_lookup.h"

#include <string.h>

#include "screen.h"

Window *WindowByName(char *s)
{
	Window *window;

	for (window = mru_window; window; window = window->w_prev_mru)
		if (!strcmp(window->w_title, s))
			return window;
	for (window = mru_window; window; window = window->w_prev_mru)
		if (!strncmp(window->w_title, s, strlen(s)))
			return window;
	return NULL;
}

int WindowByNumber(char *string)
{
	int i;
	char *s;

	for (i = 0, s = string; *s; s++) {
		if (*s < '0' || *s > '9')
			break;
		i = i * 10 + (*s - '0');
	}
	return *s ? -1 : i;
}

int WindowByNoN(char *string)
{
	int i;
	Window *window;

	if ((i = WindowByNumber(string)) < 0 || i > last_window->w_number) {
		if ((window = WindowByName(string)))
			return window->w_number;
		return -1;
	}
	return i;
}

int IsNumColon(char *s, char *p, int psize)
{
	char *q;

	if ((q = strrchr(s, ':')) != NULL) {
		strncpy(p, q + 1, psize - 1);
		p[psize - 1] = '\0';
		*q = '\0';
	} else
		*p = '\0';

	for (; *s; ++s)
		if (*s < '0' || *s > '9')
			return 0;
	return 1;
}
