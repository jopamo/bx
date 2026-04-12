#ifndef BX_SEARCH_SEARCH_PLAN_H
#define BX_SEARCH_SEARCH_PLAN_H

#include <stdbool.h>
#include <stdio.h>

#include "search.h"

struct search_opts;

enum bx_search_plan_orchestrator {
    BX_SEARCH_PLAN_ORCHESTRATOR_SINGLE = 0,
    BX_SEARCH_PLAN_ORCHESTRATOR_PARALLEL_GENERIC,
    BX_SEARCH_PLAN_ORCHESTRATOR_PARALLEL_SUBTREE,
    BX_SEARCH_PLAN_ORCHESTRATOR_METADATA_SORTED,
};

enum bx_search_plan_input_kind {
    BX_SEARCH_PLAN_INPUT_NONE = 0,
    BX_SEARCH_PLAN_INPUT_RAW_STREAM,
    BX_SEARCH_PLAN_INPUT_RAW_BUFFER,
    BX_SEARCH_PLAN_INPUT_TRANSFORMED_BUFFER,
    BX_SEARCH_PLAN_INPUT_MULTILINE_BUFFER,
};

enum bx_search_plan_kernel_kind {
    BX_SEARCH_PLAN_KERNEL_NONE = 0,
    BX_SEARCH_PLAN_KERNEL_BUFFERED,
    BX_SEARCH_PLAN_KERNEL_MULTILINE,
    BX_SEARCH_PLAN_KERNEL_STREAMING,
    BX_SEARCH_PLAN_KERNEL_DEFERRED_FASTPATH,
};

enum bx_search_plan_output_kind {
    BX_SEARCH_PLAN_OUTPUT_FILES_ONLY = 0,
    BX_SEARCH_PLAN_OUTPUT_FILE_NAMES,
    BX_SEARCH_PLAN_OUTPUT_COUNTS,
    BX_SEARCH_PLAN_OUTPUT_MATCH_LINES,
};

enum bx_search_plan_publication_kind {
    BX_SEARCH_PLAN_PUBLICATION_DIRECT = 0,
    BX_SEARCH_PLAN_PUBLICATION_ORDERED,
    BX_SEARCH_PLAN_PUBLICATION_UNORDERED,
};

struct bx_search_plan {
    enum bx_search_plan_orchestrator orchestrator;
    enum bx_search_plan_input_kind input_kind;
    enum bx_search_plan_kernel_kind kernel_kind;
    enum bx_search_plan_output_kind output_kind;
    enum bx_search_plan_publication_kind publication_kind;
    bool rg_searches_stdin;
    bool has_metadata_sort;
    bool has_context;
    bool has_explicit_transform;
    bool has_multiline;
    bool parallel_supported;
    bool subtree_parallel_supported;
};

bool bx_search_plan_needs_line_buffering(const struct search_opts *opts);
bool bx_search_plan_plain_output_needs_binary_sensitive_path(const struct search_opts *opts);
void bx_search_plan_build(struct bx_search_plan *plan,
                          enum bx_search_personality personality,
                          const struct search_opts *opts,
                          int num_files,
                          bool rg_searches_stdin);
bool bx_search_plan_debug_enabled(void);
void bx_search_plan_debug_dump(FILE *stream, const struct bx_search_plan *plan);

#endif
