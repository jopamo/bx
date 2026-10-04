#ifndef BX_APPLETS_ARCHIVE_TAR_BACKEND_H
#define BX_APPLETS_ARCHIVE_TAR_BACKEND_H

/* Frontend actions are distinct from process exit statuses. */
enum bx_tar_frontend_action {
    BX_TAR_PRINT_HELP = 256,
    BX_TAR_PRINT_USAGE,
    BX_TAR_PRINT_VERSION,
};

int bx_tar_run(int argc, char** argv);

#endif /* BX_APPLETS_ARCHIVE_TAR_BACKEND_H */
