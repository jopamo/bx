#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "crypto/digestsum.h"

enum {
    BX_DIGESTSUM_EXIT_OK = 0,
    BX_DIGESTSUM_EXIT_FAIL = 1,
};

enum bx_digestsum_longopt {
    BX_DIGESTSUM_OPT_IGNORE_MISSING = 256,
    BX_DIGESTSUM_OPT_QUIET,
    BX_DIGESTSUM_OPT_STATUS,
    BX_DIGESTSUM_OPT_STRICT,
    BX_DIGESTSUM_OPT_TAG,
    BX_DIGESTSUM_OPT_WARN,
    BX_DIGESTSUM_OPT_ZERO,
};

struct bx_digestsum_options {
    const char* progname;
    bool check_mode;
    bool binary_mode;
    bool binary_option_seen;
    bool text_option_seen;
    bool ignore_missing;
    bool quiet;
    bool status;
    bool strict;
    bool tag;
    bool warn;
    bool zero;
    bool show_help;
    bool show_version;
    int first_operand;
};

static const char* bx_digestsum_progname(const struct bx_digestsum_impl* impl, const char* argv0) {
    if (argv0 != NULL && argv0[0] != '\0') {
        return argv0;
    }
    return impl->default_progname;
}

static void bx_digestsum_print_try_help(const char* progname) {
    fprintf(stderr, "Try '%s --help' for more information.\n", progname);
}

static void bx_digestsum_print_help(FILE* stream, const struct bx_digestsum_impl* impl, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [FILE]...\n", progname);
    fprintf(stream, "Print or check %s checksums.\n", impl->algorithm_label);
    fprintf(stream, "Legacy interface to the cksum utility.\n");
    fprintf(stream, "\n");
    fprintf(stream, "With no FILE, or when FILE is -, read standard input.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -b, --binary   read in binary mode\n");
    fprintf(stream, "  -c, --check    read checksums from the FILEs and check them\n");
    fprintf(stream, "      --tag      create a BSD-style checksum\n");
    fprintf(stream, "  -t, --text     read in text mode (default)\n");
    fprintf(stream, "  -z, --zero     end each output line with NUL, not newline,\n");
    fprintf(stream, "                 and disable file name escaping\n");
    fprintf(stream, "\n");
    fprintf(stream, "The following five options are useful only when verifying checksums:\n");
    fprintf(stream, "      --ignore-missing  don't fail or report status for missing files\n");
    fprintf(stream, "      --quiet           don't print OK for each successfully verified file\n");
    fprintf(stream, "      --status          don't output anything, status code shows success\n");
    fprintf(stream, "      --strict          exit non-zero for improperly formatted checksum lines\n");
    fprintf(stream, "  -w, --warn            warn about improperly formatted checksum lines\n");
    fprintf(stream, "      --help            display this help and exit\n");
    fprintf(stream, "      --version         output version information and exit\n");
}

static void bx_digestsum_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static void bx_digestsum_invalid_usage(const char* progname, const char* message) {
    fprintf(stderr, "%s: %s\n", progname, message);
    bx_digestsum_print_try_help(progname);
}

static int bx_digestsum_hex_value(int ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static bool bx_digestsum_parse_hex_digest(const char* text, size_t digest_len, uint8_t* out) {
    size_t hex_len = digest_len * 2u;

    if (strlen(text) != hex_len) {
        return false;
    }

    for (size_t i = 0; i < digest_len; i++) {
        int hi = bx_digestsum_hex_value((unsigned char)text[i * 2u]);
        int lo = bx_digestsum_hex_value((unsigned char)text[i * 2u + 1u]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }

    return true;
}

static bool bx_digestsum_filename_needs_escape(const char* path) {
    for (const unsigned char* p = (const unsigned char*)path; *p != '\0'; p++) {
        if (*p == '\\' || *p == '\n' || *p == '\r') {
            return true;
        }
    }
    return false;
}

static void bx_digestsum_write_path(FILE* stream, const char* path) {
    (void)fwrite(path, 1, strlen(path), stream);
}

static void bx_digestsum_write_escaped_path(FILE* stream, const char* path) {
    for (const unsigned char* p = (const unsigned char*)path; *p != '\0'; p++) {
        if (*p == '\\') {
            fputs("\\\\", stream);
        }
        else if (*p == '\n') {
            fputs("\\n", stream);
        }
        else if (*p == '\r') {
            fputs("\\r", stream);
        }
        else {
            fputc(*p, stream);
        }
    }
}

static bool bx_digestsum_status_path_needs_quoting(const char* path) {
    for (const unsigned char* p = (const unsigned char*)path; *p != '\0'; p++) {
        if (*p == '\\' || *p == '\n' || *p == '\r' || *p == '\'') {
            return true;
        }
    }
    return false;
}

static void bx_digestsum_write_status_path(FILE* stream, const char* path) {
    if (!bx_digestsum_status_path_needs_quoting(path)) {
        bx_digestsum_write_path(stream, path);
        return;
    }

    bool in_single_quotes = false;
    for (const unsigned char* p = (const unsigned char*)path; *p != '\0'; p++) {
        if (*p == '\n' || *p == '\r' || *p == '\'') {
            if (in_single_quotes) {
                fputc('\'', stream);
                in_single_quotes = false;
            }

            if (*p == '\n') {
                fputs("$'\\n'", stream);
            }
            else if (*p == '\r') {
                fputs("$'\\r'", stream);
            }
            else {
                fputs("'\\''", stream);
            }
            continue;
        }

        if (!in_single_quotes) {
            fputc('\'', stream);
            in_single_quotes = true;
        }
        fputc(*p, stream);
    }

    if (in_single_quotes) {
        fputc('\'', stream);
    }
}

static bool bx_digestsum_unescape_path(char* text) {
    char* dst = text;
    const char* src = text;

    while (*src != '\0') {
        if (*src != '\\') {
            *dst++ = *src++;
            continue;
        }

        src++;
        if (*src == '\0') {
            return false;
        }

        if (*src == '\\') {
            *dst++ = '\\';
        }
        else if (*src == 'n') {
            *dst++ = '\n';
        }
        else if (*src == 'r') {
            *dst++ = '\r';
        }
        else {
            return false;
        }
        src++;
    }

    *dst = '\0';
    return true;
}

static char* bx_digestsum_find_last_tagged_separator(char* text) {
    char* match = NULL;
    char* scan = text;

    while (true) {
        char* found = strstr(scan, ") = ");
        if (found == NULL) {
            return match;
        }
        match = found;
        scan = found + 1;
    }
}

static bool bx_digestsum_parse_untagged_line(char* line, size_t digest_len, struct bx_checksum_record* record) {
    bool escaped_path = false;

    if (line[0] == '\\') {
        escaped_path = true;
        line++;
    }

    if (!bx_parse_check_line(line, digest_len, record)) {
        return false;
    }

    if (escaped_path && !bx_digestsum_unescape_path(record->filename)) {
        return false;
    }

    return true;
}

static bool bx_digestsum_parse_tagged_line(char* line, const struct bx_digestsum_impl* impl, struct bx_checksum_record* record) {
    bool escaped_path = false;
    char prefix[32];

    if (line[0] == '\\') {
        escaped_path = true;
        line++;
    }

    if (snprintf(prefix, sizeof(prefix), "%s (", impl->algorithm_label) < 0) {
        return false;
    }

    size_t prefix_len = strlen(prefix);
    if (strncmp(line, prefix, prefix_len) != 0) {
        return false;
    }

    char* path = line + prefix_len;
    char* sep = bx_digestsum_find_last_tagged_separator(path);
    if (sep == NULL) {
        return false;
    }

    *sep = '\0';
    const char* digest_text = sep + 4u;

    if (path[0] == '\0') {
        return false;
    }
    if (!bx_digestsum_parse_hex_digest(digest_text, impl->digest_size, record->digest)) {
        return false;
    }
    if (escaped_path && !bx_digestsum_unescape_path(path)) {
        return false;
    }

    record->digest_len = impl->digest_size;
    record->binary_mode = false;
    record->filename = path;
    return true;
}

static bool bx_digestsum_parse_check_record(char* line, const struct bx_digestsum_impl* impl, struct bx_checksum_record* record) {
    if (bx_digestsum_parse_untagged_line(line, impl->digest_size, record)) {
        return true;
    }
    return bx_digestsum_parse_tagged_line(line, impl, record);
}

static int bx_digestsum_hash_path(const struct bx_digestsum_impl* impl, const char* path, uint8_t* out) {
    void* ctx = malloc(impl->ctx_size);
    if (ctx == NULL) {
        errno = ENOMEM;
        return -1;
    }

    int rc = bx_digest_file(
        ctx,
        impl->ctx_size,
        impl->init_fn,
        impl->update_fn,
        impl->final_fn,
        path,
        out,
        impl->digest_size
    );
    free(ctx);
    return rc;
}

static const char* bx_digestsum_plural_suffix(size_t count) {
    return (count == 1u) ? "" : "s";
}

static int bx_digestsum_verify_stream(FILE* stream,
                                      const char* source_name,
                                      const struct bx_digestsum_impl* impl,
                                      const struct bx_digestsum_options* options) {
    char* line = NULL;
    size_t cap = 0u;
    size_t line_no = 0u;
    size_t parsed_count = 0u;
    size_t malformed_count = 0u;
    size_t success_count = 0u;
    size_t mismatch_count = 0u;
    size_t read_fail_count = 0u;
    bool failed = false;

    while (true) {
        ssize_t nread = getline(&line, &cap, stream);
        if (nread < 0) {
            break;
        }

        line_no++;
        if (nread > 0 && line[nread - 1] == '\n') {
            line[--nread] = '\0';
        }
        if (nread > 0 && line[nread - 1] == '\r') {
            line[--nread] = '\0';
        }

        struct bx_checksum_record expected;
        if (!bx_digestsum_parse_check_record(line, impl, &expected)) {
            malformed_count++;
            if (options->warn && !options->status) {
                fprintf(stderr,
                        "%s: %s: %zu: improperly formatted %s checksum line\n",
                        options->progname,
                        source_name,
                        line_no,
                        impl->algorithm_label);
            }
            continue;
        }
        parsed_count++;

        uint8_t actual[64];
        if (bx_digestsum_hash_path(impl, expected.filename, actual) != 0) {
            int saved_errno = errno;
            if (options->ignore_missing && saved_errno == ENOENT) {
                continue;
            }

            if (!options->status) {
                fprintf(stderr, "%s: %s: %s\n", options->progname, expected.filename, strerror(saved_errno));
                bx_digestsum_write_status_path(stdout, expected.filename);
                printf(": FAILED open or read\n");
            }
            read_fail_count++;
            failed = true;
            continue;
        }

        if (memcmp(actual, expected.digest, impl->digest_size) == 0) {
            success_count++;
            if (!options->quiet && !options->status) {
                bx_digestsum_write_status_path(stdout, expected.filename);
                printf(": OK\n");
            }
            continue;
        }

        if (!options->status) {
            bx_digestsum_write_status_path(stdout, expected.filename);
            printf(": FAILED\n");
        }
        mismatch_count++;
        failed = true;
    }

    if (ferror(stream)) {
        if (!options->status) {
            fprintf(stderr, "%s: %s: read error\n", options->progname, source_name);
        }
        failed = true;
    }

    if (parsed_count == 0u) {
        if (!options->status) {
            fprintf(stderr, "%s: %s: no properly formatted checksum lines found\n", options->progname, source_name);
        }
        failed = true;
    }

    if (malformed_count > 0u) {
        if (!options->status) {
            fprintf(stderr, "%s: WARNING: %zu line%s is improperly formatted\n", options->progname, malformed_count, bx_digestsum_plural_suffix(malformed_count));
        }
        if (options->strict) {
            failed = true;
        }
    }

    if (mismatch_count > 0u && !options->status) {
        fprintf(stderr, "%s: WARNING: %zu computed checksum%s did NOT match\n", options->progname, mismatch_count, bx_digestsum_plural_suffix(mismatch_count));
    }

    if (read_fail_count > 0u && !options->status) {
        fprintf(stderr, "%s: WARNING: %zu listed file%s could not be read\n", options->progname, read_fail_count, bx_digestsum_plural_suffix(read_fail_count));
    }

    if (options->ignore_missing && parsed_count > 0u && success_count == 0u) {
        if (!options->status) {
            fprintf(stderr, "%s: %s: no file was verified\n", options->progname, source_name);
        }
        failed = true;
    }

    free(line);
    return failed ? BX_DIGESTSUM_EXIT_FAIL : BX_DIGESTSUM_EXIT_OK;
}

static int bx_digestsum_check_file(const struct bx_digestsum_impl* impl,
                                   const struct bx_digestsum_options* options,
                                   const char* path) {
    if (strcmp(path, "-") == 0) {
        return bx_digestsum_verify_stream(stdin, "-", impl, options);
    }

    FILE* stream = fopen(path, "r");
    if (stream == NULL) {
        if (!options->status) {
            fprintf(stderr, "%s: %s: %s\n", options->progname, path, strerror(errno));
        }
        return BX_DIGESTSUM_EXIT_FAIL;
    }

    int rc = bx_digestsum_verify_stream(stream, path, impl, options);
    fclose(stream);
    return rc;
}

static int bx_digestsum_print_hash_result(const struct bx_digestsum_impl* impl,
                                          const struct bx_digestsum_options* options,
                                          const char* path) {
    uint8_t digest[64];
    char digest_hex[(64u * 2u) + 1u];
    bool escaped_path = !options->zero && bx_digestsum_filename_needs_escape(path);

    if (bx_digestsum_hash_path(impl, path, digest) != 0) {
        fprintf(stderr, "%s: %s: %s\n", options->progname, path, strerror(errno));
        return BX_DIGESTSUM_EXIT_FAIL;
    }

    bx_hex_encode_lower(digest, impl->digest_size, digest_hex);

    if (escaped_path) {
        putchar('\\');
    }

    if (options->tag) {
        printf("%s (", impl->algorithm_label);
        if (escaped_path) {
            bx_digestsum_write_escaped_path(stdout, path);
        }
        else {
            bx_digestsum_write_path(stdout, path);
        }
        printf(") = %s", digest_hex);
    }
    else {
        printf("%s %c", digest_hex, options->binary_mode ? '*' : ' ');
        if (escaped_path) {
            bx_digestsum_write_escaped_path(stdout, path);
        }
        else {
            bx_digestsum_write_path(stdout, path);
        }
    }

    putchar(options->zero ? '\0' : '\n');
    return BX_DIGESTSUM_EXIT_OK;
}

static bool bx_digestsum_parse_options(int argc,
                                       char** argv,
                                       const struct bx_digestsum_impl* impl,
                                       struct bx_digestsum_options* options) {
    static const struct option long_options[] = {
        {"binary", no_argument, NULL, 'b'},
        {"check", no_argument, NULL, 'c'},
        {"text", no_argument, NULL, 't'},
        {"warn", no_argument, NULL, 'w'},
        {"zero", no_argument, NULL, 'z'},
        {"ignore-missing", no_argument, NULL, BX_DIGESTSUM_OPT_IGNORE_MISSING},
        {"quiet", no_argument, NULL, BX_DIGESTSUM_OPT_QUIET},
        {"status", no_argument, NULL, BX_DIGESTSUM_OPT_STATUS},
        {"strict", no_argument, NULL, BX_DIGESTSUM_OPT_STRICT},
        {"tag", no_argument, NULL, BX_DIGESTSUM_OPT_TAG},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_digestsum_progname(impl, argv[0]);

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "+bctwz", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'b':
                options->binary_mode = true;
                options->binary_option_seen = true;
                break;
            case 'c':
                options->check_mode = true;
                break;
            case 't':
                options->binary_mode = false;
                options->text_option_seen = true;
                break;
            case 'w':
                options->warn = true;
                break;
            case 'z':
                options->zero = true;
                break;
            case BX_DIGESTSUM_OPT_IGNORE_MISSING:
                options->ignore_missing = true;
                break;
            case BX_DIGESTSUM_OPT_QUIET:
                options->quiet = true;
                break;
            case BX_DIGESTSUM_OPT_STATUS:
                options->status = true;
                break;
            case BX_DIGESTSUM_OPT_STRICT:
                options->strict = true;
                break;
            case BX_DIGESTSUM_OPT_TAG:
                options->tag = true;
                break;
            case 1:
                options->show_help = true;
                break;
            case 2:
                options->show_version = true;
                break;
            case '?': {
                const char* bad = NULL;
                if (optind > 0 && optind <= argc) {
                    bad = argv[optind - 1];
                }

                if (optopt != 0 && isprint((unsigned char)optopt)) {
                    fprintf(stderr, "%s: invalid option -- '%c'\n", options->progname, optopt);
                }
                else if (bad != NULL) {
                    fprintf(stderr, "%s: unrecognized option '%s'\n", options->progname, bad);
                }
                else {
                    fprintf(stderr, "%s: unrecognized option\n", options->progname);
                }
                bx_digestsum_print_try_help(options->progname);
                return false;
            }
            default:
                return false;
        }
    }

    options->first_operand = optind;
    return true;
}

static bool bx_digestsum_validate_options(const struct bx_digestsum_options* options) {
    if (options->check_mode) {
        if (options->binary_option_seen || options->text_option_seen) {
            bx_digestsum_invalid_usage(options->progname, "the --binary and --text options are meaningless when verifying checksums");
            return false;
        }
        if (options->tag) {
            bx_digestsum_invalid_usage(options->progname, "the --tag option is meaningless when verifying checksums");
            return false;
        }
        if (options->zero) {
            bx_digestsum_invalid_usage(options->progname, "the --zero option is not supported when verifying checksums");
            return false;
        }
        return true;
    }

    if (options->ignore_missing) {
        bx_digestsum_invalid_usage(options->progname, "the --ignore-missing option is meaningful only when verifying checksums");
        return false;
    }
    if (options->quiet) {
        bx_digestsum_invalid_usage(options->progname, "the --quiet option is meaningful only when verifying checksums");
        return false;
    }
    if (options->status) {
        bx_digestsum_invalid_usage(options->progname, "the --status option is meaningful only when verifying checksums");
        return false;
    }
    if (options->strict) {
        bx_digestsum_invalid_usage(options->progname, "the --strict option is meaningful only when verifying checksums");
        return false;
    }
    if (options->warn) {
        bx_digestsum_invalid_usage(options->progname, "the --warn option is meaningful only when verifying checksums");
        return false;
    }

    return true;
}

int bx_digestsum_main(int argc, char** argv, const struct bx_digestsum_impl* impl) {
    struct bx_digestsum_options options;
    if (!bx_digestsum_parse_options(argc, argv, impl, &options)) {
        return BX_DIGESTSUM_EXIT_FAIL;
    }

    if (options.show_help) {
        bx_digestsum_print_help(stdout, impl, options.progname);
        return BX_DIGESTSUM_EXIT_OK;
    }

    if (options.show_version) {
        bx_digestsum_print_version(options.progname);
        return BX_DIGESTSUM_EXIT_OK;
    }

    if (!bx_digestsum_validate_options(&options)) {
        return BX_DIGESTSUM_EXIT_FAIL;
    }

    if (options.check_mode) {
        if (options.first_operand >= argc) {
            return bx_digestsum_check_file(impl, &options, "-");
        }

        int rc = BX_DIGESTSUM_EXIT_OK;
        for (int i = options.first_operand; i < argc; i++) {
            if (bx_digestsum_check_file(impl, &options, argv[i]) != BX_DIGESTSUM_EXIT_OK) {
                rc = BX_DIGESTSUM_EXIT_FAIL;
            }
        }
        return rc;
    }

    if (options.first_operand >= argc) {
        return bx_digestsum_print_hash_result(impl, &options, "-");
    }

    int rc = BX_DIGESTSUM_EXIT_OK;
    for (int i = options.first_operand; i < argc; i++) {
        if (bx_digestsum_print_hash_result(impl, &options, argv[i]) != BX_DIGESTSUM_EXIT_OK) {
            rc = BX_DIGESTSUM_EXIT_FAIL;
        }
    }
    return rc;
}
