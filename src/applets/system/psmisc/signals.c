#include <stdio.h>
#include <stdlib.h>

#include "applets/system/psmisc/i18n.h"
#include "applets/system/psmisc/signals.h"
#include "lib/signal_names.h"

void list_signals(void) {
    bx_signal_name_list(stdout);
}

int get_signal(char* name, const char* cmd) {
    int number = 0;

    if (bx_signal_name_lookup(name, &number)) {
        return number;
    }

    fprintf(stderr, _("%s: unknown signal; %s -l lists signals.\n"), name, cmd);
    exit(1);
}
