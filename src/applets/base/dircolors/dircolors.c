#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"

#ifndef BX_LSCOLORS_FILE
#define BX_LSCOLORS_FILE "/usr/share/bx/lscolors.dircolors"
#endif

enum bx_dircolors_shell_mode {
    BX_DIRCOLORS_SHELL_BOURNE = 0,
    BX_DIRCOLORS_SHELL_C,
};

struct bx_dircolors_options {
    const char* progname;
    enum bx_dircolors_shell_mode shell_mode;
    bool show_help;
    bool show_version;
    const char* input_path;
};

struct bx_dircolors_palette {
    char* data;
    size_t len;
    size_t cap;
    bool has_entries;
    bool has_reset;
};

struct bx_dircolors_keyword_map {
    const char* keyword;
    const char* key;
};

static const struct bx_dircolors_keyword_map bx_dircolors_keyword_map[] = {
    {"NORMAL", "no"},
    {"NORM", "no"},
    {"RESET", "rs"},
    {"FILE", "fi"},
    {"DIR", "di"},
    {"LNK", "ln"},
    {"LINK", "ln"},
    {"ORPHAN", "or"},
    {"MISSING", "mi"},
    {"FIFO", "pi"},
    {"SOCK", "so"},
    {"DOOR", "do"},
    {"BLK", "bd"},
    {"CHR", "cd"},
    {"EXEC", "ex"},
    {"LEFTCODE", "lc"},
    {"RIGHTCODE", "rc"},
    {"ENDCODE", "ec"},
    {"SUID", "su"},
    {"SETUID", "su"},
    {"SGID", "sg"},
    {"SETGID", "sg"},
    {"STICKY", "st"},
    {"OTHER_WRITABLE", "ow"},
    {"OWR", "ow"},
    {"STICKY_OTHER_WRITABLE", "tw"},
    {"OWT", "tw"},
    {"MULTIHARDLINK", "mh"},
    {"CAPABILITY", "ca"},
};

static enum bx_dircolors_shell_mode bx_dircolors_default_shell_mode(void) {
    const char* shell = getenv("SHELL");
    if (shell == NULL) {
        return BX_DIRCOLORS_SHELL_BOURNE;
    }

    if (strstr(shell, "csh") != NULL || strstr(shell, "tcsh") != NULL) {
        return BX_DIRCOLORS_SHELL_C;
    }

    return BX_DIRCOLORS_SHELL_BOURNE;
}

static void bx_dircolors_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [FILE]\n", progname);
    fprintf(stream, "Output shell commands to set LS_COLORS.\n");
    fprintf(stream, "FILE defaults to the first readable path among:\n");
    fprintf(stream, "  /etc/LS_COLORS, /etc/DIR_COLORS, %s\n", BX_LSCOLORS_FILE);
    fprintf(stream, "\n");
    fprintf(stream, "  -b, --bourne-shell   output Bourne shell commands\n");
    fprintf(stream, "  -c, --c-shell        output C shell commands\n");
    fprintf(stream, "      --help           display this help and exit\n");
    fprintf(stream, "      --version        output version information and exit\n");
}

static bool bx_dircolors_parse_options(int argc, char** argv, struct bx_dircolors_options* options, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"bourne-shell", no_argument, NULL, 'b'},
        {"c-shell", no_argument, NULL, 'c'},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "dircolors");
    options->shell_mode = bx_dircolors_default_shell_mode();
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int c = getopt_long(argc, argv, "+bc", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'b':
                options->shell_mode = BX_DIRCOLORS_SHELL_BOURNE;
                break;
            case 'c':
                options->shell_mode = BX_DIRCOLORS_SHELL_C;
                break;
            case 1:
                options->show_help = true;
                return true;
            case 2:
                options->show_version = true;
                return true;
            case '?':
                bx_cli_diag_unrecognized_option(diag, optopt, optind, argc, argv);
                return false;
            default:
                return false;
        }
    }

    if (optind < argc) {
        options->input_path = argv[optind];
        optind++;
    }

    if (optind < argc) {
        bx_diag(diag, "unexpected operand '%s'", argv[optind]);
        return false;
    }

    return true;
}

static void bx_dircolors_palette_free(struct bx_dircolors_palette* palette) {
    free(palette->data);
    memset(palette, 0, sizeof(*palette));
}

static bool bx_dircolors_palette_append(struct bx_dircolors_palette* palette, const char* text, size_t text_len, struct bx_diag_ctx* diag) {
    if (text_len == 0u) {
        return true;
    }

    size_t required = palette->len + text_len + 1u;
    if (required > palette->cap) {
        size_t new_cap = palette->cap == 0u ? 128u : palette->cap;
        while (new_cap < required) {
            if (new_cap > (SIZE_MAX / 2u)) {
                bx_diag(diag, "dircolors output too large");
                return false;
            }
            new_cap *= 2u;
        }
        palette->data = xrealloc(palette->data, new_cap);
        palette->cap = new_cap;
    }

    memcpy(palette->data + palette->len, text, text_len);
    palette->len += text_len;
    palette->data[palette->len] = '\0';
    return true;
}

static bool bx_dircolors_palette_add_entry(struct bx_dircolors_palette* palette, const char* key, const char* value, struct bx_diag_ctx* diag) {
    if (palette->has_entries) {
        if (!bx_dircolors_palette_append(palette, ":", 1u, diag)) {
            return false;
        }
    }

    if (!bx_dircolors_palette_append(palette, key, strlen(key), diag)) {
        return false;
    }
    if (!bx_dircolors_palette_append(palette, "=", 1u, diag)) {
        return false;
    }
    if (!bx_dircolors_palette_append(palette, value, strlen(value), diag)) {
        return false;
    }

    palette->has_entries = true;
    if (strcmp(key, "rs") == 0) {
        palette->has_reset = true;
    }

    return true;
}

static bool bx_dircolors_keyword_is_ignored(const char* key) {
    return strcasecmp(key, "TERM") == 0 ||
           strcasecmp(key, "COLOR") == 0 ||
           strcasecmp(key, "EIGHTBIT") == 0 ||
           strcasecmp(key, "OPTIONS") == 0 ||
           strcasecmp(key, "COLORTERM") == 0 ||
           strcasecmp(key, "*LS_COLORS") == 0;
}

static bool bx_dircolors_translate_key(const char* token, char* buffer, size_t buffer_size) {
    if (token[0] == '.') {
        int written = snprintf(buffer, buffer_size, "*%s", token);
        return written > 0 && (size_t)written < buffer_size;
    }

    if (token[0] == '*') {
        int written = snprintf(buffer, buffer_size, "%s", token);
        return written > 0 && (size_t)written < buffer_size;
    }

    for (size_t i = 0; i < sizeof(bx_dircolors_keyword_map) / sizeof(bx_dircolors_keyword_map[0]); i++) {
        if (strcasecmp(token, bx_dircolors_keyword_map[i].keyword) == 0) {
            int written = snprintf(buffer, buffer_size, "%s", bx_dircolors_keyword_map[i].key);
            return written > 0 && (size_t)written < buffer_size;
        }
    }

    size_t token_len = strlen(token);
    if (token_len == 2u && token_len + 1u <= buffer_size) {
        buffer[0] = (char)tolower((unsigned char)token[0]);
        buffer[1] = (char)tolower((unsigned char)token[1]);
        buffer[2] = '\0';
        return true;
    }

    return false;
}

static char* bx_dircolors_next_token(char** cursor) {
    char* text = *cursor;
    while (*text != '\0' && isspace((unsigned char)*text)) {
        text++;
    }

    if (*text == '\0') {
        *cursor = text;
        return NULL;
    }

    char* token = text;
    while (*text != '\0' && !isspace((unsigned char)*text)) {
        text++;
    }
    if (*text != '\0') {
        *text = '\0';
        text++;
    }

    *cursor = text;
    return token;
}

static bool bx_dircolors_parse_line(char* line, struct bx_dircolors_palette* palette, struct bx_diag_ctx* diag) {
    char* comment = strchr(line, '#');
    if (comment != NULL) {
        *comment = '\0';
    }

    char* cursor = line;
    char* key = bx_dircolors_next_token(&cursor);
    if (key == NULL) {
        return true;
    }

    char* value = bx_dircolors_next_token(&cursor);
    if (value == NULL) {
        return true;
    }

    if (bx_dircolors_keyword_is_ignored(key)) {
        return true;
    }

    char translated_key[256];
    if (!bx_dircolors_translate_key(key, translated_key, sizeof(translated_key))) {
        return true;
    }

    if (!bx_dircolors_palette_add_entry(palette, translated_key, value, diag)) {
        return false;
    }

    return true;
}

static bool bx_dircolors_parse_stream(FILE* stream, struct bx_dircolors_palette* palette, struct bx_diag_ctx* diag) {
    char* line = NULL;
    size_t line_cap = 0u;

    while (getline(&line, &line_cap, stream) >= 0) {
        if (!bx_dircolors_parse_line(line, palette, diag)) {
            free(line);
            return false;
        }
    }

    if (ferror(stream)) {
        bx_diag(diag, "failed to read dircolors input: %s", strerror(errno));
        free(line);
        return false;
    }

    free(line);

    if (!palette->has_reset) {
        if (!bx_dircolors_palette_add_entry(palette, "rs", "0", diag)) {
            return false;
        }
    }

    return true;
}

static bool bx_dircolors_emit_quoted(const char* text, struct bx_diag_ctx* diag) {
    if (fputc('\'', stdout) == EOF) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }

    for (const char* cursor = text; *cursor != '\0'; cursor++) {
        if (*cursor == '\'') {
            if (fputs("'\\''", stdout) == EOF) {
                bx_diag(diag, "write error: %s", strerror(errno));
                return false;
            }
            continue;
        }

        if (fputc(*cursor, stdout) == EOF) {
            bx_diag(diag, "write error: %s", strerror(errno));
            return false;
        }
    }

    if (fputc('\'', stdout) == EOF) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }

    return true;
}

static bool bx_dircolors_emit_output(const struct bx_dircolors_options* options, const struct bx_dircolors_palette* palette, struct bx_diag_ctx* diag) {
    const char* ls_colors = palette->data != NULL ? palette->data : "rs=0";

    if (options->shell_mode == BX_DIRCOLORS_SHELL_C) {
        if (fputs("setenv LS_COLORS ", stdout) == EOF) {
            bx_diag(diag, "write error: %s", strerror(errno));
            return false;
        }
        if (!bx_dircolors_emit_quoted(ls_colors, diag)) {
            return false;
        }
        if (fputc('\n', stdout) == EOF) {
            bx_diag(diag, "write error: %s", strerror(errno));
            return false;
        }
    }
    else {
        if (fputs("LS_COLORS=", stdout) == EOF) {
            bx_diag(diag, "write error: %s", strerror(errno));
            return false;
        }
        if (!bx_dircolors_emit_quoted(ls_colors, diag)) {
            return false;
        }
        if (fputs(";\nexport LS_COLORS\n", stdout) == EOF) {
            bx_diag(diag, "write error: %s", strerror(errno));
            return false;
        }
    }

    if (fflush(stdout) == EOF) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }

    return true;
}

static const char* bx_dircolors_default_input_path(void) {
    static const char* candidates[] = {
        "/etc/LS_COLORS",
        "/etc/DIR_COLORS",
        BX_LSCOLORS_FILE,
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (access(candidates[i], R_OK) == 0) {
            return candidates[i];
        }
    }

    return NULL;
}

int bx_dircolors_main(int argc, char** argv) {
    struct bx_dircolors_options options;
    struct bx_diag_ctx diag = {
        .progname = "dircolors",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (!bx_dircolors_parse_options(argc, argv, &options, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_dircolors_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    const char* input_path = options.input_path;
    if (input_path == NULL) {
        input_path = bx_dircolors_default_input_path();
        if (input_path == NULL) {
            bx_diag(&diag, "no readable dircolors database found");
            return diag.exit_status;
        }
    }

    FILE* stream = stdin;
    bool close_stream = false;
    if (strcmp(input_path, "-") != 0) {
        stream = fopen(input_path, "r");
        if (stream == NULL) {
            bx_perror_path(&diag, input_path);
            return diag.exit_status;
        }
        close_stream = true;
    }

    struct bx_dircolors_palette palette = {0};
    bool ok = bx_dircolors_parse_stream(stream, &palette, &diag);

    if (close_stream && fclose(stream) != 0) {
        bx_perror_path(&diag, input_path);
        ok = false;
    }

    if (ok) {
        ok = bx_dircolors_emit_output(&options, &palette, &diag);
    }

    bx_dircolors_palette_free(&palette);
    return ok ? 0 : (diag.exit_status != 0 ? diag.exit_status : 1);
}
