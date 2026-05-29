#ifndef BX_LIB_STATUS_H
#define BX_LIB_STATUS_H

#include <stdbool.h>

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
    if (applet_main == NULL) {
        return BX_STATUS_ERROR;
    }
    return bx_status_from_applet(applet_main(argc, argv));
}

#endif /* BX_LIB_STATUS_H */
