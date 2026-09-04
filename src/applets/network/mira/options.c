#include "options.h"

static const struct option mira_long_options[] = {
#define MIRA_OPTION(name, has_arg, value, short_fragment, alias, metavar, description, category, supported) {name, has_arg, NULL, value},
#include "options.def"
#undef MIRA_OPTION
    {NULL, 0, NULL, 0},
};

static const char mira_short_options[] =
#define MIRA_OPTION(name, has_arg, value, short_fragment, alias, metavar, description, category, supported) short_fragment
#include "options.def"
#undef MIRA_OPTION
    "n:";

static const MiraOptionSpec mira_option_specs[] = {
#define MIRA_OPTION(name, has_arg, value, short_fragment, alias, metavar, description, category, supported) {name, value, has_arg, alias, metavar, description, category, supported},
#include "options.def"
#undef MIRA_OPTION
};

const struct option* bx_mira_long_options(void) {
    return mira_long_options;
}

const char* bx_mira_short_options(void) {
    return mira_short_options;
}

const MiraOptionSpec* bx_mira_option_specs(size_t* count_out) {
    if (count_out)
        *count_out = sizeof(mira_option_specs) / sizeof(mira_option_specs[0]);
    return mira_option_specs;
}

const MiraOptionSpec* bx_mira_option_spec_for_value(int value) {
    size_t count = 0;
    const MiraOptionSpec* specs = bx_mira_option_specs(&count);
    for (size_t index = 0; index < count; index++) {
        if (specs[index].value == value)
            return &specs[index];
    }
    return NULL;
}
