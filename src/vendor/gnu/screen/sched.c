/* Copyright (c) 2008, 2009
 *      Juergen Weigert (jnweiger@immd4.informatik.uni-erlangen.de)
 *      Michael Schroeder (mlschroe@immd4.informatik.uni-erlangen.de)
 *      Micah Cowan (micah@cowan.name)
 *      Sadrul Habib Chowdhury (sadrul@users.sourceforge.net)
 * Copyright (c) 1993-2002, 2003, 2005, 2006, 2007
 *      Juergen Weigert (jnweiger@immd4.informatik.uni-erlangen.de)
 *      Michael Schroeder (mlschroe@immd4.informatik.uni-erlangen.de)
 * Copyright (c) 1987 Oliver Laumann
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program (see the file COPYING); if not, see
 * https://www.gnu.org/licenses/, or contact Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA
 *
 ****************************************************************
 */

#include "applets/system/screen/config.h"

#include "sched.h"

#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>
#include <sys/time.h>

#include "lib/time_parse.h"
#include "applets/system/screen/screen.h"

static Event *evs;
static Event *tevs;
static Event *nextev;
static int calctimeout;
static struct pollfd *pfd;
static int pfd_cnt;


static Event *calctimo(void);

void evenq(Event *ev)
{
	int i = 0;

	Event *evp, **evpp;
	if (ev->queued)
		return;
	evpp = &evs;
	if (ev->type == EV_TIMEOUT) {
		calctimeout = 1;
		evpp = &tevs;
	}

	for (; (evp = *evpp); evpp = &evp->next)
		if (ev->priority > evp->priority)
			break;
	ev->next = evp;
	*evpp = ev;
	ev->queued = true;

	/* check if we need more pollfd */
	for (evp = evs; evp; evp = evp->next)
		if (evp->type == EV_READ || evp->type == EV_WRITE)
			i++;

	if (i > pfd_cnt) {
		pfd_cnt = i;
		pfd = realloc(pfd, pfd_cnt * sizeof(struct pollfd));
	}
}

void evdeq(Event *ev)
{
	Event *evp, **evpp;
	if (!ev || !ev->queued)
		return;
	evpp = &evs;
	if (ev->type == EV_TIMEOUT) {
		calctimeout = 1;
		evpp = &tevs;
	}
	for (; (evp = *evpp); evpp = &evp->next)
		if (evp == ev)
			break;
	*evpp = ev->next;
	ev->queued = false;
	if (ev == nextev)
		nextev = nextev->next;

	/* mark fd to be skipped (see checks in sched()) */
	for (int i = 0; i < pfd_cnt; i++)
		if (pfd[i].fd == ev->fd)
			pfd[i].fd = -pfd[i].fd;
}

static Event *calctimo(void)
{
	Event *ev, *min;
	int64_t mins;

	if ((min = tevs) == NULL)
		return NULL;
	mins = min->timeout;
	for (ev = tevs->next; ev; ev = ev->next) {
		if (mins > ev->timeout) {
			min = ev;
			mins = ev->timeout;
		}
	}
	return min;
}

void sched(void)
{
	Event *ev;
	Event *timeoutev = NULL;
	int timeout = 0;
	int i, n;

	for (;;) {
		if (calctimeout)
			timeoutev = calctimo();
		if (timeoutev) {
			struct timeval now;
			int64_t now_ms = 0;
			int64_t remaining_ms = 0;
			gettimeofday(&now, NULL);
			/* tp - timeout */
			if (!bx_time_timeval_to_milliseconds_int64(&now, &now_ms) ||
			    timeoutev->timeout <= now_ms) {
				timeout = 0;
			} else {
				remaining_ms = timeoutev->timeout - now_ms;
				timeout = remaining_ms > INT_MAX ? INT_MAX : (int)remaining_ms;
			}
		}

		memset(pfd, 0, sizeof(struct pollfd) * pfd_cnt);
		i = 0;
		for (ev = evs; ev; ev = ev->next) {
			if (ev->condpos && *ev->condpos <= (ev->condneg ? *ev->condneg : 0))
				goto skip;
			if (ev->type == EV_READ) {
				pfd[i].fd = ev->fd;
				pfd[i].events = POLLIN;
			} else if (ev->type == EV_WRITE) {
				pfd[i].fd = ev->fd;
				pfd[i].events = POLLOUT;
			}
skip:
			if (ev->type == EV_READ || ev->type == EV_WRITE)
				i++;
		}

		n = poll(pfd, i, timeoutev ? timeout : 1000);
		if (n < 0) {
			if (errno != EINTR) {
				Panic(errno, "poll");
			}
			n = 0;
		} else if (n == 0) {	/* timeout */
			if (timeoutev) {
				evdeq(timeoutev);
				timeoutev->handler(timeoutev, timeoutev->data);
			}
		}

		i = 0;

		for (ev = evs; ev; ev = nextev) {
			nextev = ev->next;
			switch (ev->type) {
			case EV_READ:
			case EV_WRITE:
				/* check if we parsed all events from poll()
				 * if we did just continue, as we may still
				 * need to run EV_ALWAYS event */
				if (n == 0)
					continue;
				/* check if we have anything to do for EV_READ
				 * or EV_WRITE, or if event is still queued,
				 * if not skip to the next event */
				if (!pfd[i].revents || pfd[i].fd < 0) {
					i++;
					continue;
				}
				/* this is one of events from poll(), decrease
				 * counter */
				n--;
				/* advance pollfd pointer */
				i++;
				__attribute__ ((fallthrough));
			default:
				if (ev->condpos && *ev->condpos <= (ev->condneg ? *ev->condneg : 0))
					continue;
				ev->handler(ev, ev->data);
			}
		}
	}
}

void SetTimeout(Event *ev, int64_t timo)
{
	struct timeval now;
	int64_t now_ms = 0;
	gettimeofday(&now, NULL);

	if (!bx_time_timeval_to_milliseconds_int64(&now, &now_ms) ||
	    (timo > 0 && now_ms > INT64_MAX - timo) ||
	    (timo < 0 && now_ms < INT64_MIN - timo))
		ev->timeout = 0;
	else
		ev->timeout = now_ms + timo;

	if (ev->queued)
		calctimeout = 1;
}

void SetTimeoutSeconds(Event *ev, int seconds)
{
	int milliseconds = 0;

	if (seconds <= 0 ||
	    !bx_time_seconds_to_milliseconds_int((uintmax_t)seconds, &milliseconds)) {
		SetTimeout(ev, 0);
		return;
	}

	SetTimeout(ev, milliseconds);
}
