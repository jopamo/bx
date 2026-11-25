#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/option_scan.h"
#include "lib/path_ops.h"

static void bx_dirname_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION] NAME...\n", progname);
    fprintf(stream, "Output each NAME with its last non-slash component and trailing slashes\n");
    fprintf(stream, "removed; if NAME contains no /'s, output '.' (meaning the current directory).\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -z, --zero\n");
    fprintf(stream, "         end each output line with NUL, not newline\n");
    fprintf(stream, "      --help\n");
    fprintf(stream, "         display this help and exit\n");
    fprintf(stream, "      --version\n");
    fprintf(stream, "         output version information and exit\n");
    fprintf(stream, "\n");
    fprintf(stream, "Examples:\n");
    fprintf(stream, "  %s /usr/bin/          -> \"/usr\"\n", progname);
    fprintf(stream, "  %s dir1/str dir2/str  -> \"dir1\" followed by \"dir2\"\n", progname);
    fprintf(stream, "  %s stdio.h            -> \".\"\n", progname);
}

enum {
    BX_DIRNAME_OPT_HELP = 256,
    BX_DIRNAME_OPT_VERSION,
};

int bx_dirname_main(int argc, char** argv) {
    static const struct bx_option_spec specs[] = {
        {'z', 'z', "zero", BX_OPTION_NO_VALUE},
        {BX_DIRNAME_OPT_HELP, 0, "help", BX_OPTION_NO_VALUE},
        {BX_DIRNAME_OPT_VERSION, 0, "version", BX_OPTION_NO_VALUE},
    };
    struct bx_diag_ctx diag = {
        .progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "dirname"),
    };
    const char** names = argc > 1 ? calloc((size_t)(argc - 1), sizeof(*names)) : NULL;
    if (argc > 1 && names == NULL) {
        bx_diag(&diag, "memory exhausted");
        return diag.exit_status;
    }
    size_t count = 0;
    bool zero = false;
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, argc > 0 ? 1u : 0u, (size_t)argc,
                          BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE, specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_OPERAND) {
            names[count++] = option.token;
            continue;
        }
        if (option.event != BX_OPTION_MATCH) {
            if (option.short_name)
                bx_diag(&diag, "invalid option -- '%c'", option.short_name);
            else
                bx_diag(&diag, "unrecognized option '%s'", option.token);
            goto done;
        }

        switch (option.spec->id) {
            case 'z':
                zero = true;
                break;
            case BX_DIRNAME_OPT_HELP:
                bx_dirname_print_help(stdout, diag.progname);
                goto done;
            case BX_DIRNAME_OPT_VERSION:
                bx_cli_print_version(diag.progname);
                goto done;
        }
    }

    if (count == 0) {
        bx_cli_diag_missing_operand(&diag);
        goto done;
    }

    for (size_t i = 0; i < count; i++) {
        char* value = bx_path_dirname_dup(names[i]);
        bool ok = bx_cli_emit_line(value, zero, &diag);
        free(value);
        if (!ok)
            goto done;
    }

    bx_cli_flush_stdout(&diag);

done:
    free(names);
    return diag.exit_status;
}
