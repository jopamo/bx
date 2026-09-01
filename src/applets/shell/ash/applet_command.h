#ifndef BX_APPLETS_SHELL_ASH_APPLET_COMMAND_H
#define BX_APPLETS_SHELL_ASH_APPLET_COMMAND_H

#include <stddef.h>
#include <sys/types.h>

struct ash_command_resolution;
struct ash_shell;

/*
 * A plan is prepared in the shell process and copied across fork. The
 * originating PID is the witness that run_child is no longer executing in
 * the parent shell. invocation_process makes each child copy single-use, so
 * applet-global mutations can never cross direct invocations.
 */
struct ash_applet_child_plan {
    pid_t origin_process;
    pid_t invocation_process;
};

/*
 * Flush parent-owned stdio before a direct applet fork so the child cannot
 * emit inherited buffered shell output a second time. This is a no-op when
 * the target will cross an exec boundary.
 */
int ash_applet_command_prepare_fork(
    struct ash_shell* shell,
    size_t argc,
    char** argv,
    const struct ash_command_resolution* resolution,
    struct ash_applet_child_plan* plan
);

/*
 * Run one bx applet target in an already forked child. Eligibility comes only
 * from immutable dispatch execution policy. Every ineligible target exact-
 * execs resolution->fallback_path; this function never performs PATH lookup
 * and rejects use in the plan's originating shell process.
 */
int ash_applet_command_run_child(
    struct ash_shell* shell,
    size_t argc,
    char** argv,
    const struct ash_command_resolution* resolution,
    struct ash_applet_child_plan* plan
);

#endif /* BX_APPLETS_SHELL_ASH_APPLET_COMMAND_H */
