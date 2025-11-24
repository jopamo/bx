#include "config.h"

#include "process_resize_prompt.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "input.h"
#include "process.h"
#include "resize.h"
#include "screen.h"
#include "viewport.h"

static char *resizeprompts[] = {
	"resize # lines: ",
	"resize -h # columns: ",
	"resize -v # lines: ",
	"resize -b # columns: ",
	"resize -l # lines: ",
	"resize -l -h # columns: ",
	"resize -l -v # lines: ",
	"resize -l -b # columns: ",
};

const char *ResizePrompt(int flags)
{
	return resizeprompts[flags];
}

static int CalcSlicePercent(Canvas *cv, int percent)
{
	int w, wsum, up;

	if (!cv || !cv->c_slback)
		return percent;
	up = CalcSlicePercent(cv->c_slback->c_slback, percent);
	w = cv->c_slweight;
	for (cv = cv->c_slback->c_slperp, wsum = 0; cv; cv = cv->c_slnext)
		wsum += cv->c_slweight;
	if (wsum == 0)
		return 0;
	return (up * w) / wsum;
}

static int ChangeCanvasSize(Canvas *fcv, int abs, int diff, bool gflag, int percent)
{
	Canvas *cv;
	int done, have, m, dir;

	if (abs == 0 && diff == 0)
		return 0;
	if (abs == 2) {
		if (diff == 0)
			fcv->c_slweight = 0;
		else {
			for (cv = fcv->c_slback->c_slperp; cv; cv = cv->c_slnext)
				cv->c_slweight = 0;
			fcv->c_slweight = 1;
			cv = fcv->c_slback->c_slback;
			if (gflag && cv && cv->c_slback)
				ChangeCanvasSize(cv, abs, diff, gflag, percent);
		}
		return diff;
	}
	if (abs) {
		if (diff < 0)
			diff = 0;
		if (percent && diff > percent)
			diff = percent;
	}
	if (percent) {
		int wsum, up;

		for (cv = fcv->c_slback->c_slperp, wsum = 0; cv; cv = cv->c_slnext)
			wsum += cv->c_slweight;
		if (wsum) {
			up = gflag ? CalcSlicePercent(fcv->c_slback->c_slback, percent) : percent;
			if (wsum < 1000) {
				int scale = wsum < 10 ? 1000 : 100;

				for (cv = fcv->c_slback->c_slperp; cv; cv = cv->c_slnext)
					cv->c_slweight *= scale;
				wsum *= scale;
			}
			for (cv = fcv->c_slback->c_slperp; cv; cv = cv->c_slnext) {
				if (cv->c_slweight) {
					cv->c_slweight = (cv->c_slweight * up) / percent;
					if (cv->c_slweight == 0)
						cv->c_slweight = 1;
				}
			}
			diff = (diff * wsum) / percent;
			percent = wsum;
		}
	} else {
		if (abs
		    && diff == (fcv->c_slorient == SLICE_VERT ? fcv->c_ye - fcv->c_ys + 2 : fcv->c_xe - fcv->c_xs + 2))
			return 0;
		for (cv = fcv->c_slback->c_slperp; cv; cv = cv->c_slnext) {
			cv->c_slweight =
			    cv->c_slorient == SLICE_VERT ? cv->c_ye - cv->c_ys + 2 : cv->c_xe - cv->c_xs + 2;
		}
	}
	if (abs)
		diff = diff - fcv->c_slweight;
	if (diff == 0)
		return 0;
	if (diff < 0) {
		cv = fcv->c_slnext ? fcv->c_slnext : fcv->c_slprev;
		fcv->c_slweight += diff;
		cv->c_slweight -= diff;
		return diff;
	}
	done = 0;
	dir = 1;
	for (cv = fcv->c_slnext; diff > 0; cv = dir > 0 ? cv->c_slnext : cv->c_slprev) {
		if (!cv) {
			if (dir == -1)
				break;
			dir = -1;
			cv = fcv;
			continue;
		}
		if (percent)
			m = 1;
		else
			m = cv->c_slperp ? CountCanvasPerp(cv) * 2 : 2;
		if (cv->c_slweight > m) {
			have = cv->c_slweight - m;
			if (have > diff)
				have = diff;
			cv->c_slweight -= have;
			done += have;
			diff -= have;
		}
	}
	if (diff && gflag) {
		cv = fcv->c_slback->c_slback;
		if (cv && cv->c_slback)
			done += ChangeCanvasSize(fcv->c_slback->c_slback, 0, diff, gflag, percent);
	}
	fcv->c_slweight += done;
	return done;
}

void ResizeRegions(char *arg, int flags)
{
	Canvas *cv;
	int diff, l;
	bool gflag = 0;
	int abs = 0, percent = 0;
	int orient = 0;

	if (!*arg)
		return;
	if (D_forecv->c_slorient == SLICE_UNKN) {
		Msg(0, "resize: need more than one region");
		return;
	}
	gflag = flags & RESIZE_FLAG_L ? 0 : 1;
	orient |= flags & RESIZE_FLAG_H ? SLICE_HORI : 0;
	orient |= flags & RESIZE_FLAG_V ? SLICE_VERT : 0;
	if (orient == 0)
		orient = D_forecv->c_slorient;
	l = strlen(arg);
	if (*arg == '=') {
		Canvas *equalize_cv = gflag ? &D_canvas : D_forecv->c_slback;

		if (equalize_cv->c_slperp->c_slorient & orient)
			EqualizeCanvas(equalize_cv->c_slperp, gflag);
		if ((equalize_cv->c_slperp->c_slorient ^ (SLICE_HORI ^ SLICE_VERT)) & orient) {
			if (equalize_cv->c_slback) {
				equalize_cv = equalize_cv->c_slback;
				EqualizeCanvas(equalize_cv->c_slperp, gflag);
			} else
				EqualizeCanvas(equalize_cv, gflag);
		}
		ResizeCanvas(equalize_cv);
		RecreateCanvasChain();
		RethinkDisplayViewports();
		ResizeLayersToCanvases();
		return;
	}
	if (!strcmp(arg, "min") || !strcmp(arg, "0")) {
		abs = 2;
		diff = 0;
	} else if (!strcmp(arg, "max") || !strcmp(arg, "_")) {
		abs = 2;
		diff = 1;
	} else {
		if (l > 0 && arg[l - 1] == '%')
			percent = 1000;
		if (*arg == '+')
			diff = atoi(arg + 1);
		else if (*arg == '-')
			diff = -atoi(arg + 1);
		else {
			diff = atoi(arg);
			if (diff < 0)
				diff = 0;
			abs = diff == 0 ? 2 : 1;
		}
	}
	if (!abs && !diff)
		return;
	if (percent)
		diff = diff * percent / 100;
	cv = D_forecv;
	if (cv->c_slorient & orient)
		ChangeCanvasSize(cv, abs, diff, gflag, percent);
	if (cv->c_slback->c_slorient & orient)
		ChangeCanvasSize(cv->c_slback, abs, diff, gflag, percent);

	ResizeCanvas(&D_canvas);
	RecreateCanvasChain();
	RethinkDisplayViewports();
	ResizeLayersToCanvases();
}

void ResizeFin(char *buf, size_t len, void *data)
{
	int ch;
	int flags = *(int *)data;

	ch = ((unsigned char *)buf)[len];
	if (ch == 0) {
		ResizeRegions(buf, flags);
		return;
	}
	if (ch == 'h')
		flags ^= RESIZE_FLAG_H;
	else if (ch == 'v')
		flags ^= RESIZE_FLAG_V;
	else if (ch == 'b')
		flags |= RESIZE_FLAG_H | RESIZE_FLAG_V;
	else if (ch == 'p')
		flags ^= D_forecv->c_slorient == SLICE_VERT ? RESIZE_FLAG_H : RESIZE_FLAG_V;
	else if (ch == 'l')
		flags ^= RESIZE_FLAG_L;
	else
		return;
	inp_setprompt(resizeprompts[flags], NULL);
	*(int *)data = flags;
	buf[len] = '\034';
}
