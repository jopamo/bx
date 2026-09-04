#define _GNU_SOURCE
#include "applets.h"
#include "lib/fetch/config.h"
#include "lib/fetch/error.h"
#include "lib/fetch/exit_code.h"
#include "lib/size_parse.h"
#include "mira.h"
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BX_MIRA_VERSION "0.1.0"

enum {
    MIRA_OPT_DRY_RUN = 1000,
    MIRA_OPT_NO_DIRECTORIES,
    MIRA_OPT_NO_PROXY,
    MIRA_OPT_TRIES,
};

static const struct option mira_options[] = {
    {"version", no_argument, NULL, 'V'},
    {"help", no_argument, NULL, 'h'},
    {"quiet", no_argument, NULL, 'q'},
    {"output-document", required_argument, NULL, 'O'},
    {"directory-prefix", required_argument, NULL, 'P'},
    {"dry-run", no_argument, NULL, MIRA_OPT_DRY_RUN},
    {"no-directories", no_argument, NULL, MIRA_OPT_NO_DIRECTORIES},
    {"no-proxy", no_argument, NULL, MIRA_OPT_NO_PROXY},
    {"tries", required_argument, NULL, MIRA_OPT_TRIES},
    {NULL, 0, NULL, 0},
};

static void mira_emit_parse_error(const struct bx_fetch_config* config, const char* summary) {
    fprintf(stderr, "mira: %s\n", summary);
    if (!config || config->logging.structured_errors)
        bx_fetch_error_emit_simple(stderr, BX_FETCH_ERROR_CLASS_PARSE, summary, NULL, NULL, -1, -1);
}

static int mira_replace_string(char** destination, const char* value) {
    char* replacement = NULL;
    if (value[0]) {
        replacement = strdup(value);
        if (!replacement)
            return -1;
    }
    free(*destination);
    *destination = replacement;
    return 0;
}

static int mira_parse_tries(const char* text, int* tries_out) {
    uintmax_t value = 0;
    if (!bx_size_parse_uint(text, &value) || value > (uintmax_t)INT_MAX) {
        errno = EINVAL;
        return -1;
    }
    *tries_out = (int)value;
    return 0;
}

static int mira_add_url(struct bx_fetch_config* config, const char* url) {
    if (!config || !url || config->input.url_count != 0) {
        errno = E2BIG;
        return -1;
    }
    char** urls = calloc(1, sizeof(*urls));
    if (!urls)
        return -1;
    urls[0] = strdup(url);
    if (!urls[0]) {
        free(urls);
        return -1;
    }
    config->input.urls = urls;
    config->input.url_count = 1;
    return 0;
}

static struct bx_fetch_config* mira_parse_cli(int argc, char** argv) {
    struct bx_fetch_config* config = bx_fetch_config_new();
    if (!config)
        return NULL;

    opterr = 0;
    optind = 0;
    for (;;) {
        int option_index = 0;
        int option = getopt_long(argc, argv, "VhqO:P:", mira_options, &option_index);
        if (option == -1)
            break;
        config->startup.cli_options_provided = true;
        switch (option) {
            case 'V':
                config->startup.show_version = true;
                break;
            case 'h':
                config->startup.show_help = true;
                break;
            case 'q':
                config->logging.verbosity = BX_FETCH_VERBOSITY_QUIET;
                config->download.show_progress = false;
                break;
            case 'O':
                if (mira_replace_string(&config->download.output_document, optarg) != 0)
                    goto allocation_failure;
                break;
            case 'P':
                if (mira_replace_string(&config->dirs.directory_prefix, optarg) != 0)
                    goto allocation_failure;
                break;
            case MIRA_OPT_DRY_RUN:
                config->download.dry_run = true;
                break;
            case MIRA_OPT_NO_DIRECTORIES:
                config->dirs.no_directories = true;
                break;
            case MIRA_OPT_NO_PROXY:
                config->download.no_proxy = true;
                break;
            case MIRA_OPT_TRIES:
                if (mira_parse_tries(optarg, &config->download.tries) != 0) {
                    char summary[256];
                    snprintf(summary, sizeof(summary), "invalid value for --tries: %s", optarg);
                    mira_emit_parse_error(config, summary);
                    bx_fetch_config_free(config);
                    return NULL;
                }
                break;
            case '?':
            default: {
                const char* token = optind > 0 && optind <= argc ? argv[optind - 1] : NULL;
                char summary[512];
                snprintf(summary, sizeof(summary), "invalid option token%s%s", token ? ": " : "", token ? token : "");
                mira_emit_parse_error(config, summary);
                bx_fetch_config_free(config);
                errno = EINVAL;
                return NULL;
            }
        }
    }

    for (int index = optind; index < argc; index++) {
        if (mira_add_url(config, argv[index]) != 0) {
            mira_emit_parse_error(config, "initial native frontend accepts exactly one URL");
            bx_fetch_config_free(config);
            errno = E2BIG;
            return NULL;
        }
    }
    return config;

allocation_failure:
    mira_emit_parse_error(config, "out of memory while parsing command line");
    bx_fetch_config_free(config);
    errno = ENOMEM;
    return NULL;
}

static void mira_print_help(void) {
    fputs(
        "Usage: mira [OPTION]... URL\n"
        "Native bx network retriever backed by the shared fetch core.\n\n"
        "  -V, --version                 display version information\n"
        "  -h, --help                    display this help\n"
        "  -q, --quiet                   suppress normal status output\n"
        "  -O, --output-document=FILE    write the document to FILE\n"
        "  -P, --directory-prefix=DIR    save files below DIR\n"
        "      --no-directories          save using only the URL filename\n"
        "      --no-proxy                disable proxy use\n"
        "      --tries=NUMBER            bound transfer attempts\n"
        "      --dry-run                 plan without network or filesystem writes\n",
        stdout);
}

int bx_mira_main(int argc, char** argv) {
    struct bx_fetch_config* config = mira_parse_cli(argc, argv);
    if (!config)
        return BX_FETCH_EXIT_PARSE_OR_CONFIG;

    int result = BX_FETCH_EXIT_SUCCESS;
    if (config->startup.show_version) {
        printf("mira %s\n", BX_MIRA_VERSION);
    }
    else if (config->startup.show_help) {
        mira_print_help();
    }
    else if (config->input.url_count == 0) {
        mira_emit_parse_error(config, "no URLs specified");
        result = BX_FETCH_EXIT_PARSE_OR_CONFIG;
    }
    else {
        result = bx_mira_run_config(config);
    }

    bx_fetch_config_free(config);
    return result;
}
