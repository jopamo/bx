#include <stdio.h>

#include "applets/archive/cpio/cpio_backend.h"
#include "applets/archive/archive_temp.h"
#include "dispatch/applets.h"
#include "lib/cli_common.h"

static void bx_cpio_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s -o [OPTION]... < NAME-LIST > ARCHIVE\n", progname);
    fprintf(stream, "       %s -i [OPTION]... [PATTERN...]\n", progname);
    fprintf(stream, "       %s -p [OPTION]... DIRECTORY < NAME-LIST\n", progname);
    fprintf(stream, "Copy files to and from cpio archives.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -o                    create an archive from standard input names\n");
    fprintf(stream, "  -i                    extract files from an archive\n");
    fprintf(stream, "  -p                    pass files through to a target directory\n");
    fprintf(stream, "  -t                    list archive members (with -i)\n");
    fprintf(stream, "  -d                    create leading directories as needed\n");
    fprintf(stream, "  -m                    preserve modification times during filesystem copy\n");
    fprintf(stream, "  -0, --null            read NUL-terminated input names (-o or -p)\n");
    fprintf(stream, "  -F, --file=ARCHIVE    use ARCHIVE instead of standard input/output (-o or -i)\n");
    fprintf(stream, "  -H, --format=FORMAT   create newc (default) or odc archives (-o only)\n");
    fprintf(stream, "  -R UID:GID            override numeric ownership (-o, extraction, or -p)\n");
    fprintf(stream, "      --reproducible    normalize identities, directory links and ownership (-o)\n");
    fprintf(stream, "      --quiet           suppress the copied-blocks summary\n");
    fprintf(stream, "      --to-stdout       write selected file data to standard output (-i, no -t)\n");
    fprintf(stream, "      --sparse          recreate holes during filesystem copy\n");
    fprintf(stream, "      --help            display this help and exit\n");
    fprintf(stream, "      --version         output version information and exit\n");
    fprintf(stream, "\nInput format is detected automatically. Archive modes preserve only cpio fields,\n");
    fprintf(stream, "not ACLs, xattrs, labels, capabilities, flags, or fractional timestamps.\n");
    fprintf(stream, "--preserve-all and other formats are unsupported.\n");
}

int bx_cpio_main(int argc, char** argv) {
    int handled = bx_cli_maybe_handle_help_or_version(argc, argv, "cpio", NULL, NULL, bx_cpio_print_help);
    if (handled >= 0) {
        return handled;
    }

    if (!bx_archive_temp_install_signal_cleanup()) {
        fprintf(stderr, "%s: failed to install archive temp signal cleanup\n", bx_cli_progname(argc > 0 ? argv[0] : NULL, "cpio"));
        return 2;
    }
    int rc = bx_cpio_run(argc, argv);
    int pending_signal = bx_archive_temp_pending_signal();
    if (pending_signal) {
        bx_archive_temp_cleanup_all();
        (void)bx_cancel_state_mark_joined(bx_archive_temp_cancel_state());
        (void)bx_cancel_state_mark_published(bx_archive_temp_cancel_state());
        bx_archive_temp_clear_pending_signal();
        return 128 + pending_signal;
    }
    return rc;
}
