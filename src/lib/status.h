#ifndef BX_LIB_STATUS_H
#define BX_LIB_STATUS_H

#include <stdbool.h>

#include "lib/args_common.h"
#include "lib/output_alloc_counter.h"

enum bx_status_code {
    BX_STATUS_OK = 0,
    BX_STATUS_ERROR = 1,
    BX_STATUS_USAGE = 2,
    BX_STATUS_CANNOT_EXEC = 126,
    BX_STATUS_NOT_FOUND = 127,
};

static inline int bx_status_success(void) {
    return BX_STATUS_OK;
}

static inline int bx_status_error(void) {
    return BX_STATUS_ERROR;
}

static inline int bx_status_usage(void) {
    return BX_STATUS_USAGE;
}

static inline int bx_status_from_bool(bool ok) {
    return ok ? BX_STATUS_OK : BX_STATUS_ERROR;
}

static inline int bx_status_from_applet(int status) {
    if (status >= 0 && status <= 255) {
        return status;
    }
    return BX_STATUS_ERROR;
}

typedef int (*bx_status_applet_main_fn)(int argc, char** argv);

static inline int bx_status_run_applet(bx_status_applet_main_fn applet_main, int argc, char** argv) {
    const char* applet_name = (argc > 0 && argv != NULL) ? argv[0] : NULL;
    int status;

    if (applet_main == NULL) {
        return BX_STATUS_ERROR;
    }

    /*
     * Applet entry is the process-global getopt ownership boundary. Reset on
     * both sides so an early parser return cannot contaminate this invocation
     * or any later trusted caller in the same process.
     */
    bx_args_getopt_reset();
    bx_output_alloc_counter_begin_from_env(applet_name);
    status = bx_status_from_applet(applet_main(argc, argv));
    bx_output_alloc_counter_report_stderr();
    bx_output_alloc_counter_reset();
    bx_args_getopt_reset();
    return status;
}

#endif /* BX_LIB_STATUS_H */
