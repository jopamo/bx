#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dev_counters.h"
#include "literal.h"
#include "search_internal.h"
#include "search_raw_presence.h"

#define BX_SEARCH_RAW_PRESENCE_CHUNK_CAP 65536u

static bool bx_search_raw_presence_reserve_buffer(struct bx_search_scanner *scanner,
                                                  size_t needed) {
    if (!scanner)
        return false;
    if (scanner->cap >= needed)
        return true;

    size_t new_cap = scanner->cap == 0u ? BX_SEARCH_RAW_PRESENCE_CHUNK_CAP : scanner->cap;
    while (new_cap < needed) {
        if (new_cap > (SIZE_MAX / 2u))
            return false;
        new_cap *= 2u;
    }

    unsigned char *tmp = realloc(scanner->buf, new_cap);
    if (!tmp)
        return false;
    scanner->buf = tmp;
    scanner->cap = new_cap;
    return true;
}

int bx_search_raw_presence_opened(FILE *f,
                                  bool use_stdin,
                                  const char *filename,
                                  const char *display_name,
                                  const char *progname,
                                  struct bx_matcher *m,
                                  struct search_opts *opts,
                                  int *match_count,
                                  struct bx_search_scanner *scanner,
                                  struct bx_search_stats *stats) {
    int status = 1;
    int file_matches = 0;
    size_t carry = 0u;
    bool counted_file = false;
    struct bx_literal_matcher *literal = bx_search_matcher_literal(m);

    if (!f || !m || !opts || !scanner || !literal)
        return 2;
    (void)filename;

    size_t plen = bx_literal_len(literal);
    size_t overlap = plen > 0u ? plen - 1u : 0u;
    scanner->len = 0u;
    if (!bx_search_raw_presence_reserve_buffer(scanner,
                                               BX_SEARCH_RAW_PRESENCE_CHUNK_CAP + overlap)) {
        if (!use_stdin)
            fclose(f);
        return 2;
    }

    for (;;) {
        if (carry > 0u)
            memmove(scanner->buf, scanner->buf + scanner->len - carry, carry);
        scanner->len = carry;

        size_t nread = fread(scanner->buf + carry, 1u, scanner->cap - carry, f);
        scanner->len += nread;
        bx_search_dev_counters_note_bytes_read(nread);

        if (!counted_file) {
            counted_file = true;
            if (stats)
                stats->files_searched++;
        }
        if (stats)
            stats->bytes_searched += nread;

        if (scanner->len > 0u) {
            struct bx_match literal_match;
            if (bx_literal_find(literal, scanner->buf, scanner->len, 0u, &literal_match) == 0) {
                bx_search_dev_counters_note_candidate_hit();
                file_matches = 1;
                if (stats) {
                    stats->matches++;
                    stats->matched_lines++;
                    stats->files_with_matches++;
                }
                status = 0;
                break;
            }
        }

        if (nread == 0u) {
            if (ferror(f)) {
                bx_search_report_path_error(progname, display_name, errno ? errno : EIO, opts);
                status = 2;
            }
            break;
        }

        carry = overlap < scanner->len ? overlap : scanner->len;
    }

    if (status != 2) {
        if (opts->files_with_matches && file_matches > 0 && display_name) {
            if (opts->null_output)
                bx_search_printf_out("%s%c", display_name, '\0');
            else
                bx_search_printf_out("%s\n", display_name);
            bx_search_dev_counters_note_output_line_emitted();
        }
        if (opts->files_without_match && file_matches == 0 && display_name) {
            if (opts->null_output)
                bx_search_printf_out("%s%c", display_name, '\0');
            else
                bx_search_printf_out("%s\n", display_name);
            bx_search_dev_counters_note_output_line_emitted();
        }
        if (match_count)
            *match_count += file_matches;
    }

    if (!use_stdin)
        fclose(f);
    return status;
}
