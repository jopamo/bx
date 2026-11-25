#include "options.h"

static const struct bx_option_spec mira_scan_options[] = {
#define MIRA_OPTION(name, arity, id, short_name, alias, metavar, description, category) {id, short_name, name, arity},
#include "options.def"
#undef MIRA_OPTION
    {'n', 'n', NULL, BX_OPTION_OPTIONAL_VALUE},
};

static const MiraOptionSpec mira_option_specs[] = {
#define MIRA_OPTION(name, arity, id, short_name, alias, metavar, description, category) {name, id, alias, metavar, description, category},
#include "options.def"
#undef MIRA_OPTION
};

void bx_mira_option_cursor_init(struct bx_option_cursor* cursor, char* const argv[], size_t first, size_t end) {
    bx_option_cursor_init(cursor, argv, first, end, BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE,
                          mira_scan_options, sizeof(mira_scan_options) / sizeof(mira_scan_options[0]));
}

const MiraOptionSpec* bx_mira_option_specs(size_t* count_out) {
    if (count_out)
        *count_out = sizeof(mira_option_specs) / sizeof(mira_option_specs[0]);
    return mira_option_specs;
}
