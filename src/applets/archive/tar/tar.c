#include <stdio.h>
#include <string.h>

#include "applets/archive/archive_temp.h"
#include "applets/archive/tar/tar_backend.h"
#include "dispatch/applets.h"
#include "lib/cli_common.h"

static void bx_tar_print_usage(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s -cf ARCHIVE [OPTION]... FILE...\n", progname);
    fprintf(stream, "       %s -Af ARCHIVE [OPTION]... SOURCE_ARCHIVE...\n", progname);
    fprintf(stream, "       %s -tf ARCHIVE [OPTION]... [MEMBER...]\n", progname);
    fprintf(stream, "       %s -xf ARCHIVE [OPTION]... [MEMBER...]\n", progname);
}

static void bx_tar_print_help(FILE* stream, const char* progname) {
    bx_tar_print_usage(stream, progname);
    fprintf(stream, "Manipulate tar archives.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -c                    create a new archive\n");
    fprintf(stream, "  -t                    list archive members\n");
    fprintf(stream, "  -x                    extract archive members\n");
    fprintf(stream, "  -f ARCHIVE            use ARCHIVE instead of standard input/output\n");
    fprintf(stream, "  -C DIR                change to DIR before processing files\n");
    fprintf(stream, "  -T FILE               read create inputs from FILE\n");
    fprintf(stream, "      --add-file=FILE   add FILE even when its name begins with '-'\n");
    fprintf(stream, "      --null            read -T input as NUL-delimited names\n");
    fprintf(stream, "      --verbatim-files-from\n");
    fprintf(stream, "                        read -T names literally without option parsing\n");
    fprintf(stream, "      --no-unquote      disable backslash escape decoding in -T names\n");
    fprintf(stream, "  -X FILE               exclude create inputs matching patterns from FILE\n");
    fprintf(stream, "      --exclude=PATTERN skip matching paths during archive creation\n");
    fprintf(stream, "      --no-recursion    archive directory entries without descending\n");
    fprintf(stream, "      --remove-files    delete archived source files after success\n");
    fprintf(stream, "  -O                    write extracted file data to standard output\n");
    fprintf(stream, "  -A                    append members from source archives\n");
    fprintf(stream, "  -d                    compare archive members against the filesystem\n");
    fprintf(stream, "  -r                    append files to an existing archive\n");
    fprintf(stream, "  -u                    append files only when newer than the archived copy\n");
    fprintf(stream, "      --delete          remove named members from an archive\n");
    fprintf(stream, "      --test-label      print or match the archive volume label\n");
    fprintf(stream, "      --occurrence[=N]  process only the Nth occurrence of each named member\n");
    fprintf(stream, "  -k, --keep-old-files  do not overwrite existing files; report an error\n");
    fprintf(stream, "      --skip-old-files  do not overwrite existing files; continue\n");
    fprintf(stream, "      --keep-newer-files\n");
    fprintf(stream, "                        preserve files newer than their archive copies\n");
    fprintf(stream, "      --same-owner      restore archive ownership when permitted\n");
    fprintf(stream, "      --no-same-owner   do not restore archive ownership\n");
    fprintf(stream, "      --no-seek         archive is not seekable\n");
    fprintf(stream, "  -n, --seek            archive is seekable\n");
    fprintf(stream, "  -v                    report processed member names\n");
    fprintf(stream, "      --index-file=FILE write listing or verbose output to FILE\n");
    fprintf(stream, "      --block-number    prefix archive-read reports with parser block numbers\n");
    fprintf(stream, "      --totals          write final byte totals to standard error\n");
    fprintf(stream, "      --show-transformed-names\n");
    fprintf(stream, "                        report rewritten member names when listing or extracting\n");
    fprintf(stream, "      --show-stored-names\n");
    fprintf(stream, "                        GNU-compatible alias for transformed report names\n");
    fprintf(stream, "  -N DATE               include only files newer than DATE during filesystem input collection\n");
    fprintf(stream, "      --newer=DATE      compare by status change or modification time\n");
    fprintf(stream, "      --newer-mtime=DATE\n");
    fprintf(stream, "                        compare by modification time only\n");
    fprintf(stream, "  -K FILE               start list or extract at member FILE\n");
    fprintf(stream, "  -j                    filter the archive through bzip2\n");
    fprintf(stream, "  -z                    filter the archive through gzip\n");
    fprintf(stream, "  -J                    filter the archive through xz\n");
    fprintf(stream, "      --zstd           filter the archive through zstd\n");
    fprintf(stream, "  -a                    choose compression from the archive suffix\n");
    fprintf(stream, "      --threads=N       use N gzip compression workers (0 uses online CPUs)\n");
    fprintf(stream, "      --compress-threads=N\n");
    fprintf(stream, "                        override gzip compression workers for archive output\n");
    fprintf(stream, "      --mt-chunk-size=SIZE\n");
    fprintf(stream, "                        set multithreaded gzip member chunk size\n");
    fprintf(stream, "      --no-mt           disable multithreaded gzip archive output\n");
    fprintf(stream, "\n");
    fprintf(stream, "Regular archive-file output uses best-effort staged-temp cleanup on HUP/INT/TERM.\n");
    fprintf(stream, "SIGKILL cannot be intercepted, so it may still leave a staged temp file behind.\n");
    fprintf(stream, "\n");
    fprintf(stream, "      --help            display this help and exit\n");
    fprintf(stream, "      --version         output version information and exit\n");
}

static int bx_tar_maybe_handle_usage(int argc, char** argv) {
    const char* progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "tar");

    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];

        if (arg == NULL) {
            continue;
        }
        if (strcmp(arg, "--") == 0) {
            break;
        }
        if (strcmp(arg, "--usage") == 0) {
            bx_tar_print_usage(stdout, progname);
            return 0;
        }
    }

    return -1;
}

int bx_tar_main(int argc, char** argv) {
    int rc;
    int pending_signal;

    int handled = bx_cli_maybe_handle_help_or_version(argc, argv, "tar", "-?", NULL, bx_tar_print_help);
    if (handled >= 0) {
        return handled;
    }

    handled = bx_tar_maybe_handle_usage(argc, argv);
    if (handled >= 0) {
        return handled;
    }

    if (!bx_archive_temp_install_signal_cleanup()) {
        fprintf(stderr, "%s: failed to install archive temp signal cleanup\n",
                bx_cli_progname((argc > 0) ? argv[0] : NULL, "tar"));
        return 2;
    }

    rc = bx_tar_run(argc, argv);
    pending_signal = bx_archive_temp_pending_signal();
    if (pending_signal != 0) {
        bx_archive_temp_cleanup_all();
        (void)bx_cancel_state_mark_joined(bx_archive_temp_cancel_state());
        (void)bx_cancel_state_mark_published(bx_archive_temp_cancel_state());
        bx_archive_temp_clear_pending_signal();
        return 128 + pending_signal;
    }
    return rc;
}
