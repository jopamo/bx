#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dev_counters.h"
#include "literal_scan.h"
#include "pcre2_matcher.h"
#include "search_chunk_overlap.h"
#include "search_internal.h"
#include "search_raw_presence.h"

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
                                  const struct bx_lit_plan *absence_plan,
                                  struct search_opts *opts,
                                  int *match_count,
                                  struct bx_search_scanner *scanner,
                                  struct bx_search_stats *stats) {
    int status = 1;
    int file_matches = 0;
    size_t carry = 0u;
    bool counted_file = false;

    if (!f || !absence_plan || !opts || !scanner)
        return 2;
    (void)filename;
    size_t overlap = absence_plan->min_overlap_len;
    scanner->len = 0u;
    if (!bx_search_raw_presence_reserve_buffer(scanner,
                                               BX_SEARCH_RAW_PRESENCE_CHUNK_CAP + overlap)) {
        if (!use_stdin)
            fclose(f);
        return 2;
    }

    for (;;) {
        scanner->len = bx_search_chunk_overlap_prepend_carry(scanner->buf,
                                                             scanner->len,
                                                             carry);

        size_t nread = fread(scanner->buf + scanner->len, 1u, scanner->cap - scanner->len, f);
        scanner->len += nread;
        bx_search_dev_counters_note_content_read(nread);

        if (!counted_file) {
            counted_file = true;
            if (stats)
                stats->files_searched++;
        }
        if (stats)
            stats->bytes_searched += nread;

        if (bx_search_chunk_overlap_has_fresh_bytes(scanner->len, carry)) {
            struct bx_match literal_match;
            bool found = false;
            size_t scan_start = bx_search_chunk_overlap_scan_start(carry, overlap);
            size_t overlap_bytes_scanned =
                bx_search_chunk_overlap_rescanned_bytes(carry, overlap);

            bx_search_dev_counters_note_literal_overlap_bytes_scanned(overlap_bytes_scanned);
            size_t literal_match_off = SIZE_MAX;

            if (bx_literal_scan_absent(absence_plan,
                                       scanner->buf + scan_start,
                                       scanner->len - scan_start,
                                       &literal_match_off) == BX_LIT_FOUND) {
                literal_match.start = scan_start + literal_match_off;
                literal_match.end = literal_match.start + absence_plan->needle_len;
                found = true;
            }
            if (found) {
                bx_search_chunk_overlap_note_cross_chunk_match(literal_match.start,
                                                               literal_match.end,
                                                               carry);
                bx_search_dev_counters_note_literal_candidate_hit();
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

        carry = bx_search_chunk_overlap_carry_len(scanner->len, overlap);
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
