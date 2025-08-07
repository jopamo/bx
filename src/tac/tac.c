#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include "applets.h"
#include "bx/diag.h"

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

static void tac_file_regex(FILE* f, const char* pattern, bool before) {
    int errcode;
    PCRE2_SIZE erroffset;
    pcre2_code* re = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED, 0, &errcode, &erroffset, NULL);
    if (!re) {
        PCRE2_UCHAR errbuf[256];
        pcre2_get_error_message(errcode, errbuf, sizeof(errbuf));
        bx_err("%s: %s", pattern, (const char*)errbuf);
        return;
    }

    pcre2_match_data* md = pcre2_match_data_create_from_pattern(re, NULL);

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
        if (sep_lens[nrecords - 1] > 0)
            fwrite(seps[nrecords - 1], 1, sep_lens[nrecords - 1], stdout);
        for (size_t i = nrecords - 1; i > 0; i--) {
            if (sep_lens[i - 1] > 0)
                fwrite(seps[i - 1], 1, sep_lens[i - 1], stdout);
            if (body_lens[i] > 0)
                fwrite(bodies[i], 1, body_lens[i], stdout);
        }
        if (body_lens[0] > 0)
            fwrite(bodies[0], 1, body_lens[0], stdout);
    } else {
        for (size_t i = nrecords; i > 0; i--) {
            size_t idx = i - 1;
            if (body_lens[idx] > 0)
                fwrite(bodies[idx], 1, body_lens[idx], stdout);
            if (sep_lens[idx] > 0)
                fwrite(seps[idx], 1, sep_lens[idx], stdout);
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
    pcre2_match_data_free(md);
    pcre2_code_free(re);
}

static void tac_file(FILE* f, const char* separator, bool before) {
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
        fwrite(records[i - 1], 1, record_lens[i - 1], stdout);
        free(records[i - 1]);
    }
    free(records);
    free(record_lens);
}

int bx_tac_main(int argc, char** argv) {
    static const struct option long_options[] = {{"before", no_argument, NULL, 'b'}, {"regex", no_argument, NULL, 'r'},   {"separator", required_argument, NULL, 's'},
                                                  {"help", no_argument, NULL, 'h'},   {"version", no_argument, NULL, 'v'}, {NULL, 0, NULL, 0}};

    bool before = false;
    bool regex = false;
    const char* separator = "\n";
    int c;
    while ((c = getopt_long(argc, argv, "brs:", long_options, NULL)) != -1) {
        switch (c) {
            case 'b':
                before = true;
                break;
            case 'r':
                regex = true;
                break;
            case 's':
                separator = optarg;
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
                return 0;
            case 'v':
                printf("tac (bx) %s\n", BX_VERSION);
                return 0;
            default:
                return 1;
        }
    }

    if (regex) {
        if (optind == argc) {
            tac_file_regex(stdin, separator, before);
        }
        else {
            bool had_error = false;
            for (int i = optind; i < argc; i++) {
                FILE* f;
                if (strcmp(argv[i], "-") == 0) {
                    f = stdin;
                }
                else {
                    f = fopen(argv[i], "r");
                    if (!f) {
                        bx_perror(argv[i]);
                        had_error = true;
                        continue;
                    }
                }
                tac_file_regex(f, separator, before);
                if (f != stdin)
                    fclose(f);
            }
            if (had_error)
                return 1;
        }
    }
    else {
        if (optind == argc) {
            tac_file(stdin, separator, before);
        }
        else {
            bool had_error = false;
            for (int i = optind; i < argc; i++) {
                FILE* f;
                if (strcmp(argv[i], "-") == 0) {
                    f = stdin;
                }
                else {
                    f = fopen(argv[i], "r");
                    if (!f) {
                        bx_perror(argv[i]);
                        had_error = true;
                        continue;
                    }
                }
                tac_file(f, separator, before);
                if (f != stdin)
                    fclose(f);
            }
            if (had_error)
                return 1;
        }
    }

    return 0;
}
