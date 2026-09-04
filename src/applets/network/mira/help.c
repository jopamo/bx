#include "mira.h"
#include "options.h"
#include <stdio.h>

static const char* mira_category_heading(MiraOptionCategory category) {
    switch (category) {
        case MIRA_OPTION_CATEGORY_STARTUP:
            return "Startup";
        case MIRA_OPTION_CATEGORY_DOWNLOAD:
            return "Download control";
        case MIRA_OPTION_CATEGORY_DIRECTORIES:
            return "Directories";
        case MIRA_OPTION_CATEGORY_HTTP:
            return "HTTP";
        case MIRA_OPTION_CATEGORY_TLS:
            return "HTTPS / TLS";
        case MIRA_OPTION_CATEGORY_RECURSIVE:
            return "Recursive retrieval";
        case MIRA_OPTION_CATEGORY_UNSUPPORTED:
            return "Explicitly unsupported until shared mechanics exist";
    }
    return NULL;
}

void bx_mira_print_help(void) {
    fputs(
        "Usage: mira [OPTION]... [URL]...\n"
        "Native bx fetch/crawler frontend over the shared fetch core.\n"
        "Only listed supported behavior is accepted; no configuration files "
        "are loaded.\n",
        stdout);

    size_t count = 0;
    const MiraOptionSpec* specs = bx_mira_option_specs(&count);
    for (MiraOptionCategory category = MIRA_OPTION_CATEGORY_STARTUP; category <= MIRA_OPTION_CATEGORY_UNSUPPORTED; category++) {
        fprintf(stdout, "\n%s:\n", mira_category_heading(category));
        for (size_t index = 0; index < count; index++) {
            const MiraOptionSpec* spec = &specs[index];
            if (spec->category != category)
                continue;

            char syntax[160];
            int written = 0;
            if (spec->alias)
                written = snprintf(syntax, sizeof(syntax), "%s, --%s%s%s", spec->alias, spec->name, spec->metavar ? "=" : "", spec->metavar ? spec->metavar : "");
            else if (spec->value > 0 && spec->value < 256)
                written = snprintf(syntax, sizeof(syntax), "-%c, --%s%s%s", spec->value, spec->name, spec->metavar ? "=" : "", spec->metavar ? spec->metavar : "");
            else
                written = snprintf(syntax, sizeof(syntax), "    --%s%s%s", spec->name, spec->metavar ? "=" : "", spec->metavar ? spec->metavar : "");

            if (written < 0 || (size_t)written >= sizeof(syntax))
                continue;
            fprintf(stdout, "  %-34s %s%s\n", syntax, spec->supported ? "" : "[unsupported] ", spec->description);
        }
    }

    fputs(
        "\nConfiguration compatibility flags such as --config, --no-config, "
        "and --execute are rejected.\n",
        stdout);
}
