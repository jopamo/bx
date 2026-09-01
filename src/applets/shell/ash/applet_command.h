#ifndef BX_APPLETS_SHELL_ASH_APPLET_COMMAND_H
#define BX_APPLETS_SHELL_ASH_APPLET_COMMAND_H

#include <stddef.h>

struct ash_command_resolution;
struct ash_shell;

/*
 * Flush parent-owned stdio before a direct applet fork so the child cannot
 * emit inherited buffered shell output a second time. This is a no-op when
 * the target will cross an exec boundary.
 */
int ash_applet_command_prepare_fork(
    struct ash_shell* shell,
    size_t argc,
    char** argv,
    const struct ash_command_resolution* resolution
);

/*
 * Run one bx applet target in an already forked child. Eligibility comes only
 * from immutable dispatch execution policy. Every ineligible target exact-
 * execs resolution->fallback_path; this function never performs PATH lookup.
 */
int ash_applet_command_run_child(
    struct ash_shell* shell,
    size_t argc,
    char** argv,
    const struct ash_command_resolution* resolution
);

#endif /* BX_APPLETS_SHELL_ASH_APPLET_COMMAND_H */
