#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "applets.h"
#include "libbx.h"

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

static const char* bx_chgrp_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "chgrp";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }

    return argv0;
}

static void bx_chgrp_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... GROUP FILE...\n", progname);
    fprintf(stream, "  or:  %s [OPTION]... --reference=RFILE FILE...\n", progname);
    fprintf(stream, "Change the group of each FILE to GROUP.\n");
    fprintf(stream, "With --reference, change the group of each FILE to that of RFILE.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -c, --changes\n");
    fprintf(stream, "         like verbose but report only when a change is made\n");
    fprintf(stream, "  -f, --silent, --quiet\n");
    fprintf(stream, "         suppress most error messages\n");
    fprintf(stream, "  -v, --verbose\n");
    fprintf(stream, "         output a diagnostic for every file processed\n");
    fprintf(stream, "      --dereference\n");
    fprintf(stream, "         affect the referent of each symbolic link (this is\n");
    fprintf(stream, "         the default), rather than the symbolic link itself\n");
    fprintf(stream, "  -h, --no-dereference\n");
    fprintf(stream, "         affect symbolic links instead of any referenced file;\n");
    fprintf(stream, "         useful only on systems that can change the ownership of a symlink\n");
    fprintf(stream, "      --from=CURRENT_OWNER:CURRENT_GROUP\n");
    fprintf(stream, "         change the ownership of each file only if its\n");
    fprintf(stream, "         current owner and/or group match those specified here.\n");
    fprintf(stream, "         Either may be omitted, in which case a match\n");
    fprintf(stream, "         is not required for the omitted attribute\n");
    fprintf(stream, "      --no-preserve-root\n");
    fprintf(stream, "         do not treat '/' specially (the default)\n");
    fprintf(stream, "      --preserve-root\n");
    fprintf(stream, "         fail to operate recursively on '/'\n");
    fprintf(stream, "      --reference=RFILE\n");
    fprintf(stream, "         use RFILE's ownership rather than specifying values.\n");
    fprintf(stream, "         RFILE is always dereferenced if a symbolic link.\n");
    fprintf(stream, "  -R, --recursive\n");
    fprintf(stream, "         operate on files and directories recursively\n");
    fprintf(stream, "\n");
    fprintf(stream, "The following options modify how a hierarchy is traversed when the -R\n");
    fprintf(stream, "option is also specified.  If more than one is specified, only the final\n");
    fprintf(stream, "one takes effect. -P is the default.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -H\n");
    fprintf(stream, "         if a command line argument is a symlink to a directory, traverse it\n");
    fprintf(stream, "  -L\n");
    fprintf(stream, "         traverse every symbolic link to a directory encountered\n");
    fprintf(stream, "  -P\n");
    fprintf(stream, "         do not traverse any symbolic links\n");
    fprintf(stream, "\n");
    fprintf(stream, "      --help\n");
    fprintf(stream, "         display this help and exit\n");
    fprintf(stream, "      --version\n");
    fprintf(stream, "         output version information and exit\n");
    fprintf(stream, "\n");
    fprintf(stream, "Examples:\n");
    fprintf(stream, "  %s staff /u      Change the group of /u to \"staff\".\n", progname);
    fprintf(stream, "  %s -hR staff /u  Change the group of /u and subfiles to \"staff\".\n", progname);
}

static void bx_chgrp_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
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
    const char* progname = bx_chgrp_progname((argc > 0) ? argv[0] : NULL);

    struct bx_chgrp_parse_result parse_result;
    bx_chgrp_parse_options(argc, argv, &parse_result);

    if (parse_result.ok && parse_result.show_help) {
        bx_chgrp_print_help(stdout, progname);
        return 0;
    }
    if (parse_result.ok && parse_result.show_version) {
        bx_chgrp_print_version(progname);
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
