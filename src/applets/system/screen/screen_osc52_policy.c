#include <stddef.h>
#include <string.h>

#include "screen.h"
#include "display.h"
#include "screen_osc52_policy.h"

void ScreenOsc52Relay(Window *window, char *payload)
{
	char *semicolon;
	char *selection;
	char *data;
	size_t selection_length;
	size_t data_length;

	if (window == NULL || payload == NULL)
		return;

	/* OSC 52 payload: Pc ; Pd, where Pd == "?" requests a read. */
	selection = payload;
	semicolon = strchr(selection, ';');
	if (semicolon == NULL)
		return;
	selection_length = semicolon - selection;
	data = semicolon + 1;
	data_length = strlen(data);

	/*
	 * Clipboard reads stay hard-blocked. Serving one would require
	 * correlating a response with the originating display and window.
	 */
	if (data_length == 1 && data[0] == '?')
		return;
	if (!defosc52)
		return;

	for (display = displays; display; display = display->d_next) {
		if (D_forecv->c_layer->l_bottom == &window->w_layer)
			break;
	}
	if (display == NULL)
		return;

	DisplayOSC52(
		display,
		selection,
		selection_length,
		data,
		data_length
	);
}
