#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <regex.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "applets.h"
#include "applets/system/psmisc/procfs.h"
#include "applets/system/psmisc/psmisc_wrapper.h"
#include "applets/system/psmisc/signals.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/id_parse.h"
#include "lib/prompt_ops.h"
#include "lib/args_common.h"

struct bx_killall_options {
    int signal_number;
    bool exact;
    bool process_group;
    bool interactive;
    bool ignore_case;
    bool list_signals;
    bool quiet;
    bool regexp;
    bool verbose;
    bool wait;
    bool older_than_set;
    bool younger_than_set;
    double older_than_seconds;
    double younger_than_seconds;
    bool user_set;
    uid_t user;
    bool ns_set;
    pid_t ns_pid;
    bool context_set;
    regex_t context_regex;
    bool context_regex_ready;
    int first_name_index;
};

static void bx_killall_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [--] NAME...\n", progname);
    fprintf(stream, "       %s -l\n", progname);
    fprintf(stream, "Send signals to processes selected by name.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -e, --exact            require exact match for long names\n");
    fprintf(stream, "  -g, --process-group    signal the process group instead of each PID\n");
    fprintf(stream, "  -i, --interactive      ask before signaling each match\n");
    fprintf(stream, "  -I, --ignore-case      match process names case-insensitively\n");
    fprintf(stream, "  -l, --list             list known signal names\n");
    fprintf(stream, "  -n, --ns PID           match processes in PID's namespaces\n");
    fprintf(stream, "  -o, --older-than TIME  match processes older than TIME\n");
    fprintf(stream, "  -q, --quiet            suppress complaints about missing matches\n");
    fprintf(stream, "  -r, --regexp           treat NAME as an extended regular expression\n");
    fprintf(stream, "  -s, --signal SIGNAL    send SIGNAL instead of TERM\n");
    fprintf(stream, "  -u, --user USER        match only processes owned by USER\n");
    fprintf(stream, "  -v, --verbose          report each successful signal\n");
    fprintf(stream, "  -w, --wait             wait for matched processes to exit\n");
    fprintf(stream, "  -y, --younger-than TIME\n");
    fprintf(stream, "                        match processes younger than TIME\n");
    fprintf(stream, "  -Z, --context REGEXP   match only processes with a matching context\n");
    fprintf(stream, "  -h, --help             display this help and exit\n");
    fprintf(stream, "  -V, --version          output version information and exit\n");
}

static void bx_killall_options_cleanup(struct bx_killall_options* options) {
    if (options->context_regex_ready) {
        regfree(&options->context_regex);
        options->context_regex_ready = false;
    }
}

static bool bx_killall_parse_duration(const char* text, double* seconds_out) {
    char* end;
    double value;
    double factor = 1.0;

    if (text == NULL || text[0] == '\0' || seconds_out == NULL) {
        return false;
    }

    errno = 0;
    value = strtod(text, &end);
    if (errno != 0 || end == text || value < 0.0 || !isfinite(value)) {
        return false;
    }
    if (*end == '\0') {
        *seconds_out = value;
        return true;
    }
    if (end[1] != '\0') {
        return false;
    }
    switch (*end) {
        case 's': factor = 1.0; break;
        case 'm': factor = 60.0; break;
        case 'h': factor = 3600.0; break;
        case 'd': factor = 86400.0; break;
        default: return false;
    }
    double seconds = value * factor;
    if (!isfinite(seconds)) {
        return false;
    }
    *seconds_out = seconds;
    return true;
}

static bool bx_killall_is_signal_short_option(const char* arg) {
    static const char* known = "egiIlnoqrsuvwyZhV";
    if (arg == NULL || arg[0] != '-' || arg[1] == '\0' || arg[1] == '-') {
        return false;
    }
    if (isdigit((unsigned char)arg[1])) {
        return true;
    }
    return strchr(known, arg[1]) == NULL;
}

static bool bx_killall_parse_options(struct bx_killall_options* options,
                                     int argc,
                                     char** argv,
                                     struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"exact", no_argument, NULL, 'e'},
        {"process-group", no_argument, NULL, 'g'},
        {"interactive", no_argument, NULL, 'i'},
        {"ignore-case", no_argument, NULL, 'I'},
        {"list", no_argument, NULL, 'l'},
        {"ns", required_argument, NULL, 'n'},
        {"older-than", required_argument, NULL, 'o'},
        {"quiet", no_argument, NULL, 'q'},
        {"regexp", no_argument, NULL, 'r'},
        {"signal", required_argument, NULL, 's'},
        {"user", required_argument, NULL, 'u'},
        {"verbose", no_argument, NULL, 'v'},
        {"wait", no_argument, NULL, 'w'},
        {"younger-than", required_argument, NULL, 'y'},
        {"context", required_argument, NULL, 'Z'},
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {NULL, 0, NULL, 0},
    };
    int c;
    struct bx_id_user user;

    memset(options, 0, sizeof(*options));
    options->signal_number = SIGTERM;
    if (argc > 1 && bx_killall_is_signal_short_option(argv[1])) {
        options->signal_number = get_signal(argv[1] + 1, diag->progname);
        argv[1] = (char*)"--";
    }

    bx_args_getopt_reset();
    while ((c = bx_args_getopt_long(argc, argv, "+egiIln:o:qrs:u:vwy:Z:hV", long_options, NULL)) != -1) {
        switch (c) {
            case 'e': options->exact = true; break;
            case 'g': options->process_group = true; break;
            case 'i': options->interactive = true; break;
            case 'I': options->ignore_case = true; break;
            case 'l': options->list_signals = true; break;
            case 'n':
                if (!bx_proc_parse_pid_arg(optarg, &options->ns_pid)) {
                    bx_diag(diag, "invalid namespace PID");
                    return false;
                }
                options->ns_set = true;
                break;
            case 'o':
                if (!bx_killall_parse_duration(optarg, &options->older_than_seconds)) {
                    bx_diag(diag, "invalid time '%s'", optarg);
                    return false;
                }
                options->older_than_set = true;
                break;
            case 'q': options->quiet = true; break;
            case 'r': options->regexp = true; break;
            case 's':
                options->signal_number = get_signal(optarg, diag->progname);
                break;
            case 'u':
                if (!bx_id_parse_user(optarg, &user, diag)) {
                    return false;
                }
                options->user = user.uid;
                options->user_set = true;
                break;
            case 'v': options->verbose = true; break;
            case 'w': options->wait = true; break;
            case 'y':
                if (!bx_killall_parse_duration(optarg, &options->younger_than_seconds)) {
                    bx_diag(diag, "invalid time '%s'", optarg);
                    return false;
                }
                options->younger_than_set = true;
                break;
            case 'Z':
                if (regcomp(&options->context_regex, optarg, REG_EXTENDED | REG_NOSUB) != 0) {
                    bx_diag(diag, "invalid context regexp: %s", optarg);
                    return false;
                }
                options->context_set = true;
                options->context_regex_ready = true;
                break;
            case 'h':
            case 'V':
                return true;
            case '?':
                if (optopt != 0) {
                    bx_diag(diag, "invalid option -- '%c'", optopt);
                }
                else if (optind > 0 && optind <= argc && argv[optind - 1] != NULL) {
                    bx_diag(diag, "unrecognized option '%s'", argv[optind - 1]);
                }
                else {
                    bx_diag(diag, "unrecognized option");
                }
                return false;
            default:
                return false;
        }
    }

    options->first_name_index = optind;
    if (!options->list_signals && options->first_name_index >= argc) {
        bx_diag(diag, "missing process name");
        return false;
    }
    return true;
}

static bool bx_killall_name_equals(const char* left, const char* right, bool ignore_case) {
    if (left == NULL || right == NULL) {
        return false;
    }
    return ignore_case ? strcasecmp(left, right) == 0 : strcmp(left, right) == 0;
}

static bool bx_killall_name_prefix_match(const char* pattern, const char* candidate, bool ignore_case) {
    size_t pattern_len;
    size_t candidate_len;
    if (pattern == NULL || candidate == NULL) {
        return false;
    }
    pattern_len = strlen(pattern);
    candidate_len = strlen(candidate);
    if (pattern_len == 0u || candidate_len == 0u) {
        return false;
    }
    if (candidate_len < pattern_len) {
        return false;
    }
    return ignore_case ? strncasecmp(candidate, pattern, pattern_len) == 0 : strncmp(candidate, pattern, pattern_len) == 0;
}

#define BX_KILLALL_CMD_STORAGE 256u

static const char* bx_killall_basename_span(const char* text, size_t len, size_t* base_len_out) {
    size_t start = 0u;
    size_t i;

    for (i = 0u; i < len; i++) {
        if (text[i] == '/') {
            start = i + 1u;
        }
    }
    *base_len_out = len - start;
    return text + start;
}

static const char* bx_killall_long_cmd(const struct bx_proc_info* info, char* storage, size_t storage_size) {
    const char* cursor;
    size_t comm_len;

    if (info->comm == NULL || info->comm[0] == '\0' || info->cmdline == NULL || info->cmdline[0] == '\0') {
        return NULL;
    }
    comm_len = strlen(info->comm);
    if (comm_len == 0u) {
        return NULL;
    }

    cursor = info->cmdline;
    while (*cursor != '\0') {
        const char* token;
        const char* base;
        size_t token_len;
        size_t base_len;

        while (*cursor == ' ') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        token = cursor;
        while (*cursor != '\0' && *cursor != ' ') {
            cursor++;
        }
        token_len = (size_t)(cursor - token);
        base = bx_killall_basename_span(token, token_len, &base_len);
        if (base_len >= comm_len && strncmp(base, info->comm, comm_len) == 0) {
            if (base_len >= storage_size) {
                base_len = storage_size - 1u;
            }
            memcpy(storage, base, base_len);
            storage[base_len] = '\0';
            return storage;
        }
    }

    return NULL;
}

static const char* bx_killall_cmd0(const struct bx_proc_info* info, char* storage, size_t storage_size) {
    size_t len;
    const char* src;
    if (info->cmdline != NULL && info->cmdline[0] != '\0') {
        const char* space = strchr(info->cmdline, ' ');
        len = space != NULL ? (size_t)(space - info->cmdline) : strlen(info->cmdline);
        if (len >= storage_size) {
            len = storage_size - 1u;
        }
        memcpy(storage, info->cmdline, len);
        storage[len] = '\0';
        src = strrchr(storage, '/');
        return src != NULL && src[1] != '\0' ? src + 1 : storage;
    }
    if (info->exe != NULL && info->exe[0] != '\0') {
        src = strrchr(info->exe, '/');
        return src != NULL && src[1] != '\0' ? src + 1 : info->exe;
    }
    return info->comm;
}

static const char* bx_killall_exe_base(const struct bx_proc_info* info) {
    const char* src;

    if (info->exe == NULL || info->exe[0] == '\0') {
        return NULL;
    }
    src = strrchr(info->exe, '/');
    return src != NULL && src[1] != '\0' ? src + 1 : info->exe;
}

static bool bx_killall_single_name_matches(const struct bx_killall_options* options,
                                           const char* pattern,
                                           regex_t* regex_or_null,
                                           const char* candidate) {
    if (candidate == NULL || candidate[0] == '\0') {
        return false;
    }
    if (options->regexp) {
        return regexec(regex_or_null, candidate, 0, NULL, 0) == 0;
    }
    return bx_killall_name_equals(pattern, candidate, options->ignore_case)
        || (!options->exact && bx_killall_name_prefix_match(pattern, candidate, options->ignore_case));
}

static bool bx_killall_name_matches(const struct bx_killall_options* options,
                                    const struct bx_proc_info* info,
                                    const char* pattern,
                                    regex_t* regex_or_null) {
    char cmd0_storage[BX_KILLALL_CMD_STORAGE];
    char long_storage[BX_KILLALL_CMD_STORAGE];
    const char* cmd0 = bx_killall_cmd0(info, cmd0_storage, sizeof(cmd0_storage));
    const char* long_cmd = bx_killall_long_cmd(info, long_storage, sizeof(long_storage));
    const char* exe_base = bx_killall_exe_base(info);

    if (bx_killall_single_name_matches(options, pattern, regex_or_null, long_cmd)
        || bx_killall_single_name_matches(options, pattern, regex_or_null, info->comm)
        || bx_killall_single_name_matches(options, pattern, regex_or_null, cmd0)
        || bx_killall_single_name_matches(options, pattern, regex_or_null, exe_base)) {
        return true;
    }
    return false;
}

static bool bx_killall_same_namespaces(pid_t left_pid, pid_t right_pid) {
    static const char* names[] = {"pid", "mnt", "net", "ipc", "uts", "user"};
    size_t i;
    for (i = 0u; i < sizeof(names) / sizeof(names[0]); i++) {
        char leaf[32];
        char* left = NULL;
        char* right = NULL;
        bool vanished = false;
        snprintf(leaf, sizeof(leaf), "ns/%s", names[i]);
        if (!bx_proc_readlink_leaf(left_pid, leaf, &left, &vanished)) {
            free(left);
            free(right);
            return false;
        }
        if (!bx_proc_readlink_leaf(right_pid, leaf, &right, &vanished)) {
            free(left);
            free(right);
            return false;
        }
        if (strcmp(left, right) != 0) {
            free(left);
            free(right);
            return false;
        }
        free(left);
        free(right);
    }
    return true;
}

static bool bx_killall_context_matches(const struct bx_killall_options* options, pid_t pid) {
    char* text = NULL;
    bool vanished = false;
    bool matched = false;
    if (!options->context_set) {
        return true;
    }
    if (!bx_proc_read_text_file(pid, "attr/current", &text, &vanished)) {
        return false;
    }
    if (text[0] != '\0') {
        size_t len = strlen(text);
        while (len > 0u && (text[len - 1u] == '\n' || text[len - 1u] == '\r')) {
            text[--len] = '\0';
        }
    }
    matched = regexec(&options->context_regex, text, 0, NULL, 0) == 0;
    free(text);
    return matched;
}

static bool bx_killall_age_matches(const struct bx_killall_options* options,
                                   const struct bx_proc_info* info,
                                   double uptime_seconds,
                                   long ticks_per_second) {
    double age;
    age = uptime_seconds - ((double)info->starttime_ticks / (double)ticks_per_second);
    if (options->older_than_set && age < options->older_than_seconds) {
        return false;
    }
    if (options->younger_than_set && age > options->younger_than_seconds) {
        return false;
    }
    return true;
}

static bool bx_killall_id_seen(const pid_t* ids, size_t len, pid_t id) {
    size_t i;
    for (i = 0u; i < len; i++) {
        if (ids[i] == id) {
            return true;
        }
    }
    return false;
}

static bool bx_killall_wait_for_ids(const pid_t* ids, size_t len, bool process_group) {
    size_t remaining = len;
    while (remaining > 0u) {
        size_t i;
        remaining = 0u;
        for (i = 0u; i < len; i++) {
            pid_t id = ids[i];
            int rc = process_group ? kill(-id, 0) : kill(id, 0);
            if (rc == 0 || (rc < 0 && errno == EPERM)) {
                remaining++;
            }
        }
        if (remaining == 0u) {
            break;
        }
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000000L};
        nanosleep(&ts, NULL);
    }
    return true;
}

static bool bx_killall_signal_id_for_proc(const struct bx_killall_options* options,
                                          const struct bx_proc_info* info,
                                          pid_t* signal_id_out,
                                          bool* vanished_out) {
    if (options->process_group) {
        return bx_proc_read_ns_pgid(info->pid, info->pgrp, signal_id_out, vanished_out);
    }
    return bx_proc_read_ns_pid(info->pid, signal_id_out, vanished_out);
}

int bx_killall_main(int argc, char** argv) {
    struct bx_diag_ctx diag = {
        .progname = bx_psmisc_progname((argc > 0) ? argv[0] : NULL, "killall"),
        .exit_status = 0,
    };
    struct bx_killall_options options;
    struct bx_proc_list procs = {0};
    regex_t* regexes = NULL;
    pid_t* signaled_ids = NULL;
    size_t signaled_len = 0u;
    double uptime_seconds = 0.0;
    long ticks_per_second;
    pid_t self_host_pid;
    int handled;
    int rc = 1;
    int i;

    handled = bx_psmisc_maybe_handle_help_or_version(argc, argv, "killall", "-h", bx_killall_print_help);
    if (handled >= 0) {
        return handled;
    }
    if (!bx_killall_parse_options(&options, argc, argv, &diag)) {
        if (diag.exit_status != 0) {
            bx_cli_print_try_help(diag.progname);
        }
        bx_killall_options_cleanup(&options);
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }
    if (options.list_signals) {
        list_signals();
        bx_killall_options_cleanup(&options);
        return 0;
    }

    if (options.regexp) {
        regexes = xmalloc((size_t)(argc - options.first_name_index) * sizeof(*regexes));
        for (i = options.first_name_index; i < argc; i++) {
            if (regcomp(&regexes[i - options.first_name_index], argv[i], REG_EXTENDED | REG_NOSUB | (options.ignore_case ? REG_ICASE : 0)) != 0) {
                bx_diag(&diag, "bad regular expression: %s", argv[i]);
                bx_killall_options_cleanup(&options);
                free(regexes);
                return diag.exit_status != 0 ? diag.exit_status : 1;
            }
        }
    }

    if ((options.older_than_set || options.younger_than_set) && !bx_proc_uptime_seconds(&uptime_seconds)) {
        bx_diag(&diag, "failed to determine system uptime: %s", strerror(errno));
        bx_killall_options_cleanup(&options);
        free(regexes);
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }
    ticks_per_second = bx_proc_clock_ticks_per_second();
    self_host_pid = bx_proc_self_host_pid();

    if (!bx_proc_list_read(&procs, BX_PROC_READ_CMDLINE | BX_PROC_READ_EXE)) {
        bx_diag(&diag, "failed to read /proc: %s", strerror(errno));
        bx_killall_options_cleanup(&options);
        free(regexes);
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    for (i = 0; i < (int)procs.len; i++) {
        const struct bx_proc_info* info = &procs.items[i];
        pid_t display_id;
        pid_t signal_id;
        int j;
        bool matched = false;
        bool vanished = false;
        char cmd0_storage[BX_KILLALL_CMD_STORAGE];
        const char* display_name;

        if (info->pid == self_host_pid) {
            continue;
        }
        if (options.user_set && info->uid != options.user) {
            continue;
        }
        if (options.ns_set && !bx_killall_same_namespaces(info->pid, options.ns_pid)) {
            continue;
        }
        if (!bx_killall_context_matches(&options, info->pid)) {
            continue;
        }
        if (!bx_killall_age_matches(&options, info, uptime_seconds, ticks_per_second)) {
            continue;
        }
        for (j = options.first_name_index; j < argc; j++) {
            regex_t* regex = options.regexp ? &regexes[j - options.first_name_index] : NULL;
            if (bx_killall_name_matches(&options, info, argv[j], regex)) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            continue;
        }

        display_id = options.process_group ? info->pgrp : info->pid;
        if (!bx_killall_signal_id_for_proc(&options, info, &signal_id, &vanished)) {
            if (!options.quiet && !vanished) {
                fprintf(stderr,
                        "%s(%s%ld): %s\n",
                        info->comm != NULL ? info->comm : "?",
                        options.process_group ? "pgid " : "",
                        (long)display_id,
                        strerror(errno));
            }
            continue;
        }
        if (signal_id <= 0 || bx_killall_id_seen(signaled_ids, signaled_len, signal_id)) {
            continue;
        }

        display_name = bx_killall_cmd0(info, cmd0_storage, sizeof(cmd0_storage));
        if (options.interactive) {
            char prompt[512];
            snprintf(prompt,
                     sizeof(prompt),
                     "Signal %s(%s%ld)? ",
                     display_name != NULL ? display_name : "?",
                     options.process_group ? "pgid " : "",
                     (long)display_id);
            if (!bx_prompt_confirm(prompt)) {
                continue;
            }
        }

        if ((options.process_group ? kill(-signal_id, options.signal_number) : kill(signal_id, options.signal_number)) != 0) {
            if (!options.quiet) {
                fprintf(stderr,
                        "%s(%s%ld): %s\n",
                        display_name != NULL ? display_name : "?",
                        options.process_group ? "pgid " : "",
                        (long)display_id,
                        strerror(errno));
            }
            continue;
        }

        signaled_ids = xrealloc(signaled_ids, (signaled_len + 1u) * sizeof(*signaled_ids));
        signaled_ids[signaled_len++] = signal_id;
        rc = 0;
        if (options.verbose) {
            printf("Killed %s(%s%ld) with signal %d\n",
                   display_name != NULL ? display_name : "?",
                   options.process_group ? "pgid " : "",
                   (long)display_id,
                   options.signal_number);
        }
    }

    if (options.wait && signaled_len > 0u) {
        bx_killall_wait_for_ids(signaled_ids, signaled_len, options.process_group);
    }
    if (rc != 0 && !options.quiet) {
        bx_diag(&diag, "no process found");
        rc = diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.regexp) {
        for (i = options.first_name_index; i < argc; i++) {
            regfree(&regexes[i - options.first_name_index]);
        }
    }
    free(regexes);
    free(signaled_ids);
    bx_proc_list_free(&procs);
    bx_killall_options_cleanup(&options);
    return rc;
}
