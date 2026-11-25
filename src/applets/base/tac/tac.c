#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/fopen_dash.h"
#include "lib/line_writer.h"
#include "lib/option_scan.h"

static bool tac_write_error(struct bx_diag_ctx* diag) {
    bx_diag(diag, "write error: %s", strerror(errno));
    return false;
}

static bool tac_write(struct bx_line_writer* writer, const void* data, size_t len, struct bx_diag_ctx* diag) {
    if (!bx_line_writer_write(writer, data, len)) {
        return tac_write_error(diag);
    }
    return true;
}

static void tac_push_record(char*** records, size_t** record_lens, size_t* count, size_t* cap, const char* data, size_t len) {
    if (*count >= *cap) {
        *cap = *cap ? *cap * 2 : 1024;
        *records = realloc(*records, *cap * sizeof(char*));
        *record_lens = realloc(*record_lens, *cap * sizeof(size_t));
    }

    (*records)[*count] = malloc(len + 1);
    if (len > 0) {
        memcpy((*records)[*count], data, len);
    }
    (*records)[*count][len] = '\0';
    (*record_lens)[*count] = len;
    (*count)++;
}

static bool tac_file_regex(FILE* f, const pcre2_code* re, pcre2_match_data* md, bool before, struct bx_line_writer* writer, struct bx_diag_ctx* diag) {
    char* data = NULL;
    size_t data_len = 0;
    size_t data_cap = 0;
    char buffer[4096];
    size_t nread;

    while ((nread = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        if (data_len + nread > data_cap) {
            size_t new_cap = data_cap ? data_cap : 4096;
            while (new_cap < data_len + nread)
                new_cap *= 2;
            data = realloc(data, new_cap);
            data_cap = new_cap;
        }
        memcpy(data + data_len, buffer, nread);
        data_len += nread;
    }

    size_t record_cap = 64;
    char** bodies = malloc(record_cap * sizeof(char*));
    size_t* body_lens = malloc(record_cap * sizeof(size_t));
    char** seps = malloc(record_cap * sizeof(char*));
    size_t* sep_lens = malloc(record_cap * sizeof(size_t));
    size_t nrecords = 0;
    bool ok = true;

    if (data_len == 0)
        goto cleanup;

    size_t offset = 0;
    size_t last_body_start = 0;

    while (offset <= data_len) {
        int rc = pcre2_match(re, (PCRE2_SPTR)data, data_len, offset, 0, md, NULL);
        if (rc < 0) {
            size_t body_len = data_len - last_body_start;
            if (body_len > 0 || nrecords > 0) {
                if (nrecords >= record_cap) {
                    record_cap *= 2;
                    bodies = realloc(bodies, record_cap * sizeof(char*));
                    body_lens = realloc(body_lens, record_cap * sizeof(size_t));
                    seps = realloc(seps, record_cap * sizeof(char*));
                    sep_lens = realloc(sep_lens, record_cap * sizeof(size_t));
                }
                bodies[nrecords] = malloc(body_len + 1);
                if (body_len > 0)
                    memcpy(bodies[nrecords], data + last_body_start, body_len);
                bodies[nrecords][body_len] = '\0';
                body_lens[nrecords] = body_len;
                seps[nrecords] = NULL;
                sep_lens[nrecords] = 0;
                nrecords++;
            }
            break;
        }

        PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md);
        size_t match_start = (size_t)ov[0];
        size_t match_end = (size_t)ov[1];
        size_t msep_len = match_end - match_start;

        if (msep_len == 0) {
            offset = match_end + 1;
            continue;
        }

        size_t body_len = match_start - last_body_start;
        if (nrecords >= record_cap) {
            record_cap *= 2;
            bodies = realloc(bodies, record_cap * sizeof(char*));
            body_lens = realloc(body_lens, record_cap * sizeof(size_t));
            seps = realloc(seps, record_cap * sizeof(char*));
            sep_lens = realloc(sep_lens, record_cap * sizeof(size_t));
        }
        bodies[nrecords] = malloc(body_len + 1);
        if (body_len > 0)
            memcpy(bodies[nrecords], data + last_body_start, body_len);
        bodies[nrecords][body_len] = '\0';
        body_lens[nrecords] = body_len;
        seps[nrecords] = malloc(msep_len + 1);
        memcpy(seps[nrecords], data + match_start, msep_len);
        seps[nrecords][msep_len] = '\0';
        sep_lens[nrecords] = msep_len;
        nrecords++;

        last_body_start = match_end;
        offset = match_end;
    }

    if (nrecords == 0)
        goto cleanup;

    if (before) {
        if (sep_lens[nrecords - 1] > 0 && !tac_write(writer, seps[nrecords - 1], sep_lens[nrecords - 1], diag)) {
            ok = false;
            goto cleanup;
        }
        for (size_t i = nrecords - 1; i > 0; i--) {
            if (sep_lens[i - 1] > 0 && !tac_write(writer, seps[i - 1], sep_lens[i - 1], diag)) {
                ok = false;
                goto cleanup;
            }
            if (body_lens[i] > 0 && !tac_write(writer, bodies[i], body_lens[i], diag)) {
                ok = false;
                goto cleanup;
            }
        }
        if (body_lens[0] > 0 && !tac_write(writer, bodies[0], body_lens[0], diag)) {
            ok = false;
            goto cleanup;
        }
    }
    else {
        for (size_t i = nrecords; i > 0; i--) {
            size_t idx = i - 1;
            if (body_lens[idx] > 0 && !tac_write(writer, bodies[idx], body_lens[idx], diag)) {
                ok = false;
                goto cleanup;
            }
            if (sep_lens[idx] > 0 && !tac_write(writer, seps[idx], sep_lens[idx], diag)) {
                ok = false;
                goto cleanup;
            }
        }
    }

cleanup:
    for (size_t i = 0; i < nrecords; i++) {
        free(bodies[i]);
        free(seps[i]);
    }
    free(bodies);
    free(body_lens);
    free(seps);
    free(sep_lens);
    free(data);
    return ok;
}

static bool tac_file(FILE* f, const char* separator, bool before, struct bx_line_writer* writer, struct bx_diag_ctx* diag) {
    char* data = NULL;
    size_t data_len = 0;
    size_t data_cap = 0;
    char buffer[4096];
    size_t nread;

    char** records = NULL;
    size_t* record_lens = NULL;
    size_t nrecords = 0;
    size_t record_cap = 0;

    while ((nread = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        if (data_len + nread > data_cap) {
            size_t new_cap = data_cap ? data_cap : 4096;
            while (new_cap < data_len + nread) {
                new_cap *= 2;
            }
            data = realloc(data, new_cap);
            data_cap = new_cap;
        }

        memcpy(data + data_len, buffer, nread);
        data_len += nread;
    }

    if (separator == NULL) {
        separator = "\n";
    }
    size_t sep_len = strlen(separator);

    if (sep_len == 0) {
        if (data_len > 0) {
            tac_push_record(&records, &record_lens, &nrecords, &record_cap, data, data_len);
        }
    }
    else {
        size_t offset = 0;
        while (offset < data_len) {
            void* match = memmem(data + offset, data_len - offset, separator, sep_len);
            if (match != NULL) {
                size_t match_off = (size_t)((char*)match - (data + offset));
                size_t record_len = match_off + sep_len;
                tac_push_record(&records, &record_lens, &nrecords, &record_cap, data + offset, record_len);
                offset += record_len;
            }
            else {
                tac_push_record(&records, &record_lens, &nrecords, &record_cap, data + offset, data_len - offset);
                break;
            }
        }
    }

    free(data);

    if (before && sep_len > 0) {
        bool last_had_sep = false;

        for (size_t i = 0; i < nrecords; i++) {
            size_t record_len = record_lens[i];
            bool has_sep = (record_len >= sep_len && memcmp(records[i] + record_len - sep_len, separator, sep_len) == 0);
            size_t body_len = has_sep ? record_len - sep_len : record_len;
            size_t prefix_len = (i == 0) ? 0 : sep_len;
            char* record = malloc(prefix_len + body_len + 1);
            size_t out_len = 0;

            if (i > 0) {
                memcpy(record + out_len, separator, sep_len);
                out_len += sep_len;
            }

            if (body_len > 0) {
                memcpy(record + out_len, records[i], body_len);
                out_len += body_len;
            }

            record[out_len] = '\0';

            free(records[i]);
            records[i] = record;
            record_lens[i] = out_len;
            last_had_sep = has_sep;
        }

        if (last_had_sep) {
            tac_push_record(&records, &record_lens, &nrecords, &record_cap, separator, sep_len);
        }
    }

    for (size_t i = nrecords; i > 0; i--) {
        bool ok = tac_write(writer, records[i - 1], record_lens[i - 1], diag);
        free(records[i - 1]);
        if (!ok) {
            for (size_t j = i - 1; j > 0; j--) {
                free(records[j - 1]);
            }
            free(records);
            free(record_lens);
            return false;
        }
    }
    free(records);
    free(record_lens);
    return true;
}

int bx_tac_main(int argc, char** argv) {
    static const struct bx_option_spec specs[] = {
        {'b', 'b', "before", BX_OPTION_NO_VALUE}, {'r', 'r', "regex", BX_OPTION_NO_VALUE},   {'s', 's', "separator", BX_OPTION_REQUIRED_VALUE},
        {'h', 'h', "help", BX_OPTION_NO_VALUE},   {'v', 'v', "version", BX_OPTION_NO_VALUE},
    };
    struct bx_diag_ctx diag = {
        .progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "tac"),
    };
    bool before = false;
    bool regex = false;
    const char* separator = "\n";
    const char** files = NULL;
    size_t file_count = 0;
    int status = 1;
    pcre2_code* re = NULL;
    pcre2_match_data* md = NULL;
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, argc > 0 ? 1u : 0u, (size_t)argc, BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE, specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_OPERAND) {
            if (!files) {
                files = calloc((size_t)argc - option.index, sizeof(*files));
                if (!files) {
                    bx_perror_path(&diag, "allocate input operands");
                    goto done;
                }
            }
            files[file_count++] = option.token;
            continue;
        }
        if (option.event != BX_OPTION_MATCH) {
            if (option.event == BX_OPTION_MISSING_VALUE && option.short_name)
                bx_diag(&diag, "option requires an argument -- '%c'", option.short_name);
            else if (option.event == BX_OPTION_MISSING_VALUE)
                bx_diag(&diag, "option '%s' requires an argument", option.token);
            else if (option.short_name)
                bx_diag(&diag, "invalid option -- '%c'", option.short_name);
            else
                bx_diag(&diag, "unrecognized option '%s'", option.token);
            goto done;
        }
        switch (option.spec->id) {
            case 'b':
                before = true;
                break;
            case 'r':
                regex = true;
                break;
            case 's':
                separator = option.value;
                break;
            case 'h':
                printf("Usage: %s [OPTION]... [FILE]...\n", argv[0]);
                printf("Write each FILE to standard output, last line first.\n");
                printf("\n");
                printf("  -b, --before             attach the separator before instead of after\n");
                printf("  -r, --regex              interpret the separator as a regular expression\n");
                printf("  -s, --separator=STRING   use STRING as the separator instead of newline\n");
                printf("      --help          display this help and exit\n");
                printf("      --version       output version information and exit\n");
                status = 0;
                goto done;
            case 'v':
                printf("tac (bx) %s\n", BX_VERSION);
                status = 0;
                goto done;
        }
    }

    if (regex) {
        int code;
        PCRE2_SIZE offset;
        re = pcre2_compile((PCRE2_SPTR)separator, PCRE2_ZERO_TERMINATED, 0, &code, &offset, NULL);
        if (!re) {
            PCRE2_UCHAR detail[256];
            if (pcre2_get_error_message(code, detail, sizeof(detail)) < 0)
                bx_diag(&diag, "invalid separator regex (error %d)", code);
            else
                bx_diag(&diag, "invalid separator regex: %s", (const char*)detail);
            goto done;
        }
        md = pcre2_match_data_create_from_pattern(re, NULL);
        if (!md) {
            bx_diag(&diag, "cannot allocate separator match data");
            goto done;
        }
    }

    char output_buffer[8192];
    struct bx_line_writer writer;
    bx_line_writer_init(&writer, STDOUT_FILENO, output_buffer, sizeof(output_buffer));
    bool ok = true;
    bool had_error = false;
    for (size_t i = 0; ok && (i < file_count || (i == 0 && file_count == 0)); i++) {
        const char* path = file_count ? files[i] : "-";
        bool standard;
        FILE* f = bx_fopen_dash(path, "r", &standard);
        if (!f) {
            bx_perror_path(&diag, path);
            had_error = true;
            continue;
        }
        ok = re ? tac_file_regex(f, re, md, before, &writer, &diag) : tac_file(f, separator, before, &writer, &diag);
        bx_fclose_nonstdio(f, standard);
    }
    if (ok && bx_line_writer_error(&writer) == 0 && !bx_line_writer_flush(&writer))
        ok = tac_write_error(&diag);
    status = !ok || had_error;

done:
    pcre2_match_data_free(md);
    pcre2_code_free(re);
    free(files);
    return status;
}
