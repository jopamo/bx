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
    fprintf(stream, "  -o, --create          create an archive from standard input names\n");
    fprintf(stream, "  -i, --extract         extract files from an archive\n");
    fprintf(stream, "  -p, --pass-through    pass files through to a target directory\n");
    fprintf(stream, "  -t, --list            list archive members (implies -i)\n");
    fprintf(stream, "  -d, --make-directories  create leading directories as needed\n");
    fprintf(stream, "  -m, --preserve-modification-time  preserve mtime during filesystem copy\n");
    fprintf(stream, "  -0, --null            read NUL-terminated input names (-o or -p)\n");
    fprintf(stream, "  -F, --file=ARCHIVE    use ARCHIVE instead of standard input/output (-o or -i)\n");
    fprintf(stream, "  -H, --format=FORMAT   create or require newc, odc, crc, or bin format (-o or -i)\n");
    fprintf(stream, "  -L, --dereference     follow selected source symlinks (-o or -p)\n");
    fprintf(stream, "  -u, --unconditional   replace existing files regardless of age\n");
    fprintf(stream, "  -R, --owner=USER[:GROUP]  override ownership (-o, extraction, or -p)\n");
    fprintf(stream, "      --no-preserve-owner  do not restore source/archive ownership\n");
    fprintf(stream, "      --reproducible    normalize identities, directory links and ownership (-o)\n");
    fprintf(stream, "      --quiet           suppress the copied-blocks summary\n");
    fprintf(stream, "      --to-stdout       write selected file data to standard output (-i, no -t)\n");
    fprintf(stream, "      --sparse          recreate holes during filesystem copy\n");
    fprintf(stream, "      --preserve-all    strictly preserve source metadata during pass-through (-p)\n");
    fprintf(stream, "      --help            display this help and exit\n");
    fprintf(stream, "      --version         output version information and exit\n");
    fprintf(stream, "\nOutput defaults to newc. Input is autodetected unless -H requires a format.\n");
    fprintf(stream, "Without -u, newer or equal-age files are retained. Root restores ownership\n");
    fprintf(stream, "unless --no-preserve-owner is selected. Archive modes preserve only cpio fields,\n");
    fprintf(stream, "not ACLs, xattrs, labels, capabilities, flags, or fractional timestamps.\n");
    fprintf(stream, "Pass-through --preserve-all copies semantic metadata directly, without an archive.\n");
}

int bx_cpio_main(int argc, char** argv) {
    int handled = bx_cli_maybe_handle_help_or_version(argc, argv, "cpio", NULL, NULL, bx_cpio_print_help);
    if (handled >= 0) {
        return handled;
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
