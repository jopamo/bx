#define _GNU_SOURCE
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "dev_counters.h"
#include "lib/color.h"
#include "lib/path_ops.h"
#include "rg_output.h"

static void bx_rg_color_style_clear(struct bx_rg_color_style *style) {
    if (!style)
        return;
    memset(style, 0, sizeof(*style));
}

static void bx_rg_set_basic_fg(struct bx_rg_color_style *style, int code) {
    style->fg_basic = (struct bx_rg_basic_color){.set = true, .code = code};
    style->fg_ansi256.set = false;
    style->fg_rgb.set = false;
}

static void bx_rg_set_basic_bg(struct bx_rg_color_style *style, int code) {
    style->bg_basic = (struct bx_rg_basic_color){.set = true, .code = code};
    style->bg_ansi256.set = false;
    style->bg_rgb.set = false;
}

void bx_rg_color_settings_init_defaults(struct bx_rg_color_settings *settings) {
    if (!settings)
        return;
    memset(settings, 0, sizeof(*settings));
    bx_rg_set_basic_fg(&settings->path, 35);
    bx_rg_set_basic_fg(&settings->line, 32);
    bx_rg_set_basic_fg(&settings->column, 32);
    bx_rg_set_basic_fg(&settings->match, 31);
    settings->match.bold = true;
}

static struct bx_rg_color_style *bx_rg_color_target(struct bx_rg_color_settings *settings,
                                                    const char *name) {
    if (!settings || !name)
        return NULL;
    if (strcmp(name, "path") == 0)
        return &settings->path;
    if (strcmp(name, "line") == 0)
        return &settings->line;
    if (strcmp(name, "column") == 0)
        return &settings->column;
    if (strcmp(name, "match") == 0 || strcmp(name, "highlight") == 0)
        return &settings->match;
    return NULL;
}

static bool bx_rg_component_digit(unsigned char ch, unsigned int base,
                                  unsigned int *digit_out) {
    unsigned int digit = 0;

    if (ch >= '0' && ch <= '9') {
        digit = (unsigned int)(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
        digit = 10u + (unsigned int)(ch - 'a');
    } else if (ch >= 'A' && ch <= 'F') {
        digit = 10u + (unsigned int)(ch - 'A');
    } else {
        return false;
    }

    if (digit >= base)
        return false;

    *digit_out = digit;
    return true;
}

static bool bx_rg_parse_uint_component(const char *text, unsigned int *out) {
    if (!text || text[0] == '\0' || !out)
        return false;

    const char *digits = text;
    while (isspace((unsigned char)*digits))
        digits++;

    if (digits[0] == '-' || digits[0] == '\0')
        return false;
    if (digits[0] == '+')
        digits++;
    if (digits[0] == '\0')
        return false;

    unsigned int base = 10;
    if (digits[0] == '0') {
        base = 8;
        if (digits[1] == 'x' || digits[1] == 'X') {
            base = 16;
            digits += 2;
            if (digits[0] == '\0')
                return false;
        }
    }

    unsigned int value = 0;
    for (; *digits != '\0'; digits++) {
        unsigned int digit = 0;
        if (!bx_rg_component_digit((unsigned char)*digits, base, &digit))
            return false;
        if (value > (255u - digit) / base)
            return false;
        value = (value * base) + digit;
    }

    *out = value;
    return true;
}

static bool bx_rg_parse_named_color(const char *value, int *out_code) {
    static const struct {
        const char *name;
        int code;
    } colors[] = {
        {"black", 30},
        {"red", 31},
        {"green", 32},
        {"yellow", 33},
        {"blue", 34},
        {"magenta", 35},
        {"cyan", 36},
        {"white", 37},
    };

    for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); i++) {
        if (strcmp(value, colors[i].name) == 0) {
            *out_code = colors[i].code;
            return true;
        }
    }
    return false;
}

static bool bx_rg_parse_color_value(const char *progname, const char *spec,
                                    const char *property, const char *value,
                                    struct bx_rg_color_style *style) {
    int code = 0;
    unsigned int component = 0;
    unsigned int r = 0, g = 0, b = 0;
    const bool is_fg = strcmp(property, "fg") == 0;

    if (strcmp(value, "none") == 0) {
        if (is_fg) {
            style->fg_basic.set = false;
            style->fg_ansi256.set = false;
            style->fg_rgb.set = false;
        } else {
            style->bg_basic.set = false;
            style->bg_ansi256.set = false;
            style->bg_rgb.set = false;
        }
        return true;
    }

    if (bx_rg_parse_named_color(value, &code)) {
        if (is_fg)
            bx_rg_set_basic_fg(style, code);
        else
            bx_rg_set_basic_bg(style, code + 10);
        return true;
    }

    if (strchr(value, ',') != NULL) {
        char *copy = strdup(value);
        char *save = NULL;
        char *first = NULL;
        char *second = NULL;
        char *third = NULL;
        bool ok = false;
        if (!copy)
            return false;
        first = strtok_r(copy, ",", &save);
        second = strtok_r(NULL, ",", &save);
        third = strtok_r(NULL, ",", &save);
        if (first && second && third && strtok_r(NULL, ",", &save) == NULL &&
            bx_rg_parse_uint_component(first, &r) &&
            bx_rg_parse_uint_component(second, &g) &&
            bx_rg_parse_uint_component(third, &b)) {
            if (is_fg) {
                style->fg_basic.set = false;
                style->fg_ansi256.set = false;
                style->fg_rgb = (struct bx_rg_rgb_color){.set = true, .red = r, .green = g, .blue = b};
            } else {
                style->bg_basic.set = false;
                style->bg_ansi256.set = false;
                style->bg_rgb = (struct bx_rg_rgb_color){.set = true, .red = r, .green = g, .blue = b};
            }
            ok = true;
        }
        free(copy);
        if (ok)
            return true;
    }

    if (bx_rg_parse_uint_component(value, &component)) {
        if (is_fg) {
            style->fg_basic.set = false;
            style->fg_rgb.set = false;
            style->fg_ansi256 = (struct bx_rg_ansi_color){.set = true, .index = component};
        } else {
            style->bg_basic.set = false;
            style->bg_rgb.set = false;
            style->bg_ansi256 = (struct bx_rg_ansi_color){.set = true, .index = component};
        }
        return true;
    }

    fprintf(stderr,
            "%s: error parsing flag --colors: unrecognized color name '%s'. Choose from: black, blue, green, red, cyan, magenta, yellow, white\n",
            progname, value);
    (void)spec;
    return false;
}

static bool bx_rg_parse_style_value(const char *progname, const char *value,
                                    struct bx_rg_color_style *style) {
    if (strcmp(value, "bold") == 0 || strcmp(value, "intense") == 0) {
        style->bold = true;
        style->none = false;
        return true;
    }
    if (strcmp(value, "nobold") == 0 || strcmp(value, "nointense") == 0) {
        style->bold = false;
        return true;
    }
    if (strcmp(value, "underline") == 0) {
        style->underline = true;
        style->none = false;
        return true;
    }
    if (strcmp(value, "nounderline") == 0) {
        style->underline = false;
        return true;
    }
    if (strcmp(value, "dim") == 0) {
        style->dim = true;
        style->none = false;
        return true;
    }
    if (strcmp(value, "nodim") == 0) {
        style->dim = false;
        return true;
    }

    fprintf(stderr,
            "%s: error parsing flag --colors: unrecognized style '%s'. Choose from: bold, nobold, intense, nointense, underline, nounderline, dim, nodim\n",
            progname, value);
    return false;
}

bool bx_rg_parse_colors_spec(const char *progname, const char *spec,
                             struct bx_rg_color_settings *settings) {
    char *copy = NULL;
    char *save = NULL;
    char *target_name = NULL;
    char *property = NULL;
    char *value = NULL;
    struct bx_rg_color_style *style = NULL;

    if (!spec) {
        fprintf(stderr,
                "%s: error parsing flag --colors: invalid color spec format: ''. Valid format is '(path|line|column|match|highlight):(fg|bg|style):(value)'.\n",
                progname);
        return false;
    }

    copy = strdup(spec);
    if (!copy)
        return false;

    target_name = strtok_r(copy, ":", &save);
    property = strtok_r(NULL, ":", &save);
    value = strtok_r(NULL, ":", &save);
    style = bx_rg_color_target(settings, target_name);

    if (!style || !property || strtok_r(NULL, ":", &save) != NULL) {
        fprintf(stderr,
                "%s: error parsing flag --colors: invalid color spec format: '%s'. Valid format is '(path|line|column|match|highlight):(fg|bg|style):(value)'.\n",
                progname, spec);
        free(copy);
        return false;
    }

    if (value == NULL) {
        if (strcmp(property, "none") != 0) {
            fprintf(stderr,
                    "%s: error parsing flag --colors: invalid color spec format: '%s'. Valid format is '(path|line|column|match|highlight):(fg|bg|style):(value)'.\n",
                    progname, spec);
            free(copy);
            return false;
        }
        bx_rg_color_style_clear(style);
        style->none = true;
        free(copy);
        return true;
    }

    style->none = false;
    if (strcmp(property, "fg") == 0 || strcmp(property, "bg") == 0) {
        bool ok = bx_rg_parse_color_value(progname, spec, property, value, style);
        free(copy);
        return ok;
    }
    if (strcmp(property, "style") == 0) {
        bool ok = bx_rg_parse_style_value(progname, value, style);
        free(copy);
        return ok;
    }

    fprintf(stderr,
            "%s: error parsing flag --colors: invalid color spec format: '%s'. Valid format is '(path|line|column|match|highlight):(fg|bg|style):(value)'.\n",
            progname, spec);
    free(copy);
    return false;
}

static void bx_rg_emit_single_color(FILE *stream,
                                    const struct bx_rg_basic_color *basic,
                                    const struct bx_rg_ansi_color *ansi256,
                                    const struct bx_rg_rgb_color *rgb,
                                    bool foreground) {
    if (basic && basic->set) {
        fprintf(stream, "\033[%dm", basic->code);
        return;
    }
    if (ansi256 && ansi256->set) {
        fprintf(stream, "\033[%d;5;%um", foreground ? 38 : 48, ansi256->index);
        return;
    }
    if (rgb && rgb->set) {
        fprintf(stream, "\033[%d;2;%u;%u;%um", foreground ? 38 : 48,
                rgb->red, rgb->green, rgb->blue);
    }
}

void bx_rg_emit_color_style_start_file(FILE *stream,
                                       const struct bx_rg_color_style *style) {
    if (!stream || !style || style->none || !bx_color_enabled())
        return;
    if (style->bold)
        fputs("\033[1m", stream);
    if (style->dim)
        fputs("\033[2m", stream);
    if (style->underline)
        fputs("\033[4m", stream);
    bx_rg_emit_single_color(stream, &style->fg_basic, &style->fg_ansi256, &style->fg_rgb, true);
    bx_rg_emit_single_color(stream, &style->bg_basic, &style->bg_ansi256, &style->bg_rgb, false);
}

void bx_rg_emit_color_reset_file(FILE *stream) {
    if (!stream || !bx_color_enabled())
        return;
    fputs("\033[0m", stream);
}

void bx_rg_emit_color_style_start(const struct bx_rg_color_style *style) {
    bx_rg_emit_color_style_start_file(stdout, style);
}

void bx_rg_emit_color_reset(void) {
    bx_rg_emit_color_reset_file(stdout);
}

static bool bx_rg_display_path_buf_reserve(struct bx_rg_display_path_buf *buf,
                                           size_t needed) {
    if (!buf)
        return false;
    if (buf->cap >= needed)
        return true;

    size_t new_cap = buf->cap == 0u ? 256u : buf->cap;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2u)
            return false;
        new_cap *= 2u;
    }
    char *tmp = realloc(buf->data, new_cap);
    if (!tmp)
        return false;
    buf->data = tmp;
    buf->cap = new_cap;
    return true;
}

const char *bx_rg_display_path_buf_format(struct bx_rg_display_path_buf *buf,
                                          const char *path,
                                          bool strip_dot_prefix,
                                          char path_separator) {
    const char *display = path;

    if (!path)
        return NULL;
    if (strip_dot_prefix)
        display = bx_path_strip_dot_slash_prefix_ptr(path);
    if (!display)
        display = path;
    if (path_separator == '\0' || path_separator == '/') {
        bx_search_dev_counters_note_display_path_borrow();
        return display;
    }

    size_t len = strlen(display);
    if (!bx_rg_display_path_buf_reserve(buf, len + 1u))
        return NULL;
    for (size_t i = 0; i < len; ++i) {
        char ch = display[i];
        buf->data[i] = ch == '/' ? path_separator : ch;
    }
    buf->data[len] = '\0';
    bx_search_dev_counters_note_display_path_copy(len);
    return buf->data;
}

void bx_rg_display_path_buf_dispose(struct bx_rg_display_path_buf *buf) {
    if (!buf)
        return;
    free(buf->data);
    memset(buf, 0, sizeof(*buf));
}

char *bx_rg_display_path_dup(const char *path, bool strip_dot_prefix,
                             char path_separator) {
    const char *display = path;
    char *copy = NULL;

    if (!path)
        return NULL;
    if (strip_dot_prefix)
        display = bx_path_strip_dot_slash_prefix_ptr(path);
    size_t display_len = strlen(display ? display : path);
    copy = strdup(display ? display : path);
    if (!copy)
        return NULL;
    bx_search_dev_counters_note_display_path_copy(display_len);
    if (path_separator != '\0' && path_separator != '/') {
        for (char *p = copy; *p; ++p) {
            if (*p == '/')
                *p = path_separator;
        }
    }
    return copy;
}

bool bx_rg_parse_path_separator(const char *progname, const char *arg,
                                char *out_separator) {
    size_t len = arg ? strlen(arg) : 0u;

    if (!out_separator)
        return false;
    if (!arg || len == 0u) {
        *out_separator = '/';
        return true;
    }
    if (len != 1u) {
        fprintf(stderr,
                "%s: error parsing flag --path-separator: A path separator must be exactly one byte, but the given separator is %zu bytes: %s\n",
                progname, len, arg);
        if (strcmp(arg, "//") == 0)
            fputs("In some shells on Windows '/' is automatically expanded. Use '//' instead.\n", stderr);
        return false;
    }
    *out_separator = arg[0];
    return true;
}

static const char *bx_rg_hyperlink_alias(const char *arg) {
    if (!arg || strcmp(arg, "none") == 0)
        return "";
    if (strcmp(arg, "default") == 0 || strcmp(arg, "file") == 0)
        return "file://{host}{path}";
    if (strcmp(arg, "cursor") == 0)
        return "file://{host}{path}#L{line},{column}";
    if (strcmp(arg, "grep+") == 0)
        return "grep+://{path}:{line}:{column}";
    if (strcmp(arg, "kitty") == 0)
        return "file://{host}{path}#{line}";
    if (strcmp(arg, "macvim") == 0)
        return "mvim://open?url=file://{path}&line={line}&column={column}";
    if (strcmp(arg, "textmate") == 0)
        return "txmt://open?url=file://{path}&line={line}&column={column}";
    if (strcmp(arg, "vscode") == 0)
        return "vscode://file{path}:{line}:{column}";
    if (strcmp(arg, "vscode-insiders") == 0)
        return "vscode-insiders://file{path}:{line}:{column}";
    if (strcmp(arg, "vscodium") == 0)
        return "vscodium://file{path}:{line}:{column}";
    return NULL;
}

static bool bx_rg_hyperlink_format_has_token(const char *format, const char *token) {
    return format && token && strstr(format, token) != NULL;
}

bool bx_rg_parse_hyperlink_format(const char *progname, const char *arg,
                                  char **out_format) {
    const char *resolved = NULL;
    char *copy = NULL;

    if (!out_format)
        return false;

    resolved = bx_rg_hyperlink_alias(arg);
    if (!resolved)
        resolved = arg ? arg : "";

    if (!bx_rg_hyperlink_format_has_token(resolved, "{path}") && resolved[0] != '\0') {
        fprintf(stderr,
                "%s: error parsing flag --hyperlink-format: invalid hyperlink format: at least a {path} variable is required in a hyperlink format, or otherwise use a valid alias: default, none, cursor, file, grep+, kitty, macvim, textmate, vscode, vscode-insiders, vscodium\n",
                progname);
        return false;
    }
    if (bx_rg_hyperlink_format_has_token(resolved, "{column}") &&
        !bx_rg_hyperlink_format_has_token(resolved, "{line}")) {
        fprintf(stderr,
                "%s: error parsing flag --hyperlink-format: invalid hyperlink format: the hyperlink format contains a {column} variable, but no {line} variable is present\n",
                progname);
        return false;
    }

    copy = strdup(resolved);
    if (!copy)
        return false;
    free(*out_format);
    *out_format = copy;
    return true;
}

static bool bx_rg_hostname_from_command(const char *command, char **out) {
    int pipefd[2] = {-1, -1};
    pid_t pid;
    char *buf = NULL;
    size_t cap = 0u;
    size_t len = 0u;
    char tmp[256];
    ssize_t nread;
    int status = 0;

    if (!command || !*command || !out)
        return false;
    if (pipe(pipefd) != 0)
        return false;

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }
    if (pid == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0)
            _exit(127);
        close(pipefd[1]);
        execlp(command, command, (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);
    while ((nread = read(pipefd[0], tmp, sizeof(tmp))) > 0) {
        if (len + (size_t)nread + 1u > cap) {
            size_t new_cap = cap == 0u ? 512u : cap * 2u;
            while (new_cap < len + (size_t)nread + 1u)
                new_cap *= 2u;
            char *grown = realloc(buf, new_cap);
            if (!grown) {
                free(buf);
                close(pipefd[0]);
                waitpid(pid, &status, 0);
                return false;
            }
            buf = grown;
            cap = new_cap;
        }
        memcpy(buf + len, tmp, (size_t)nread);
        len += (size_t)nread;
    }
    close(pipefd[0]);
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        free(buf);
        return false;
    }
    if (!buf)
        return false;

    while (len > 0u && (buf[len - 1u] == '\n' || buf[len - 1u] == '\r' || isspace((unsigned char)buf[len - 1u])))
        len--;
    buf[len] = '\0';
    if (len == 0u) {
        free(buf);
        return false;
    }
    *out = buf;
    return true;
}

static char *bx_rg_resolve_hostname_dup(const char *hostname_bin) {
    char *resolved = NULL;
    char hostbuf[256];

    if (bx_rg_hostname_from_command(hostname_bin, &resolved))
        return resolved;
    if (gethostname(hostbuf, sizeof(hostbuf)) == 0) {
        hostbuf[sizeof(hostbuf) - 1] = '\0';
        return strdup(hostbuf);
    }
    return strdup("");
}

static bool bx_rg_uri_needs_escape(unsigned char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9')) {
        return false;
    }
    switch (c) {
    case '/':
    case '-':
    case '_':
    case '.':
    case '~':
        return false;
    default:
        return true;
    }
}

static char *bx_rg_percent_encode_path(const char *path) {
    char *absolute = NULL;
    size_t extra = 0u;
    char *encoded = NULL;
    size_t out = 0u;

    if (!path)
        return NULL;
    absolute = bx_path_make_absolute_dup(path);
    if (!absolute)
        return NULL;

    for (const unsigned char *p = (const unsigned char *)absolute; *p; ++p) {
        extra += bx_rg_uri_needs_escape(*p) ? 3u : 1u;
    }

    encoded = malloc(extra + 1u);
    if (!encoded) {
        free(absolute);
        return NULL;
    }

    for (const unsigned char *p = (const unsigned char *)absolute; *p; ++p) {
        if (bx_rg_uri_needs_escape(*p)) {
            snprintf(encoded + out, 4u, "%%%02X", *p);
            out += 3u;
        } else {
            encoded[out++] = (char)*p;
        }
    }
    encoded[out] = '\0';
    free(absolute);
    return encoded;
}

static void bx_rg_append_text(char **buf, size_t *len, size_t *cap, const char *text) {
    size_t text_len = text ? strlen(text) : 0u;
    if (*len + text_len + 1u > *cap) {
        size_t new_cap = *cap == 0u ? 128u : *cap * 2u;
        while (new_cap < *len + text_len + 1u)
            new_cap *= 2u;
        char *grown = realloc(*buf, new_cap);
        if (!grown)
            return;
        *buf = grown;
        *cap = new_cap;
    }
    memcpy(*buf + *len, text, text_len);
    *len += text_len;
    (*buf)[*len] = '\0';
}

static void bx_rg_append_number(char **buf, size_t *len, size_t *cap, size_t value) {
    char numbuf[64];
    snprintf(numbuf, sizeof(numbuf), "%zu", value);
    bx_rg_append_text(buf, len, cap, numbuf);
}

char *bx_rg_hyperlink_open_dup(const char *format, const char *hostname_bin,
                               const char *path, size_t line, size_t column,
                               bool have_line, bool have_column) {
    const char *wsl = NULL;
    char *encoded_path = NULL;
    char *hostname = NULL;
    char *buf = NULL;
    size_t len = 0u;
    size_t cap = 0u;

    if (!format || format[0] == '\0' || !path || !bx_color_enabled())
        return NULL;

    wsl = getenv("WSL_DISTRO_NAME");
    encoded_path = bx_rg_percent_encode_path(path);
    hostname = bx_rg_resolve_hostname_dup(hostname_bin);
    if (!encoded_path || !hostname) {
        free(encoded_path);
        free(hostname);
        return NULL;
    }

    for (const char *p = format; *p; ) {
        if (strncmp(p, "{path}", 6) == 0) {
            bx_rg_append_text(&buf, &len, &cap, encoded_path);
            p += 6;
            continue;
        }
        if (strncmp(p, "{host}", 6) == 0) {
            bx_rg_append_text(&buf, &len, &cap, hostname);
            p += 6;
            continue;
        }
        if (strncmp(p, "{line}", 6) == 0) {
            bx_rg_append_number(&buf, &len, &cap, have_line ? line : 1u);
            p += 6;
            continue;
        }
        if (strncmp(p, "{column}", 8) == 0) {
            bx_rg_append_number(&buf, &len, &cap, have_column ? column : 1u);
            p += 8;
            continue;
        }
        if (strncmp(p, "{wslprefix}", 11) == 0) {
            if (wsl && *wsl) {
                bx_rg_append_text(&buf, &len, &cap, "wsl$/");
                bx_rg_append_text(&buf, &len, &cap, wsl);
            }
            p += 11;
            continue;
        }
        char tmp[2] = {*p++, '\0'};
        bx_rg_append_text(&buf, &len, &cap, tmp);
    }

    free(encoded_path);
    free(hostname);
    if (!buf)
        return NULL;

    size_t need = len + strlen("\033]8;;") + strlen("\033\\") + 1u;
    char *wrapped = malloc(need);
    if (!wrapped) {
        free(buf);
        return NULL;
    }
    snprintf(wrapped, need, "\033]8;;%s\033\\", buf);
    free(buf);
    return wrapped;
}

const char *bx_rg_hyperlink_close(void) {
    return "\033]8;;\033\\";
}
