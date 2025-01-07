#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "applets.h"
#include "libbx.h"

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
    fprintf(stream, "\n");
    fprintf(stream, "  -c, --changes    like verbose but report only when a change is made\n");
    fprintf(stream, "  -f, --silent, --quiet  suppress most error messages\n");
    fprintf(stream, "  -h, --no-dereference   affect symbolic links instead of referenced files\n");
    fprintf(stream, "  -H                     follow command-line symbolic links during recursion\n");
    fprintf(stream, "  -L                     follow all symbolic links during recursion\n");
    fprintf(stream, "  -P                     do not follow symbolic links during recursion (default)\n");
    fprintf(stream, "  -R, --recursive        operate on files and directories recursively\n");
    fprintf(stream, "      --from=CURRENT_GROUP\n");
    fprintf(stream, "                         change only when the current group matches\n");
    fprintf(stream, "      --reference=RFILE  use RFILE's group instead of GROUP value\n");
    fprintf(stream, "  -v, --verbose    output a diagnostic for every file processed\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static void bx_chgrp_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_chgrp_short_group_is_options(const char* arg) {
    for (const char* p = arg + 1; *p != '\0'; p++) {
        if (*p != 'R' && *p != 'c' && *p != 'f' && *p != 'h' && *p != 'H' && *p != 'L' && *p != 'P' && *p != 'v') {
            return false;
        }
    }
    return true;
}

static char* bx_chgrp_to_chown_group_spec(const char* group_text) {
    if (group_text == NULL) {
        return xstrdup(":");
    }
    if (group_text[0] == ':') {
        return xstrdup(group_text);
    }

    size_t len = strlen(group_text);
    char* spec = xmalloc(len + 2);
    spec[0] = ':';
    memcpy(spec + 1, group_text, len + 1);
    return spec;
}

static char* bx_chgrp_to_chown_from_option(const char* group_text) {
    static const char prefix[] = "--from=";
    char* spec = bx_chgrp_to_chown_group_spec(group_text);
    size_t prefix_len = sizeof(prefix) - 1;
    size_t spec_len = strlen(spec);

    char* option = xmalloc(prefix_len + spec_len + 1);
    memcpy(option, prefix, prefix_len);
    memcpy(option + prefix_len, spec, spec_len + 1);
    free(spec);
    return option;
}

static void bx_chgrp_add_allocation(char** allocations, size_t* allocation_count, char* value) {
    allocations[*allocation_count] = value;
    (*allocation_count)++;
}

int bx_chgrp_main(int argc, char** argv) {
    const char* progname = bx_chgrp_progname((argc > 0) ? argv[0] : NULL);

    char** chown_argv = xmalloc(((size_t)argc + 1) * sizeof(*chown_argv));
    char** allocations = xmalloc(((size_t)argc + 1) * sizeof(*allocations));
    size_t allocation_count = 0;

    for (int i = 0; i < argc; i++) {
        chown_argv[i] = argv[i];
    }
    chown_argv[argc] = NULL;

    bool reference_mode = false;
    bool parse_failed = false;
    int operand_index = 1;

    while (operand_index < argc) {
        const char* arg = argv[operand_index];

        if (strcmp(arg, "--") == 0) {
            operand_index++;
            break;
        }
        if (strcmp(arg, "--help") == 0) {
            bx_chgrp_print_help(stdout, progname);
            free(allocations);
            free(chown_argv);
            return 0;
        }
        if (strcmp(arg, "--version") == 0) {
            bx_chgrp_print_version(progname);
            free(allocations);
            free(chown_argv);
            return 0;
        }
        if (strcmp(arg, "--reference") == 0) {
            reference_mode = true;
            if (operand_index + 1 >= argc) {
                parse_failed = true;
                break;
            }
            operand_index += 2;
            continue;
        }
        if (strncmp(arg, "--reference=", 12) == 0) {
            reference_mode = true;
            operand_index++;
            continue;
        }
        if (strcmp(arg, "--from") == 0) {
            if (operand_index + 1 >= argc) {
                parse_failed = true;
                break;
            }
            char* converted = bx_chgrp_to_chown_group_spec(argv[operand_index + 1]);
            bx_chgrp_add_allocation(allocations, &allocation_count, converted);
            chown_argv[operand_index + 1] = converted;
            operand_index += 2;
            continue;
        }
        if (strncmp(arg, "--from=", 7) == 0) {
            char* converted = bx_chgrp_to_chown_from_option(arg + 7);
            bx_chgrp_add_allocation(allocations, &allocation_count, converted);
            chown_argv[operand_index] = converted;
            operand_index++;
            continue;
        }
        if (strcmp(arg, "--changes") == 0 || strcmp(arg, "--recursive") == 0 || strcmp(arg, "--quiet") == 0 || strcmp(arg, "--silent") == 0 || strcmp(arg, "--no-dereference") == 0 ||
            strcmp(arg, "--dereference") == 0 || strcmp(arg, "--verbose") == 0) {
            operand_index++;
            continue;
        }
        if (arg[0] == '-' && arg[1] == '-') {
            parse_failed = true;
            break;
        }
        if (arg[0] == '-' && arg[1] != '\0') {
            if (!bx_chgrp_short_group_is_options(arg)) {
                parse_failed = true;
                break;
            }
            operand_index++;
            continue;
        }

        break;
    }

    if (!parse_failed && !reference_mode && operand_index < argc) {
        char* converted = bx_chgrp_to_chown_group_spec(argv[operand_index]);
        bx_chgrp_add_allocation(allocations, &allocation_count, converted);
        chown_argv[operand_index] = converted;
    }

    int rc = bx_chown_main(argc, chown_argv);

    for (size_t i = 0; i < allocation_count; i++) {
        free(allocations[i]);
    }
    free(allocations);
    free(chown_argv);

    return rc;
}
