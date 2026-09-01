#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "options.h"
#include "rg_parallel.h"
#include "rg_sched.h"
#include "search_internal.h"
#include "search_plan.h"
#include "search_scanner.h"
#include "search_streaming.h"
#include "sort.h"

static bool bx_search_plan_has_explicit_transform(const struct search_opts *opts) {
    if (!opts)
        return false;
    return opts->pre_command != NULL
        || opts->search_zip
        || opts->encoding_mode == BX_RG_ENCODING_EXPLICIT;
}

static bool bx_search_plan_fastpath_is_deferred_candidate(
    enum bx_search_personality personality,
    const struct search_opts *opts,
    bool has_metadata_sort,
    bool rg_searches_stdin
);

static bool bx_search_plan_deferred_fastpath_has_absence_plan(
    const struct bx_matcher *matcher
);
static enum bx_search_max_filesize_zero_policy bx_search_select_max_filesize_zero_policy(
    const struct search_opts *opts,
    bool deferred_literal_precheck
);

bool bx_search_plan_needs_line_buffering(const struct search_opts *opts) {
    return opts && (opts->context_requested ||
                    opts->after_context > 0 || opts->before_context > 0);
}

bool bx_search_plan_plain_output_needs_binary_sensitive_path(const struct search_opts *opts) {
    return opts &&
           !opts->null_data &&
           !opts->binary_as_text &&
           (opts->binary_without_match ||
            (!opts->quiet && !opts->count_only &&
             !opts->files_with_matches && !opts->files_without_match));
}

static enum bx_search_plan_output_kind
bx_search_plan_select_output_kind(const struct search_opts *opts) {
    if (!opts)
        return BX_SEARCH_PLAN_OUTPUT_MATCH_LINES;
    if (opts->files_only)
        return BX_SEARCH_PLAN_OUTPUT_FILES_ONLY;
    if (opts->count_only)
        return BX_SEARCH_PLAN_OUTPUT_COUNTS;
    if (opts->files_with_matches || opts->files_without_match)
        return BX_SEARCH_PLAN_OUTPUT_FILE_NAMES;
    return BX_SEARCH_PLAN_OUTPUT_MATCH_LINES;
}

static enum bx_search_plan_input_kind
bx_search_plan_select_input_kind(enum bx_search_personality personality,
                                 const struct search_opts *opts,
                                 bool has_metadata_sort,
                                 bool rg_searches_stdin) {
    if (!opts || opts->files_only)
        return BX_SEARCH_PLAN_INPUT_NONE;
    if (opts->pre_command != NULL || opts->search_zip)
        return BX_SEARCH_PLAN_INPUT_TRANSFORMED_BUFFER;
    if (opts->encoding_mode == BX_RG_ENCODING_EXPLICIT) {
        return opts->multiline
            ? BX_SEARCH_PLAN_INPUT_TRANSFORMED_BUFFER
            : BX_SEARCH_PLAN_INPUT_DECODED_STREAM;
    }
    if (opts->multiline)
        return BX_SEARCH_PLAN_INPUT_MULTILINE_BUFFER;
    if (bx_search_plan_fastpath_is_deferred_candidate(personality, opts, has_metadata_sort,
                                                      rg_searches_stdin))
        return BX_SEARCH_PLAN_INPUT_RAW_STREAM;
    if (bx_search_plan_needs_line_buffering(opts)
        || bx_search_plan_plain_output_needs_binary_sensitive_path(opts)) {
        return BX_SEARCH_PLAN_INPUT_RAW_BUFFER;
    }
    return BX_SEARCH_PLAN_INPUT_RAW_STREAM;
}

static bool bx_search_plan_deferred_fastpath_requested(const struct search_opts *opts) {
    if (!opts)
        return false;
    return opts->quiet
        || opts->files_with_matches
        || opts->files_without_match
        || opts->recursive
        || (!opts->count_only && !opts->stats);
}

static bool bx_search_plan_fastpath_is_deferred_candidate(
    enum bx_search_personality personality,
    const struct search_opts *opts,
    bool has_metadata_sort,
    bool rg_searches_stdin
) {
    (void)personality;
    if (!opts)
        return false;
    if (rg_searches_stdin)
        return false;
    if (!bx_search_plan_deferred_fastpath_requested(opts))
        return false;
    if (has_metadata_sort)
        return false;
    /*
     * The deferred literal path can keep both default binary cutoff and
     * explicit binary diagnostics candidate-triggered: no-match returns from
     * raw fd evidence, while any candidate falls back through the opened path
     * before diagnostic publication. Explicit text, hidden-filename, and stdin
     * modes stay on existing paths that own their transformed/framed input.
     */
    if (opts->binary_as_text)
        return false;
    if (opts->hide_filename)
        return false;
    if (opts->multiline || opts->invert_match)
        return false;
    if (bx_search_plan_has_explicit_transform(opts))
        return false;
    if (bx_search_plan_needs_line_buffering(opts))
        return false;
    if (opts->replace || opts->only_matching || opts->passthru || opts->vimgrep)
        return false;
    /*
     * --json is rejected during option parsing before a search plan is built,
     * so no JSON output invocation can reach the deferred fast path.
     */
    if (opts->stop_on_nonmatch)
        return false;
    return true;
}

static bool bx_search_plan_deferred_fastpath_has_absence_plan(
    const struct bx_matcher *matcher
) {
    return bx_search_matcher_absence_plan(matcher) != NULL;
}

static enum bx_search_max_filesize_zero_policy bx_search_select_max_filesize_zero_policy(
    const struct search_opts *opts,
    bool deferred_literal_precheck
) {
    if (!opts || !opts->max_filesize_set || opts->max_filesize != 0u)
        return BX_SEARCH_MAX_FILESIZE_ZERO_DISABLED;

    /*
     * --max-filesize 0 is only size-insensitive when a non-empty literal
     * absence plan proves that an empty regular file cannot produce visible
     * output. Empty literals, empty regexes, and match-empty-capable regexes
     * must keep exact metadata because empty files remain observable.
     */
    return deferred_literal_precheck
        ? BX_SEARCH_MAX_FILESIZE_ZERO_SKIP_NON_EMPTY_LITERAL_REGULARS
        : BX_SEARCH_MAX_FILESIZE_ZERO_EXACT_EMPTY_SENSITIVE;
}

static enum bx_search_plan_kernel_kind
bx_search_plan_select_kernel_kind(enum bx_search_personality personality,
                                  const struct search_opts *opts,
                                  bool has_metadata_sort,
                                  bool rg_searches_stdin) {
    if (!opts || opts->files_only)
        return BX_SEARCH_PLAN_KERNEL_NONE;
    if (opts->multiline)
        return BX_SEARCH_PLAN_KERNEL_MULTILINE;
    if (bx_search_plan_fastpath_is_deferred_candidate(personality, opts, has_metadata_sort,
                                                      rg_searches_stdin))
        return BX_SEARCH_PLAN_KERNEL_DEFERRED_FASTPATH;
    if (bx_search_plan_needs_line_buffering(opts)
        || bx_search_plan_plain_output_needs_binary_sensitive_path(opts)) {
        return BX_SEARCH_PLAN_KERNEL_BUFFERED;
    }
    return BX_SEARCH_PLAN_KERNEL_STREAMING;
}

void bx_search_plan_build(struct bx_search_plan *plan,
                          enum bx_search_personality personality,
                          const struct search_opts *opts,
                          int num_files,
                          bool rg_searches_stdin) {
    bool metadata_sort;
    bool subtree_parallel_supported;
    bool parallel_supported;

    if (!plan)
        return;

    memset(plan, 0, sizeof(*plan));
    if (!opts) {
        plan->orchestrator = BX_SEARCH_PLAN_ORCHESTRATOR_SINGLE;
        plan->input_kind = BX_SEARCH_PLAN_INPUT_NONE;
        plan->kernel_kind = BX_SEARCH_PLAN_KERNEL_NONE;
        plan->output_kind = BX_SEARCH_PLAN_OUTPUT_MATCH_LINES;
        plan->publication_kind = BX_SEARCH_PLAN_PUBLICATION_DIRECT;
        plan->order_relevance = BX_SEARCH_PLAN_OUTPUT_ORDER_REQUIRED;
        return;
    }

    metadata_sort = bx_search_sort_is_metadata(opts) && !(num_files == 0 && rg_searches_stdin);
    subtree_parallel_supported = bx_rg_sched_supported(personality, opts, num_files,
                                                       rg_searches_stdin);
    parallel_supported = bx_search_parallel_rg_supported(personality, opts, num_files,
                                                         rg_searches_stdin);

    plan->rg_searches_stdin = rg_searches_stdin;
    plan->grep_family = personality != BX_SEARCH_RG;
    plan->has_metadata_sort = metadata_sort;
    plan->has_context = bx_search_plan_needs_line_buffering(opts);
    plan->has_explicit_transform = bx_search_plan_has_explicit_transform(opts);
    plan->has_multiline = opts->multiline;
    plan->parallel_supported = parallel_supported;
    plan->subtree_parallel_supported = subtree_parallel_supported;
    plan->input_kind = bx_search_plan_select_input_kind(personality, opts, metadata_sort,
                                                        rg_searches_stdin);
    plan->kernel_kind = bx_search_plan_select_kernel_kind(personality, opts, metadata_sort,
                                                          rg_searches_stdin);
    plan->output_kind = bx_search_plan_select_output_kind(opts);
    plan->order_relevance = BX_SEARCH_PLAN_OUTPUT_ORDER_REQUIRED;

    if (metadata_sort) {
        plan->orchestrator = BX_SEARCH_PLAN_ORCHESTRATOR_METADATA_SORTED;
        plan->publication_kind = BX_SEARCH_PLAN_PUBLICATION_DIRECT;
        return;
    }
    if (subtree_parallel_supported) {
        /*
         * Policy decision: recursive subtree scheduling publishes unsorted
         * --threads >1 results as worker-completion order. Sorted output,
         * heading output, and other order-sensitive modes stay out of this
         * scheduler, so output order is explicitly irrelevant for this plan.
         */
        plan->orchestrator = BX_SEARCH_PLAN_ORCHESTRATOR_PARALLEL_SUBTREE;
        plan->publication_kind = BX_SEARCH_PLAN_PUBLICATION_UNORDERED;
        plan->order_relevance = BX_SEARCH_PLAN_OUTPUT_ORDER_IRRELEVANT;
        return;
    }
    if (parallel_supported) {
        /*
         * Generic recursive parallel rg is selected only after sorted output is
         * rejected and rg's unsorted --threads >1 output policy makes worker
         * completion order explicitly order-irrelevant.
         */
        plan->orchestrator = BX_SEARCH_PLAN_ORCHESTRATOR_PARALLEL_GENERIC;
        plan->publication_kind = BX_SEARCH_PLAN_PUBLICATION_UNORDERED;
        plan->order_relevance = BX_SEARCH_PLAN_OUTPUT_ORDER_IRRELEVANT;
        return;
    }

    plan->orchestrator = BX_SEARCH_PLAN_ORCHESTRATOR_SINGLE;
    plan->publication_kind = BX_SEARCH_PLAN_PUBLICATION_DIRECT;
}

static enum bx_search_file_kernel_kind
bx_search_exec_plan_transformed_kernel(const struct bx_search_plan *plan) {
    if (!plan)
        return BX_SEARCH_FILE_KERNEL_STREAMING;
    if (plan->kernel_kind == BX_SEARCH_PLAN_KERNEL_MULTILINE)
        return BX_SEARCH_FILE_KERNEL_MULTILINE;
    if (plan->kernel_kind == BX_SEARCH_PLAN_KERNEL_BUFFERED)
        return BX_SEARCH_FILE_KERNEL_BUFFERED;
    return BX_SEARCH_FILE_KERNEL_STREAMING;
}

void bx_search_exec_plan_build(struct bx_search_exec_plan *exec_plan,
                               const struct bx_search_plan *plan,
                               const struct bx_matcher *matcher,
                               const struct search_opts *opts) {
    bool plain_binary_sensitive_path;
    bool line_buffered_stdin;
    bool scanner_regular_supported;
    bool needs_line_buffering;
    bool needs_rolling_record_output;

    if (!exec_plan)
        return;

    memset(exec_plan, 0, sizeof(*exec_plan));
    exec_plan->transformed_buffer_kernel = BX_SEARCH_FILE_KERNEL_STREAMING;
    exec_plan->opened_special_kernel = BX_SEARCH_FILE_KERNEL_STREAMING;
    exec_plan->opened_nonbinary_kernel = BX_SEARCH_FILE_KERNEL_STREAMING;
    exec_plan->regular_path_kernel = BX_SEARCH_FILE_KERNEL_STREAMING;
    exec_plan->stdin_path_kernel = BX_SEARCH_FILE_KERNEL_STREAMING;
    exec_plan->binary_search_kernel = BX_SEARCH_FILE_KERNEL_STREAMING;
    if (!plan || !opts)
        return;

    plain_binary_sensitive_path = bx_search_plan_plain_output_needs_binary_sensitive_path(opts);
    line_buffered_stdin = bx_search_streaming_uses_line_buffered_stdin(opts, true);
    scanner_regular_supported = bx_search_scanner_can_use(matcher, opts, false);
    needs_line_buffering = plan->has_context;
    needs_rolling_record_output =
        needs_line_buffering ||
        (plain_binary_sensitive_path &&
         plan->grep_family &&
         plan->output_kind == BX_SEARCH_PLAN_OUTPUT_MATCH_LINES);

    exec_plan->transformed_buffer_kernel =
        needs_rolling_record_output
            ? BX_SEARCH_FILE_KERNEL_BUFFERED
            : bx_search_exec_plan_transformed_kernel(plan);
    exec_plan->opened_special_kernel =
        (plan->kernel_kind == BX_SEARCH_PLAN_KERNEL_MULTILINE)
            ? BX_SEARCH_FILE_KERNEL_MULTILINE
            : (needs_rolling_record_output
                   ? BX_SEARCH_FILE_KERNEL_BUFFERED
                   : BX_SEARCH_FILE_KERNEL_STREAMING);
    exec_plan->opened_nonbinary_kernel =
        (plan->kernel_kind == BX_SEARCH_PLAN_KERNEL_MULTILINE)
            ? BX_SEARCH_FILE_KERNEL_MULTILINE
            : (needs_rolling_record_output
                   ? BX_SEARCH_FILE_KERNEL_BUFFERED
                   : (scanner_regular_supported
                          ? BX_SEARCH_FILE_KERNEL_SCANNER
                          : BX_SEARCH_FILE_KERNEL_STREAMING));
    exec_plan->regular_path_kernel = exec_plan->opened_nonbinary_kernel;
    exec_plan->stdin_path_kernel =
        (plan->kernel_kind == BX_SEARCH_PLAN_KERNEL_MULTILINE)
            ? BX_SEARCH_FILE_KERNEL_MULTILINE
            : ((needs_line_buffering || (plain_binary_sensitive_path && !line_buffered_stdin))
                   ? BX_SEARCH_FILE_KERNEL_BUFFERED
                   : BX_SEARCH_FILE_KERNEL_STREAMING);
    exec_plan->binary_search_kernel =
        needs_line_buffering ? BX_SEARCH_FILE_KERNEL_BUFFERED
                             : BX_SEARCH_FILE_KERNEL_STREAMING;
    exec_plan->raw_presence_supported =
        bx_search_scanner_can_raw_shortcut_file_presence(matcher, opts)
        && bx_search_matcher_absence_plan(matcher) != NULL;
    exec_plan->deferred_literal_precheck =
        plan->kernel_kind == BX_SEARCH_PLAN_KERNEL_DEFERRED_FASTPATH
        && plan->output_kind == BX_SEARCH_PLAN_OUTPUT_MATCH_LINES
        /*
         * The absence precheck is an input-side no-match proof. It must stay
         * available even when the eventual output kernel cannot use the
         * scanner (for example forced color), because a no-match file has no
         * output policy to honor and should not materialize records just to
         * discover absence.
         */
        && bx_search_plan_deferred_fastpath_has_absence_plan(matcher)
        && !opts->stats;
    exec_plan->max_filesize_zero_policy =
        bx_search_select_max_filesize_zero_policy(opts, exec_plan->deferred_literal_precheck);
}

bool bx_search_plan_debug_enabled(void) {
    const char *value = getenv("BX_RG_DEBUG_PLAN");

    return value && *value && strcmp(value, "0") != 0;
}

static const char *bx_search_plan_orchestrator_name(enum bx_search_plan_orchestrator value) {
    switch (value) {
    case BX_SEARCH_PLAN_ORCHESTRATOR_SINGLE:
        return "single";
    case BX_SEARCH_PLAN_ORCHESTRATOR_PARALLEL_GENERIC:
        return "parallel_generic";
    case BX_SEARCH_PLAN_ORCHESTRATOR_PARALLEL_SUBTREE:
        return "parallel_subtree";
    case BX_SEARCH_PLAN_ORCHESTRATOR_METADATA_SORTED:
        return "metadata_sorted";
    }
    return "unknown";
}

static const char *bx_search_plan_input_kind_name(enum bx_search_plan_input_kind value) {
    switch (value) {
    case BX_SEARCH_PLAN_INPUT_NONE:
        return "none";
    case BX_SEARCH_PLAN_INPUT_RAW_STREAM:
        return "raw_stream";
    case BX_SEARCH_PLAN_INPUT_RAW_BUFFER:
        return "raw_buffer";
    case BX_SEARCH_PLAN_INPUT_DECODED_STREAM:
        return "decoded_stream";
    case BX_SEARCH_PLAN_INPUT_TRANSFORMED_BUFFER:
        return "transformed_buffer";
    case BX_SEARCH_PLAN_INPUT_MULTILINE_BUFFER:
        return "multiline_buffer";
    }
    return "unknown";
}

static const char *bx_search_plan_kernel_kind_name(enum bx_search_plan_kernel_kind value) {
    switch (value) {
    case BX_SEARCH_PLAN_KERNEL_NONE:
        return "none";
    case BX_SEARCH_PLAN_KERNEL_BUFFERED:
        return "buffered";
    case BX_SEARCH_PLAN_KERNEL_MULTILINE:
        return "multiline";
    case BX_SEARCH_PLAN_KERNEL_STREAMING:
        return "streaming";
    case BX_SEARCH_PLAN_KERNEL_DEFERRED_FASTPATH:
        return "deferred_fastpath";
    }
    return "unknown";
}

static const char *bx_search_plan_output_kind_name(enum bx_search_plan_output_kind value) {
    switch (value) {
    case BX_SEARCH_PLAN_OUTPUT_FILES_ONLY:
        return "files_only";
    case BX_SEARCH_PLAN_OUTPUT_FILE_NAMES:
        return "file_names";
    case BX_SEARCH_PLAN_OUTPUT_COUNTS:
        return "counts";
    case BX_SEARCH_PLAN_OUTPUT_MATCH_LINES:
        return "match_lines";
    }
    return "unknown";
}

static const char *bx_search_plan_publication_kind_name(
    enum bx_search_plan_publication_kind value) {
    switch (value) {
    case BX_SEARCH_PLAN_PUBLICATION_DIRECT:
        return "direct";
    case BX_SEARCH_PLAN_PUBLICATION_ORDERED:
        return "ordered";
    case BX_SEARCH_PLAN_PUBLICATION_UNORDERED:
        return "unordered";
    }
    return "unknown";
}

static const char *bx_search_plan_order_relevance_name(
    enum bx_search_plan_order_relevance value) {
    switch (value) {
    case BX_SEARCH_PLAN_OUTPUT_ORDER_REQUIRED:
        return "required";
    case BX_SEARCH_PLAN_OUTPUT_ORDER_IRRELEVANT:
        return "irrelevant";
    }
    return "unknown";
}

void bx_search_plan_debug_dump(FILE *stream, const struct bx_search_plan *plan) {
    if (!stream || !plan)
        return;

    fprintf(stream,
            "bx-rg-plan: orchestrator=%s input=%s kernel=%s output=%s publication=%s order=%s"
            " rg_searches_stdin=%d metadata_sort=%d context=%d transform=%d multiline=%d"
            " parallel=%d subtree_parallel=%d\n",
            bx_search_plan_orchestrator_name(plan->orchestrator),
            bx_search_plan_input_kind_name(plan->input_kind),
            bx_search_plan_kernel_kind_name(plan->kernel_kind),
            bx_search_plan_output_kind_name(plan->output_kind),
            bx_search_plan_publication_kind_name(plan->publication_kind),
            bx_search_plan_order_relevance_name(plan->order_relevance),
            plan->rg_searches_stdin ? 1 : 0,
            plan->has_metadata_sort ? 1 : 0,
            plan->has_context ? 1 : 0,
            plan->has_explicit_transform ? 1 : 0,
            plan->has_multiline ? 1 : 0,
            plan->parallel_supported ? 1 : 0,
            plan->subtree_parallel_supported ? 1 : 0);
}
