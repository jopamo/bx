#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#include "applets.h"
#include "diag.h"

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
                printf("  -r, --regex              reject regex separators as unsupported\n");
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
        bx_err("--regex is unsupported");
        return 1;
    }

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

    return 0;
}
