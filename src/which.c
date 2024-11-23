#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <limits.h>
#include <ctype.h>

#include "which.h"
#include "diag.h"
#include "libbx.h"

struct which_opts {
    bool all;
    bool skip_dot;
    bool skip_tilde;
    bool show_dot;
    bool show_tilde;
    bool tty_only;
    bool read_alias;
    bool skip_alias;
    bool read_functions;
    bool skip_functions;
};

struct alias {
    char* name;
    char* definition;
    struct alias* next;
};

struct function {
    char* name;
    char* definition;
    struct function* next;
};

static const char* which_progname(const char* argv0) {
    return (argv0 && argv0[0] != '\0') ? argv0 : "which";
}

static void print_help(FILE* stream, const char* progname) {
    fprintf(stream, "usage: %s [options] [--] COMMAND [...]\n", progname);
    fprintf(stream, "\n");
    fprintf(stream, "options:\n");
    fprintf(stream, "  -a, --all             print all matches in PATH, not just the first one\n");
    fprintf(stream, "  --skip-dot            skip directories in PATH that start with a dot\n");
    fprintf(stream, "  --skip-tilde          skip directories in PATH that start with a tilde\n");
    fprintf(stream, "  --show-dot            don't expand dot to current directory in output\n");
    fprintf(stream, "  --show-tilde          output a tilde for HOME directory for non-root\n");
    fprintf(stream, "  --tty-only            stop processing options on the right if not on tty\n");
    fprintf(stream, "  -i, --read-alias      read alias definitions from stdin\n");
    fprintf(stream, "  --skip-alias          do not read alias definitions (default)\n");
    fprintf(stream, "  --read-functions      read function definitions from stdin\n");
    fprintf(stream, "  --skip-functions      do not read function definitions (default)\n");
    fprintf(stream, "  -v, -V, --version     print bx version information\n");
    fprintf(stream, "  --help                print this help text\n");
}

static void print_version(void) {
    printf("bx which version %s\n", BX_VERSION);
}

static void print_missing_search_space(FILE* stream, const char* cmd) {
    const char* slash = strrchr(cmd, '/');
    if (!slash) {
        const char* path_env = getenv("PATH");
        fprintf(stream, "%s", path_env ? path_env : "(null)");
        return;
    }

    size_t dir_len = (size_t)(slash - cmd);
    if (dir_len == 0) {
        fputc('/', stream);
        return;
    }

    if (cmd[0] != '/' && cmd[0] != '.') {
        fputs("./", stream);
    }
    fwrite(cmd, 1, dir_len, stream);
}

static void print_not_found_diag(struct bx_diag_ctx* diag, const char* cmd) {
    const char* slash = strrchr(cmd, '/');
    const char* name = slash ? slash + 1 : cmd;
    if (*name == '\0') {
        name = cmd;
    }

    fprintf(stderr, "%s: no %s in (", diag->progname, name);
    print_missing_search_space(stderr, cmd);
    fputs(")\n", stderr);
    diag->exit_status++;
}

static bool is_executable(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    return S_ISREG(st.st_mode) && (access(path, X_OK) == 0);
}

static bool path_has_dot_component(const char* path) {
    const char* p = path;

    while (*p != '\0') {
        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        if (*p == '.') {
            return true;
        }
        while (*p != '\0' && *p != '/') {
            p++;
        }
    }

    return false;
}

static bool path_starts_with_dot_component(const char* path) {
    while (*path == '/') {
        path++;
    }
    return path[0] == '.';
}

static bool normalize_lexical_path(const char* path, bool absolute, char* out, size_t out_size) {
    char temp[PATH_MAX];
    char* parts[PATH_MAX / 2];
    size_t count = 0;
    char* saveptr = NULL;

    if (absolute && path[0] != '/') {
        return false;
    }
    if (snprintf(temp, sizeof(temp), "%s", path) < 0 || strlen(path) >= sizeof(temp)) {
        return false;
    }

    for (char* token = strtok_r(temp, "/", &saveptr); token != NULL; token = strtok_r(NULL, "/", &saveptr)) {
        if (strcmp(token, ".") == 0 || token[0] == '\0') {
            continue;
        }
        if (strcmp(token, "..") == 0) {
            if (count > 0 && (absolute || strcmp(parts[count - 1], "..") != 0)) {
                count--;
            }
            else if (!absolute) {
                if (count >= (sizeof(parts) / sizeof(parts[0]))) {
                    return false;
                }
                parts[count++] = token;
            }
            continue;
        }
        if (count >= (sizeof(parts) / sizeof(parts[0]))) {
            return false;
        }
        parts[count++] = token;
    }

    size_t pos = 0;
    if (out_size < 2) {
        return false;
    }
    if (absolute) {
        out[pos++] = '/';
    }
    for (size_t i = 0; i < count; i++) {
        size_t len = strlen(parts[i]);
        if (pos + len + 1 >= out_size) {
            return false;
        }
        memcpy(out + pos, parts[i], len);
        pos += len;
        if (i + 1 < count) {
            out[pos++] = '/';
        }
    }
    if (count == 0 && absolute) {
        out[pos] = '\0';
        return true;
    }
    if (count == 0) {
        out[0] = '.';
        out[1] = '\0';
        return true;
    }
    out[pos] = '\0';
    return true;
}

static bool build_normalized_operand_path(const char* cmd, char* out, size_t out_size) {
    char absolute[PATH_MAX];

    if (cmd[0] == '/') {
        return normalize_lexical_path(cmd, true, out, out_size);
    }

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        return false;
    }
    if (snprintf(absolute, sizeof(absolute), "%s/%s", cwd, cmd) < 0 || strlen(cwd) + 1 + strlen(cmd) >= sizeof(absolute)) {
        return false;
    }
    return normalize_lexical_path(absolute, true, out, out_size);
}

static bool build_normalized_search_hit_path(const char* dir, const char* cmd, char* out, size_t out_size) {
    char full_path[PATH_MAX];

    if (dir[0] == '/') {
        int r = snprintf(full_path, sizeof(full_path), "%s/%s", dir, cmd);
        if (r < 0 || (size_t)r >= sizeof(full_path)) {
            return false;
        }
        return normalize_lexical_path(full_path, true, out, out_size);
    }

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        return false;
    }
    int r = snprintf(full_path, sizeof(full_path), "%s/%s/%s", cwd, dir, cmd);
    if (r < 0 || (size_t)r >= sizeof(full_path)) {
        return false;
    }
    return normalize_lexical_path(full_path, true, out, out_size);
}

static bool build_show_dot_search_hit_path(const char* dir, const char* cmd, char* out, size_t out_size) {
    char relative_path[PATH_MAX];
    char normalized[PATH_MAX];

    int r = snprintf(relative_path, sizeof(relative_path), "%s/%s", dir, cmd);
    if (r < 0 || (size_t)r >= sizeof(relative_path)) {
        return false;
    }
    if (!normalize_lexical_path(relative_path, false, normalized, sizeof(normalized))) {
        return false;
    }
    r = snprintf(out, out_size, "./%s", normalized);
    return r >= 0 && (size_t)r < out_size;
}

static void print_path(const char* path, const struct which_opts* opts, const char* home) {
    if (opts->show_tilde && home && strlen(home) > 0 && getuid() != 0) {
        if (strncmp(path, home, strlen(home)) == 0) {
            printf("~%s\n", path + strlen(home));
            return;
        }
    }
    printf("%s\n", path);
}

static bool has_operand_name(int operand_count, char** operands, const char* name) {
    for (int i = 0; i < operand_count; i++) {
        if (strcmp(operands[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static bool consume_first_matching_operand(int operand_count, char** operands, bool* consumed, const char* name) {
    for (int i = 0; i < operand_count; i++) {
        if (!consumed[i] && strcmp(operands[i], name) == 0) {
            consumed[i] = true;
            return true;
        }
    }
    return false;
}

static void emit_aliases_for_operands(struct alias* aliases, int operand_count, char** operands, bool all, bool* consumed) {
    for (struct alias* a = aliases; a; a = a->next) {
        bool matched = all ? has_operand_name(operand_count, operands, a->name) : consume_first_matching_operand(operand_count, operands, consumed, a->name);
        if (!matched) {
            continue;
        }
        printf("%s", a->definition);
    }
}

/*
 * Ownership Rules:
 * - struct alias and struct function nodes are owned by the linked lists
 *   managed in bx_which_main.
 * - String fields (name, value, definition) are owned by the nodes and
 *   must be freed when the node is freed.
 * - parse_stdin is responsible for capturing complete definitions or
 *   cleaning up partial ones.
 */

static void free_aliases(struct alias* aliases) {
    while (aliases) {
        struct alias* next = aliases->next;
        free(aliases->name);
        free(aliases->definition);
        free(aliases);
        aliases = next;
    }
}

static void free_functions(struct function* functions) {
    while (functions) {
        struct function* next = functions->next;
        free(functions->name);
        free(functions->definition);
        free(functions);
        functions = next;
    }
}

static char* get_trimmed_copy(const char* str) {
    while (isspace((unsigned char)*str))
        str++;
    if (*str == 0)
        return xstrdup("");
    const char* end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end))
        end--;
    size_t len = (size_t)(end - str + 1);
    char* res = xmalloc(len + 1);
    memcpy(res, str, len);
    res[len] = '\0';
    return res;
}

static bool parse_alias_definition(const char* line, char** name_out, char** definition_out) {
    const char* p = line;

    while (isspace((unsigned char)*p)) {
        p++;
    }
    if (strncmp(p, "alias", 5) != 0 || !isspace((unsigned char)p[5])) {
        return false;
    }
    p += 5;
    while (isspace((unsigned char)*p)) {
        p++;
    }

    const char* eq = strchr(p, '=');
    if (!eq) {
        return false;
    }

    const char* name_end = eq;
    while (name_end > p && isspace((unsigned char)name_end[-1])) {
        name_end--;
    }
    if (name_end == p) {
        return false;
    }

    char* name = xmalloc((size_t)(name_end - p) + 1);
    memcpy(name, p, (size_t)(name_end - p));
    name[name_end - p] = '\0';

    *name_out = name;
    *definition_out = xstrdup(line);
    return true;
}

static bool parse_function_header(const char* trimmed, const char** name_start_out, size_t* name_len_out) {
    const char* p = trimmed;
    const char* name_start = p;

    if (*p == '\0') {
        return false;
    }
    while (*p != '\0' && !isspace((unsigned char)*p) && *p != '(') {
        p++;
    }
    if (p == name_start || !isspace((unsigned char)*p)) {
        return false;
    }

    const char* name_end = p;
    while (isspace((unsigned char)*p)) {
        p++;
    }
    if (p[0] != '(' || p[1] != ')') {
        return false;
    }
    p += 2;
    while (isspace((unsigned char)*p)) {
        p++;
    }
    if (*p != '\0') {
        if (*p != '{') {
            return false;
        }
        p++;
        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (*p != '\0') {
            return false;
        }
    }

    *name_start_out = name_start;
    *name_len_out = (size_t)(name_end - name_start);
    return true;
}

static void parse_stdin(struct which_opts* opts, struct alias** aliases, struct function** functions) {
    if ((!opts->read_alias || opts->skip_alias) && (!opts->read_functions || opts->skip_functions)) {
        return;
    }
    char* line = NULL;
    size_t line_len = 0;
    ssize_t nread;
    struct alias** a_tail = aliases;
    struct function* f_head = NULL;
    struct function** f_tail = &f_head;
    struct function* curr = NULL;
    int depth = 0;
    size_t b_size = 0;
    size_t b_len = 0;
    bool saw_function_header = false;

    while (*a_tail) {
        a_tail = &(*a_tail)->next;
    }

    while ((nread = getline(&line, &line_len, stdin)) != -1) {
        if (curr) {
            // Capture raw line including newline
            if (b_len + (size_t)nread + 1 > b_size) {
                b_size = (b_len + (size_t)nread + 1) + 1024;
                curr->definition = xrealloc(curr->definition, b_size);
            }
            memcpy(curr->definition + b_len, line, (size_t)nread);
            b_len += (size_t)nread;
            curr->definition[b_len] = '\0';

            // Track braces in raw line
            for (ssize_t i = 0; i < nread; i++) {
                if (line[i] == '{')
                    depth++;
                else if (line[i] == '}')
                    depth--;
            }

            if (depth <= 0 && strchr(curr->definition, '{')) {
                *f_tail = curr;
                f_tail = &curr->next;
                curr = NULL;
                depth = 0;
                b_size = 0;
                b_len = 0;
            }
        }
        else {
            char* trimmed = get_trimmed_copy(line);
            if (!trimmed)
                break;
            if (*trimmed == '\0') {
                free(trimmed);
                continue;
            }

            if (opts->read_alias && !opts->skip_alias && !(opts->read_functions && !opts->skip_functions && saw_function_header)) {
                char* name = NULL;
                char* definition = NULL;
                if (parse_alias_definition(line, &name, &definition)) {
                    struct alias* na = xmalloc(sizeof(struct alias));
                    na->name = name;
                    na->definition = definition;
                    na->next = NULL;
                    *a_tail = na;
                    a_tail = &na->next;
                    free(trimmed);
                    continue;
                }
            }

            if (opts->read_functions && !opts->skip_functions) {
                const char* name_start = NULL;
                size_t name_len = 0;

                if (parse_function_header(trimmed, &name_start, &name_len)) {
                    saw_function_header = true;
                    curr = xmalloc(sizeof(struct function));
                    curr->name = xmalloc(name_len + 1);
                    memcpy(curr->name, name_start, name_len);
                    curr->name[name_len] = '\0';
                    curr->next = NULL;
                    b_size = (size_t)nread + 1024;
                    curr->definition = xmalloc(b_size);
                    memcpy(curr->definition, line, (size_t)nread);
                    b_len = (size_t)nread;
                    curr->definition[b_len] = '\0';
                    depth = 0;
                    for (ssize_t i = 0; i < nread; i++) {
                        if (line[i] == '{')
                            depth++;
                        else if (line[i] == '}')
                            depth--;
                    }
                    if (depth <= 0 && strchr(curr->definition, '{')) {
                        *f_tail = curr;
                        f_tail = &curr->next;
                        curr = NULL;
                    }
                }
            }
            free(trimmed);
        }
    }
    // Cleanup any incomplete function definition at EOF
    if (curr) {
        *f_tail = curr;
        f_tail = &curr->next;
    }
    *functions = f_head;
    free(line);
}

static bool find_command(const char* cmd, const struct which_opts* opts, struct alias* aliases, struct function* functions) {
    const char* home = getenv("HOME");
    bool found = false;

    for (struct alias* a = aliases; a; a = a->next) {
        if (strcmp(a->name, cmd) == 0) {
            printf("%s", a->definition);
            found = true;
            if (!opts->all)
                return true;
        }
    }

    for (struct function* f = functions; f; f = f->next) {
        if (strcmp(f->name, cmd) == 0) {
            if (f->definition) {
                printf("%s", f->definition);
            }
            else {
                printf("%s ()\n", f->name);
            }
            found = true;
            if (!opts->all)
                return true;
        }
    }

    if (strchr(cmd, '/') != NULL) {
        if (is_executable(cmd)) {
            char normalized_path[PATH_MAX];
            if (build_normalized_operand_path(cmd, normalized_path, sizeof(normalized_path))) {
                print_path(normalized_path, opts, home);
            }
            else {
                print_path(cmd, opts, home);
            }
            return true;
        }
        return found;
    }

    const char* path_env = getenv("PATH");
    if (!path_env)
        return found;
    if (path_env[0] == '\0')
        return found;

    const char* p = path_env;
    const char* next;
    do {
        char dir[PATH_MAX];
        char lookup_path[PATH_MAX];
        char display_path[PATH_MAX];
        next = strchr(p, ':');
        size_t len = next ? (size_t)(next - p) : strlen(p);
        if (len == 0)
            strcpy(dir, ".");
        else if (len >= sizeof(dir))
            goto next_comp;
        else {
            memcpy(dir, p, len);
            dir[len] = '\0';
        }

        const char* effective_dir = dir;
        if (opts->skip_tilde && effective_dir[0] == '~')
            goto next_comp;
        if (opts->skip_dot && effective_dir[0] != '/' && effective_dir[0] != '~' && path_has_dot_component(effective_dir)) {
            goto next_comp;
        }

        char expanded_dir[PATH_MAX];
        const char* search_dir = effective_dir;
        if (effective_dir[0] == '~' && home && home[0] != '\0' && (effective_dir[1] == '\0' || effective_dir[1] == '/')) {
            int r = snprintf(expanded_dir, sizeof(expanded_dir), "%s%s", home, effective_dir + 1);
            if (r < 0 || (size_t)r >= sizeof(expanded_dir))
                goto next_comp;
            search_dir = expanded_dir;
        }

        int r = snprintf(lookup_path, sizeof(lookup_path), "%s/%s", search_dir, cmd);
        if (r < 0 || (size_t)r >= sizeof(lookup_path)) {
            goto next_comp;
        }

        bool show_dot_hit = opts->show_dot && effective_dir[0] != '/' && effective_dir[0] != '~' && path_starts_with_dot_component(effective_dir);

        if (show_dot_hit) {
            if (!build_show_dot_search_hit_path(effective_dir, cmd, display_path, sizeof(display_path))) {
                goto next_comp;
            }
        }
        else if (!build_normalized_search_hit_path(search_dir, cmd, display_path, sizeof(display_path))) {
            goto next_comp;
        }

        if (is_executable(lookup_path)) {
            print_path(display_path, opts, home);
            found = true;
            if (!opts->all)
                return true;
        }
    next_comp:
        if (next)
            p = next + 1;
    } while (next);

    return found;
}

int bx_which_main(int argc, char** argv) {
    struct which_opts opts = {0};
    int c;
    struct alias* aliases = NULL;
    struct function* functions = NULL;
    struct bx_diag_ctx diag = {0};

    diag.progname = which_progname(argv[0]);

    static struct option long_options[] = {{"all", no_argument, 0, 'a'},
                                           {"skip-dot", no_argument, 0, 1001},
                                           {"skip-tilde", no_argument, 0, 1002},
                                           {"show-dot", no_argument, 0, 1003},
                                           {"show-tilde", no_argument, 0, 1004},
                                           {"tty-only", no_argument, 0, 1005},
                                           {"read-alias", no_argument, 0, 'i'},
                                           {"skip-alias", no_argument, 0, 1006},
                                           {"read-functions", no_argument, 0, 1007},
                                           {"skip-functions", no_argument, 0, 1008},
                                           {"version", no_argument, 0, 'v'},
                                           {"help", no_argument, 0, 1009},
                                           {0, 0, 0, 0}};

    opterr = 0;
    while ((c = getopt_long(argc, argv, "aivV", long_options, NULL)) != -1) {
        switch (c) {
            case 'a':
                opts.all = true;
                break;
            case 'i':
                if (!opts.skip_alias) {
                    opts.read_alias = true;
                }
                break;
            case 'v':
            case 'V':
                print_version();
                return 0;
            case 1001:
                opts.skip_dot = true;
                break;
            case 1002:
                opts.skip_tilde = true;
                break;
            case 1003:
                opts.show_dot = true;
                break;
            case 1004:
                opts.show_tilde = true;
                break;
            case 1005:
                opts.tty_only = true;
                break;
            case 1006:
                opts.read_alias = false;
                opts.skip_alias = true;
                break;
            case 1007:
                if (!opts.skip_functions) {
                    opts.read_functions = true;
                }
                break;
            case 1008:
                opts.read_functions = false;
                opts.skip_functions = true;
                break;
            case 1009:
                print_help(stdout, diag.progname);
                return 0;
            case '?':
                if (opts.tty_only) {
                    if (optopt != 0) {
                        fprintf(stderr, "%s: invalid option -- '%c'\n", diag.progname, optopt);
                    }
                    else {
                        fprintf(stderr, "%s: unrecognized option '%s'\n", diag.progname, argv[optind - 1]);
                    }
                    break;
                }
                if (optopt != 0) {
                    fprintf(stderr, "%s: invalid option -- '%c'\n", diag.progname, optopt);
                    print_help(stderr, diag.progname);
                    return 255;
                }
                fprintf(stderr, "%s: unrecognized option '%s'\n", diag.progname, argv[optind - 1]);
                print_help(stderr, diag.progname);
                return 255;
            default:
                return 255;
        }
    }

    if (optind >= argc) {
        print_help(stderr, diag.progname);
        return 255;
    }

    parse_stdin(&opts, &aliases, &functions);

    int operand_count = argc - optind;
    bool* alias_consumed = xmalloc((size_t)operand_count * sizeof(bool));
    memset(alias_consumed, 0, (size_t)operand_count * sizeof(bool));
    emit_aliases_for_operands(aliases, operand_count, argv + optind, opts.all, alias_consumed);

    for (int i = 0; i < operand_count; i++) {
        if (!opts.all && alias_consumed[i]) {
            continue;
        }
        if (!find_command(argv[optind + i], &opts, NULL, functions)) {
            print_not_found_diag(&diag, argv[optind + i]);
        }
    }

    free(alias_consumed);
    free_aliases(aliases);
    free_functions(functions);
    return diag.exit_status > 255 ? 255 : diag.exit_status;
}
