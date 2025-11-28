#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "applets.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"

enum {
    BX_CHGRP_OPT_HELP = 1,
    BX_CHGRP_OPT_VERSION,
    BX_CHGRP_OPT_REFERENCE,
    BX_CHGRP_OPT_FROM,
    BX_CHGRP_OPT_DEREFERENCE,
    BX_CHGRP_OPT_NO_PRESERVE_ROOT,
    BX_CHGRP_OPT_PRESERVE_ROOT,
};

struct bx_chgrp_parse_result {
    bool ok;
    bool show_help;
    bool show_version;
    bool reference_mode;
    int group_operand_index;
};

static void bx_chgrp_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... GROUP FILE...\n", progname);
    fprintf(stream, "  or:  %s [OPTION]... --reference=RFILE FILE...\n", progname);
    fprintf(stream, "Change the group of each FILE to GROUP.\n");
    fprintf(stream, "With --reference, change the group of each FILE to that of RFILE.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -c, --changes                 report only when a change is made\n");
    fprintf(stream, "  -f, --silent, --quiet         suppress most diagnostics\n");
    fprintf(stream, "  -v, --verbose                 report each processed path\n");
    fprintf(stream, "  -h, --no-dereference          change symlink ownership itself\n");
    fprintf(stream, "      --dereference             change symlink referent (default)\n");
    fprintf(stream, "      --from=OWNER:GROUP        apply only when current owner/group match\n");
    fprintf(stream, "      --reference=RFILE         copy group from RFILE\n");
    fprintf(stream, "      --preserve-root           fail on recursive '/'\n");
    fprintf(stream, "      --no-preserve-root        allow recursive '/'\n");
    fprintf(stream, "  -R, --recursive               recurse into directories\n");
    fprintf(stream, "  -H                            follow command-line symlink dirs with -R\n");
    fprintf(stream, "  -L                            follow all symlink dirs with -R\n");
    fprintf(stream, "  -P                            follow no symlink dirs with -R (default)\n");
    fprintf(stream, "      --help                    display this help and exit\n");
    fprintf(stream, "      --version                 output version information and exit\n");
    fprintf(stream, "\n");
    fprintf(stream, "Examples:\n");
    fprintf(stream, "  %s staff /u      Change the group of /u to \"staff\".\n", progname);
    fprintf(stream, "  %s -hR staff /u  Change the group of /u and subfiles to \"staff\".\n", progname);
}

static char* bx_chgrp_to_chown_group_spec(const char* group_text) {
    size_t len = strlen(group_text);
    char* spec = xmalloc(len + 2);
    spec[0] = ':';
    memcpy(spec + 1, group_text, len + 1);
    return spec;
}

static void bx_chgrp_parse_options(int argc, char** argv, struct bx_chgrp_parse_result* result) {
    static const struct option long_options[] = {
        {"changes", no_argument, NULL, 'c'},
        {"recursive", no_argument, NULL, 'R'},
        {"quiet", no_argument, NULL, 'f'},
        {"silent", no_argument, NULL, 'f'},
        {"no-dereference", no_argument, NULL, 'h'},
        {"dereference", no_argument, NULL, BX_CHGRP_OPT_DEREFERENCE},
        {"from", required_argument, NULL, BX_CHGRP_OPT_FROM},
        {"reference", required_argument, NULL, BX_CHGRP_OPT_REFERENCE},
        {"no-preserve-root", no_argument, NULL, BX_CHGRP_OPT_NO_PRESERVE_ROOT},
        {"preserve-root", no_argument, NULL, BX_CHGRP_OPT_PRESERVE_ROOT},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, BX_CHGRP_OPT_HELP},
        {"version", no_argument, NULL, BX_CHGRP_OPT_VERSION},
        {NULL, 0, NULL, 0},
    };

    memset(result, 0, sizeof(*result));
    result->ok = true;
    result->group_operand_index = -1;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "+:RcfhHLPv", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'R':
            case 'c':
            case 'f':
            case 'h':
            case 'H':
            case 'L':
            case 'P':
            case 'v':
            case BX_CHGRP_OPT_FROM:
            case BX_CHGRP_OPT_DEREFERENCE:
            case BX_CHGRP_OPT_NO_PRESERVE_ROOT:
            case BX_CHGRP_OPT_PRESERVE_ROOT:
                break;
            case BX_CHGRP_OPT_REFERENCE:
                result->reference_mode = true;
                break;
            case BX_CHGRP_OPT_HELP:
                result->show_help = true;
                return;
            case BX_CHGRP_OPT_VERSION:
                result->show_version = true;
                return;
            case ':':
            case '?':
            default:
                result->ok = false;
                return;
        }
    }

    if (!result->reference_mode && optind < argc) {
        result->group_operand_index = optind;
    }
}

int bx_chgrp_main(int argc, char** argv) {
    const char* progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "chgrp");

    struct bx_chgrp_parse_result parse_result;
    bx_chgrp_parse_options(argc, argv, &parse_result);

    if (parse_result.ok && parse_result.show_help) {
        bx_chgrp_print_help(stdout, progname);
        return 0;
    }
    if (parse_result.ok && parse_result.show_version) {
        bx_cli_print_version(progname);
        return 0;
    }

    char** chown_argv = xmalloc(((size_t)argc + 1) * sizeof(*chown_argv));
    for (int i = 0; i < argc; i++) {
        chown_argv[i] = argv[i];
    }
    chown_argv[argc] = NULL;

    char* converted_group = NULL;
    if (parse_result.ok && !parse_result.reference_mode && parse_result.group_operand_index > 0 && parse_result.group_operand_index < argc) {
        converted_group = bx_chgrp_to_chown_group_spec(argv[parse_result.group_operand_index]);
        chown_argv[parse_result.group_operand_index] = converted_group;
    }

    int rc = bx_chown_main(argc, chown_argv);

    free(converted_group);
    free(chown_argv);
    return rc;
}
