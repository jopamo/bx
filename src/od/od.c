#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

#include "applets.h"
#include "diag.h"
#include "libbx.h"

struct bx_od_options {
    const char* progname;
    char address_radix;
    size_t skip_bytes;
    size_t read_bytes;
    bool output_duplicates;
    int width;
    bool show_help;
    bool show_version;
    const char* format_str;
};

static void bx_od_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [FILE]...\n", progname);
    fprintf(stream, "Write an unambiguous representation, octal bytes by default, of FILE to\n");
    fprintf(stream, "standard output.  With more than one FILE argument, concatenate them\n");
    fprintf(stream, "in the listed order to form the input.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -A, --address-radix=RADIX   output format for file offsets; RADIX is one\n");
    fprintf(stream, "                                of d, o, x or n (none)\n");
    fprintf(stream, "  -j, --skip-bytes=BYTES      skip BYTES input bytes first\n");
    fprintf(stream, "  -N, --read-bytes=BYTES      limit dump to BYTES input bytes\n");
    fprintf(stream, "  -v, --output-duplicates     do not use * to mark line suppression\n");
    fprintf(stream, "  -w, --width[=BYTES]         output BYTES bytes per output line; 32 by default\n");
    fprintf(stream, "  -t, --format=TYPE           select output format or formats\n");
    fprintf(stream, "  -a                          same as -t a,  select named characters\n");
    fprintf(stream, "  -b                          same as -t o1, select octal bytes\n");
    fprintf(stream, "  -c                          same as -t c,  select ASCII characters or backslash escapes\n");
    fprintf(stream, "  -d                          same as -t u2, select unsigned decimal 2-byte units\n");
    fprintf(stream, "  -o                          same as -t o2, select octal 2-byte units\n");
    fprintf(stream, "  -x                          same as -t x2, select hexadecimal 2-byte units\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static void bx_od_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_od_parse_options(int argc, char** argv, struct bx_od_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"address-radix", required_argument, NULL, 'A'},
        {"skip-bytes", required_argument, NULL, 'j'},
        {"read-bytes", required_argument, NULL, 'N'},
        {"output-duplicates", no_argument, NULL, 'v'},
        {"width", optional_argument, NULL, 'w'},
        {"format", required_argument, NULL, 't'},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = "od";
    options->address_radix = 'o';
    options->width = 16;
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "A:j:N:vw::t:abcdox", long_options, &option_index);
        if (c == -1)
            break;

        switch (c) {
            case 'A':
                options->address_radix = optarg[0];
                break;
            case 'j':
                options->skip_bytes = strtoul(optarg, NULL, 10);
                break;
            case 'N':
                options->read_bytes = strtoul(optarg, NULL, 10);
                break;
            case 'v':
                options->output_duplicates = true;
                break;
            case 'w':
                options->width = optarg ? atoi(optarg) : 32;
                break;
            case 't':
                options->format_str = optarg;
                break;
            case 'a':
                options->format_str = "a";
                break;
            case 'b':
                options->format_str = "o1";
                break;
            case 'c':
                options->format_str = "c";
                break;
            case 'd':
                options->format_str = "u2";
                break;
            case 'o':
                options->format_str = "o2";
                break;
            case 'x':
                options->format_str = "x2";
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

    if (!options->format_str)
        options->format_str = "o2";

    *first_operand = optind;
    return true;
}

static void print_address(size_t addr, char radix) {
    if (radix == 'n')
        return;
    if (radix == 'x')
        printf("%07zx", addr);
    else if (radix == 'd')
        printf("%07zd", addr);
    else
        printf("%07zo", addr);
}

static void dump_line(unsigned char* buf, size_t n, size_t addr, struct bx_od_options* options) {
    print_address(addr, options->address_radix);

    // Very simplified format handling for now
    if (strcmp(options->format_str, "o1") == 0) {
        for (size_t i = 0; i < n; i++)
            printf(" %03o", buf[i]);
    }
    else if (strcmp(options->format_str, "x1") == 0) {
        for (size_t i = 0; i < n; i++)
            printf(" %02x", buf[i]);
    }
    else if (strcmp(options->format_str, "c") == 0) {
        for (size_t i = 0; i < n; i++) {
            if (isprint(buf[i]))
                printf("   %c", buf[i]);
            else if (buf[i] == '\n')
                printf("  \\n");
            else
                printf(" %03o", buf[i]);
        }
    }
    else {
        // Default o2
        for (size_t i = 0; i < n; i += 2) {
            if (i + 1 < n)
                printf(" %06o", (int)buf[i] | ((int)buf[i + 1] << 8));
            else
                printf(" %06o", (int)buf[i]);
        }
    }
    printf("\n");
}

int bx_od_main(int argc, char** argv) {
    struct bx_od_options options;
    struct bx_diag_ctx diag = {.progname = "od", .exit_status = 0};
    int first_operand = 0;

    if (!bx_od_parse_options(argc, argv, &options, &first_operand, &diag))
        return 1;
    if (options.show_help) {
        bx_od_print_help(stdout, options.progname);
        return 0;
    }
    if (options.show_version) {
        bx_od_print_version(options.progname);
        return 0;
    }

    int num_files = argc - first_operand;
    size_t total_addr = 0;
    unsigned char* buf = xmalloc(options.width);
    size_t n;

    for (int i = 0; i < num_files || (i == 0 && num_files == 0); i++) {
        const char* filename = (num_files == 0) ? "-" : argv[first_operand + i];
        FILE* f = strcmp(filename, "-") == 0 ? stdin : fopen(filename, "rb");
        if (!f) {
            bx_diag(&diag, "%s: %s", filename, strerror(errno));
            continue;
        }

        if (options.skip_bytes > 0) {
            fseek(f, options.skip_bytes, SEEK_SET);
            // Fallback for non-seekable
        }

        while (true) {
            size_t to_read = options.width;
            if (options.read_bytes > 0) {
                if (total_addr >= options.read_bytes)
                    break;
                if (to_read > options.read_bytes - total_addr)
                    to_read = options.read_bytes - total_addr;
            }
            n = fread(buf, 1, to_read, f);
            if (n == 0)
                break;
            dump_line(buf, n, total_addr, &options);
            total_addr += n;
        }

        if (f != stdin)
            fclose(f);
        if (options.read_bytes > 0 && total_addr >= options.read_bytes)
            break;
    }

    print_address(total_addr, options.address_radix);
    printf("\n");

    free(buf);
    return diag.exit_status;
}
