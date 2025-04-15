#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "applets.h"
#include "diag.h"
#include "libbx.h"

struct bx_paste_options {
    const char* progname;
    int* delimiters;
    size_t delimiters_len;
    const char* delimiter_spec;
    bool serial;
    bool zero_terminated;
    bool show_help;
    bool show_version;
};

static void bx_paste_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [FILE]...\n", progname);
    fprintf(stream, "Write lines consisting of the sequentially corresponding lines from\n");
    fprintf(stream, "each FILE, separated by TABs, to standard output.\n");
    fprintf(stream, "\n");
    fprintf(stream, "With no FILE, or when FILE is -, read standard input.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -d, --delimiters=LIST   reuse characters from LIST instead of TABs;\n");
    fprintf(stream, "                            backslash escapes are supported\n");
    fprintf(stream, "  -s, --serial            paste one file at a time instead of in parallel\n");
    fprintf(stream, "  -z, --zero-terminated   line delimiter is NUL, not newline\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static void bx_paste_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_paste_parse_delimiters(const char* spec, struct bx_paste_options* options, struct bx_diag_ctx* diag) {
    size_t spec_len = strlen(spec);
    size_t capacity = spec_len == 0 ? 1 : spec_len;
    int* delimiters = xmalloc(capacity * sizeof(*delimiters));
    size_t count = 0;

    if (spec_len == 0) {
        delimiters[count++] = -1;
    }

    for (size_t i = 0; i < spec_len; i++) {
        unsigned char ch = (unsigned char)spec[i];
        if (ch != '\\') {
            delimiters[count++] = (int)ch;
            continue;
        }

        if (i + 1 >= spec_len) {
            free(delimiters);
            bx_diag(diag, "delimiter list ends with an unescaped backslash: %s", spec);
            return false;
        }

        i++;
        switch (spec[i]) {
            case '0':
                delimiters[count++] = -1;
                break;
            case 'b':
                delimiters[count++] = '\b';
                break;
            case 'f':
                delimiters[count++] = '\f';
                break;
            case 'n':
                delimiters[count++] = '\n';
                break;
            case 'r':
                delimiters[count++] = '\r';
                break;
            case 't':
                delimiters[count++] = '\t';
                break;
            case 'v':
                delimiters[count++] = '\v';
                break;
            case '\\':
                delimiters[count++] = '\\';
                break;
            default:
                delimiters[count++] = (unsigned char)spec[i];
                break;
        }
    }

    options->delimiters = delimiters;
    options->delimiters_len = count;
    return true;
}

static bool bx_paste_parse_options(int argc, char** argv, struct bx_paste_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"delimiters", required_argument, NULL, 'd'},
        {"serial", no_argument, NULL, 's'},
        {"zero-terminated", no_argument, NULL, 'z'},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = "paste";
    options->delimiter_spec = "\t";
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "d:sz", long_options, &option_index);
        if (c == -1)
            break;

        switch (c) {
            case 'd':
                options->delimiter_spec = optarg;
                break;
            case 's':
                options->serial = true;
                break;
            case 'z':
                options->zero_terminated = true;
                break;
            case 1:
                options->show_help = true;
                return true;
            case 2:
                options->show_version = true;
                return true;
            case '?':
                bx_diag(diag, "invalid option -- '%c'", optopt);
                return false;
            default:
                return false;
        }
    }

    *first_operand = optind;
    return bx_paste_parse_delimiters(options->delimiter_spec, options, diag);
}

static void bx_paste_emit_delimiter(const struct bx_paste_options* options, size_t index) {
    int delim = options->delimiters[index % options->delimiters_len];
    if (delim >= 0) {
        putchar(delim);
    }
}

static void paste_serial(int num_files, char** filenames, struct bx_paste_options* options, struct bx_diag_ctx* diag) {
    int delimiter = options->zero_terminated ? '\0' : '\n';

    for (int i = 0; i < num_files; i++) {
        FILE* f = strcmp(filenames[i], "-") == 0 ? stdin : fopen(filenames[i], "r");
        if (!f) {
            bx_diag(diag, "%s: %s", filenames[i], strerror(errno));
            continue;
        }

        char* line = NULL;
        size_t cap = 0;
        ssize_t len;
        bool first_line = true;
        size_t delim_idx = 0;
        while ((len = getdelim(&line, &cap, delimiter, f)) != -1) {
            if (!first_line) {
                bx_paste_emit_delimiter(options, delim_idx);
                delim_idx++;
            }
            if (line[len - 1] == delimiter)
                line[len - 1] = '\0';
            fputs(line, stdout);
            first_line = false;
        }
        putchar(delimiter);
        free(line);
        if (f != stdin)
            fclose(f);
    }
}

static void paste_parallel(int num_files, char** filenames, struct bx_paste_options* options, struct bx_diag_ctx* diag) {
    FILE** files = xmalloc(num_files * sizeof(FILE*));
    for (int i = 0; i < num_files; i++) {
        files[i] = strcmp(filenames[i], "-") == 0 ? stdin : fopen(filenames[i], "r");
        if (!files[i]) {
            bx_diag(diag, "%s: %s", filenames[i], strerror(errno));
        }
    }

    int delimiter = options->zero_terminated ? '\0' : '\n';

    char* line = NULL;
    size_t line_cap = 0;
    char** row_fields = xmalloc(num_files * sizeof(char*));

    while (true) {
        bool any_active = false;
        for (int i = 0; i < num_files; i++) {
            row_fields[i] = NULL;
            if (files[i]) {
                ssize_t len = getdelim(&line, &line_cap, delimiter, files[i]);
                if (len != -1) {
                    any_active = true;
                    if (line[len - 1] == delimiter)
                        line[len - 1] = '\0';
                    row_fields[i] = xstrdup(line);
                }
                else {
                    if (files[i] != stdin)
                        fclose(files[i]);
                    files[i] = NULL;
                }
            }
        }

        if (!any_active)
            break;

        for (int i = 0; i < num_files; i++) {
            if (row_fields[i])
                fputs(row_fields[i], stdout);
            if (i + 1 < num_files) {
                bx_paste_emit_delimiter(options, (size_t)i);
            }
            free(row_fields[i]);
        }
        putchar(delimiter);
    }

    for (int i = 0; i < num_files; i++)
        if (files[i] && files[i] != stdin)
            fclose(files[i]);
    free(files);
    free(line);
    free(row_fields);
}

int bx_paste_main(int argc, char** argv) {
    struct bx_paste_options options;
    struct bx_diag_ctx diag = {.progname = "paste", .exit_status = 0};
    int first_operand = 0;

    if (!bx_paste_parse_options(argc, argv, &options, &first_operand, &diag))
        return 1;
    if (options.show_help) {
        bx_paste_print_help(stdout, options.progname);
        return 0;
    }
    if (options.show_version) {
        bx_paste_print_version(options.progname);
        return 0;
    }

    int num_files = argc - first_operand;
    char* default_filenames[] = {"-"};
    char** filenames = (num_files == 0) ? default_filenames : &argv[first_operand];
    int real_num_files = (num_files == 0) ? 1 : num_files;

    if (options.serial) {
        paste_serial(real_num_files, filenames, &options, &diag);
    }
    else {
        paste_parallel(real_num_files, filenames, &options, &diag);
    }

    free(options.delimiters);
    return diag.exit_status;
}
