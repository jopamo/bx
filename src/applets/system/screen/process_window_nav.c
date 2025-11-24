#include "config.h"

#include "process_window_nav.h"

Window *NextWindow(void)
{
	Window *w;
	Window *group = fore ? fore->w_group : NULL;

	for (w = fore ? fore->w_next : first_window; w != fore; w = w->w_next) {
		if (w == NULL)
			w = first_window;
		if (!fore || group == w->w_group)
			break;
	}
	return w;
}

Window *PreviousWindow(void)
{
	Window *w;
	Window *group = fore ? fore->w_group : NULL;

	for (w = fore ? fore->w_prev : last_window; w != fore; w = w->w_prev) {
		if (w == NULL)
			w = last_window;
		if (!fore || group == w->w_group)
			break;
	}
	return w;
}

Window *ParentWindow(void)
{
	Window *w = fore ? fore->w_group : NULL;
	return w;
}

int MoreWindows(void)
{
	char *m = "No other window.";

	if (mru_window && (fore == NULL || mru_window->w_prev_mru))
		return 1;
	if (fore == NULL) {
		Msg(0, "No window available");
		return 0;
	}
	Msg(0, m, fore->w_number);
	return 0;
}
