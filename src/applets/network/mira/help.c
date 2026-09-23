#include "mira.h"
#include "options.h"
#include <stdio.h>

void bx_mira_print_read_help(void) {
    fputs(
        "Usage: mira read [OPTION]... URL\n"
        "Read one verified HTTPS URL to stdout; no download file is created.\n"
        "HTML becomes Markdown; text, JSON, and XML pass through unchanged.\n"
        "Output is staged until the transfer and conversion succeed (64 MiB limit).\n"
        "Place read before options. No configuration files are loaded.\n"
        "\n"
        "  --raw                 pass the response body through without conversion\n"
        "  --absolute-links      resolve Markdown links/images using the final URL\n"
        "                        and the HTML base URL, when present\n"
        "  --expect=json         require a JSON response\n"
        "  -q, --quiet           suppress progress and session messages (default)\n"
        "  -v, --verbose         show transfer diagnostics\n"
        "  --json-diagnostics    emit only JSON Lines diagnostics, including events\n"
        "  --ca-certificate=FILE use a PEM CA bundle for TLS verification\n"
        "  --no-retry            make one attempt\n"
        "  --max-attempts=N      bound attempts, including the first request\n"
        "  --max-retry-time=N    bound recovery time in seconds (default: 30)\n"
        "  -h, --help            show this help\n"
        "\n"
        "HTTP, TLS-verification bypass, -O FILE, request bodies, saved headers,\n"
        "and forced --markdown conversion are not supported by read.\n"
        "Use mira --help for all transfer and authentication options.\n",
        stdout);
}

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
        "       mira read [OPTION]... URL\n"
        "       mira --spider URL\n"
        "       mira -O FILE URL\n"
        "       mira github COMMAND [ARGUMENT]...\n"
        "       mira github tree OWNER/REPO REF [--recursive]\n"
        "       mira gitlab raw PROJECT REF PATH\n"
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
        "\n--json-diagnostics emits lifecycle events and errors as JSON Lines only, "
        "overriding verbosity, progress, and response-header display options. "
        "It includes --debug events; ordinary --debug keeps human diagnostics. "
        "Only diagnostics change: response bodies and help still use stdout. "
        "The mode applies to downloads and read, including parse errors. "
        "--output-file and --append-output still redirect runtime diagnostics.\n"
        "\nRecovery: GET/HEAD requests without upload bodies retry transient transport "
        "failures and HTTP 408/425/429/500/502/503/504. Provider 403s retry only with "
        "explicit exhausted-rate-limit/reset evidence. Authentication and ordinary "
        "permission failures are terminal. --retry-on-http-error replaces the default status list. "
        "--no-retry makes one attempt; --max-attempts includes the first request. "
        "--max-retry-time defaults to 120 seconds across transport work and waits "
        "(30 seconds for read). Expired responses are not published. Retry-After "
        "is never shortened to fit the budget. --max-requests defaults to 64, "
        "shared across attempts, redirects, and internal requests.\n"
        "read requires verified HTTPS, stages stdout, converts HTML to Markdown, "
        "and preserves text/JSON/XML. Missing or unsupported MIME types and NUL "
        "bytes fail unless --raw is explicit. read takes one URL.\n"
        "Spider starts with HEAD; on 405/501 it tries a ranged GET and stops after "
        "successful headers, even if the server ignores Range. Combining --spider "
        "with --recursive or --page-requisites is rejected before any requests.\n"
        "Crawling does not fetch or apply robots.txt. Use explicit domain, directory, "
        "depth, and request limits to constrain retrieval. --no-parent confines links "
        "and redirects to the seed directories on their respective origins; use a "
        "trailing slash for a directory seed. Depth counts navigation links, not "
        "page-requisite dependencies. --no-clobber reuses existing documents for "
        "link discovery without replacing their payloads.\n"
        "Query variants use distinct @mira@query@HEX@ filenames, retaining the original "
        "extension. Converted links are URL-escaped and relative to each saved document.\n"
        "Stdout is staged privately up to 64 MiB and emitted only after a complete "
        "successful transfer and conversion. Larger documents must use -O FILE. "
        "Once stdout publication starts it cannot be rolled back or retried.\n",
        stdout);
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
        "--output-document=- for stdout. Add --absolute-links to resolve Markdown "
        "links and images against the final response URL and any HTML base URL; "
        "this also works with read.\n"
        "\nConfiguration compatibility flags such as --config, --no-config, "
        "and --execute are rejected.\n",
        stdout);
}
