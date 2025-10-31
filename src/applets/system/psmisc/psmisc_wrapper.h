#ifndef BX_APPLETS_SYSTEM_PSMISC_WRAPPER_H
#define BX_APPLETS_SYSTEM_PSMISC_WRAPPER_H

#include <stdio.h>

typedef void (*bx_psmisc_help_fn)(FILE* stream, const char* progname);

const char* bx_psmisc_progname(const char* argv0, const char* fallback);
int bx_psmisc_maybe_handle_help_or_version(int argc, char** argv, const char* fallback,
                                           const char* short_help_opt, bx_psmisc_help_fn print_help);

#endif /* BX_APPLETS_SYSTEM_PSMISC_WRAPPER_H */
