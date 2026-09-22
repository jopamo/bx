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
        case MIRA_OPTION_CATEGORY_FTP:
            return "FTP";
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
        "       mira github COMMAND [ARGUMENT]...\n"
        "Native bx fetch/crawler frontend over the shared fetch core.\n"
        "Only listed supported behavior is accepted; no configuration files "
        "are loaded. Use `mira github --help` for native GitHub API access.\n",
        stdout);

    size_t count = 0;
    const MiraOptionSpec* specs = bx_mira_option_specs(&count);
    for (MiraOptionCategory category = MIRA_OPTION_CATEGORY_STARTUP; category <= MIRA_OPTION_CATEGORY_UNSUPPORTED; category++) {
        bool heading_printed = false;
        for (size_t index = 0; index < count; index++) {
            const MiraOptionSpec* spec = &specs[index];
            if (spec->category != category)
                continue;
            if (!heading_printed) {
                fprintf(stdout, "\n%s:\n", mira_category_heading(category));
                heading_printed = true;
            }

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
        "\n--http-password-file keeps HTTP passwords out of process arguments. "
        "It requires verified HTTPS for every URL and redirect and conflicts with "
        "--http-password, Bearer authentication, and --no-check-certificate. "
        "Use --http-user and --auth-no-challenge for preemptive Basic authentication. "
        "The file must be owned by the effective user, mode 0400 or 0600, regular, "
        "single-link, and have no symlinks in its path. Supply at most 4096 password "
        "bytes with optional LF or CRLF; spaces and empty passwords are preserved, "
        "ASCII controls and DEL are rejected. Redirects do not forward credentials "
        "to another origin. The caller owns file creation and removal.\n"
        "\nBearer tokens require verified HTTPS for every URL and redirect; "
        "--no-check-certificate is rejected. Tokens are sent preemptively, overriding HTTP, "
        "generic, and URL username/password credentials, even with "
        "--auth-no-challenge. Redirects do not forward them to another origin.\n"
        "Prefer --bearer-token-file to avoid exposing tokens in process arguments. "
        "It is mutually exclusive with --bearer-token. The file must be owned by "
        "the effective user, mode 0400 or 0600, regular, single-link, and have no "
        "symlinks in its path. Supply one token with optional LF or CRLF, no other whitespace.\n"
        "\nFTP supports direct downloads and spider checks. FTPS is not yet "
        "supported. Set --no-proxy when FTP proxy environment variables or "
        "proxy credentials are present; FTP through proxies is not supported.\n"
        "FTP credentials override generic and URL credentials as a pair. "
        "An omitted component is empty; empty option values clear that setting.\n"
        "FTP resume, timestamping, and saved HTTP headers are rejected.\n"
        "\n--markdown buffers one bounded HTML response, parses it with Lexbor, "
        "and writes Markdown instead of the response body. Without an explicit "
        "output document, the derived filename uses a .md extension. Hidden elements, "
        "navigation chrome, forms, and known Lore reply boilerplate are omitted. "
        "It conflicts with resume, saved headers, spider mode, and recursive retrieval. Use "
        "--output-document=- for stdout.\n"
        "\nConfiguration compatibility flags such as --config, --no-config, "
        "and --execute are rejected.\n",
        stdout);
}
