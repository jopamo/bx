#define _GNU_SOURCE

#include "lib/utmp_ops.h"

#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <utmpx.h>

#include "lib/fd_ops.h"

#ifndef _PATH_UTMP
#define _PATH_UTMP "/var/run/utmp"
#endif

#ifndef _PATH_WTMP
#define _PATH_WTMP "/var/log/wtmp"
#endif

static void bx_utmp_ensure_file(void) {
    if (access(_PATH_UTMP, R_OK | W_OK) == 0)
        return;
    int fd = bx_fd_open_cloexec(
        _PATH_UTMP, O_WRONLY | O_CREAT, 0664);
    if (fd >= 0)
        close(fd);
}

void bx_utmp_mark_dead(pid_t pid) {
    if (pid <= 0)
        return;

    int saved_errno = errno;
    bx_utmp_ensure_file();
    setutxent();

    struct utmpx dead;
    bool found = false;
    struct utmpx *record;
    while ((record = getutxent()) != NULL) {
        if (record->ut_pid != pid || record->ut_id[0] == '\0')
            continue;
        if (record->ut_type != INIT_PROCESS &&
            record->ut_type != LOGIN_PROCESS &&
            record->ut_type != USER_PROCESS &&
            record->ut_type != DEAD_PROCESS)
            continue;

        dead = *record;
        if (record->ut_type >= DEAD_PROCESS)
            memset(dead.ut_host, 0, sizeof(dead.ut_host));
        dead.ut_type = DEAD_PROCESS;
        dead.ut_tv.tv_sec = time(NULL);
        (void)pututxline(&dead);
        found = true;
        break;
    }
    endutxent();
#if BX_HAVE_UPDWTMPX
    if (found)
        updwtmpx(_PATH_WTMP, &dead);
#else
    (void)found;
#endif
    errno = saved_errno;
}
