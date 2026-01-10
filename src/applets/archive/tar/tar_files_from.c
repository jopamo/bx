#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "applets/archive/tar/tar_files_from.h"
#include "bx/libbx.h"

bool bx_tar_files_from_read_buffer(const char* path,
                                   struct bx_archive_buffer* buffer,
                                   struct bx_diag_ctx* diag) {
    FILE* stream;

    bx_archive_buffer_init(buffer);
    if (strcmp(path, "-") == 0) {
        return bx_archive_buffer_read_all(stdin, buffer, diag);
    }

    stream = fopen(path, "rb");
    if (stream == NULL) {
        bx_diag(diag, "%s: %s", path, strerror(errno));
        return false;
    }

    if (!bx_archive_buffer_read_all(stream, buffer, diag)) {
        fclose(stream);
        bx_archive_buffer_free(buffer);
        return false;
    }
    if (fclose(stream) != 0) {
        bx_diag(diag, "%s: %s", path, strerror(errno));
        bx_archive_buffer_free(buffer);
        return false;
    }

    return true;
}

static bool bx_tar_files_from_append_char(struct bx_archive_buffer* buffer,
                                          unsigned char ch) {
    return bx_archive_buffer_append_byte(buffer, ch);
}

static int bx_tar_files_from_hex_value(int ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + ch - 'a';
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + ch - 'A';
    }
    return -1;
}

char* bx_tar_files_from_unquote_text(const char* text) {
    struct bx_archive_buffer out = {0};
    size_t i;

    bx_archive_buffer_init(&out);
    for (i = 0u; text[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)text[i];

        if (ch != '\\' || text[i + 1u] == '\0') {
            bx_tar_files_from_append_char(&out, ch);
            continue;
        }

        i++;
        ch = (unsigned char)text[i];
        switch (ch) {
            case '\\': bx_tar_files_from_append_char(&out, '\\'); break;
            case 'a': bx_tar_files_from_append_char(&out, '\a'); break;
            case 'b': bx_tar_files_from_append_char(&out, '\b'); break;
            case 'f': bx_tar_files_from_append_char(&out, '\f'); break;
            case 'n': bx_tar_files_from_append_char(&out, '\n'); break;
            case 'r': bx_tar_files_from_append_char(&out, '\r'); break;
            case 't': bx_tar_files_from_append_char(&out, '\t'); break;
            case 'v': bx_tar_files_from_append_char(&out, '\v'); break;
            case 'x': {
                size_t j = i + 1u;
                int value = 0;
                int digit;
                bool have_digit = false;

                while ((digit = bx_tar_files_from_hex_value((unsigned char)text[j])) >= 0) {
                    have_digit = true;
                    value = value * 16 + digit;
                    j++;
                }
                if (!have_digit) {
                    bx_tar_files_from_append_char(&out, '\\');
                    bx_tar_files_from_append_char(&out, 'x');
                    break;
                }
                bx_tar_files_from_append_char(&out, (unsigned char)value);
                i = j - 1u;
                break;
            }
            default:
                if (ch >= '0' && ch <= '7') {
                    size_t j;
                    int value = ch - '0';

                    for (j = 0u; j < 2u; j++) {
                        unsigned char next = (unsigned char)text[i + 1u];
                        if (next < '0' || next > '7') {
                            break;
                        }
                        i++;
                        value = value * 8 + (next - '0');
                    }
                    bx_tar_files_from_append_char(&out, (unsigned char)value);
                    break;
                }
                bx_tar_files_from_append_char(&out, '\\');
                bx_tar_files_from_append_char(&out, ch);
                break;
        }
    }

    bx_archive_buffer_append_byte(&out, '\0');
    return (char*)out.data;
}

char* bx_tar_files_from_decode_text(bool verbatim,
                                    bool unquote,
                                    const char* text) {
    if (verbatim || !unquote) {
        return xstrdup(text);
    }
    return bx_tar_files_from_unquote_text(text);
}

const char* bx_tar_files_from_skip_inline_space(const char* text) {
    while (*text == ' ' || *text == '\t') {
        text++;
    }
    return text;
}

void bx_tar_files_from_report_option_error(const struct bx_diag_ctx* diag,
                                           const char* list_path,
                                           size_t record_no,
                                           const char* message) {
    fprintf(stderr, "%s: %s:%zu: %s\n", diag->progname, list_path, record_no, message);
}
