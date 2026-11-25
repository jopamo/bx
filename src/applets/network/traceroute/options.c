/*
    Copyright (c)  2006, 2007		Dmitry Butskoy
                                        <dmitry@butskoy.name>
    License:  GPL v2 or any later

    See COPYING for the status of this software.
*/

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <netdb.h>

#include "lib/option_scan.h"
#include "lib/cli_common.h"
#include "lib/size_parse.h"
#include "traceroute.h"
#include "bx/version.h"

static void print_help(const struct bx_traceroute_ctx* ctx) {
    fprintf(stderr, "Usage:\n  %s", ctx->options.progname);
    fputs(" [ -46dFITnreAUDV ] [ --jsonl ] [ --quiet ] [ --bpf=mode ] [ -f first_ttl ] [ -g gate,... ] [ -i device ] [ --netns=path ] [ -m max_ttl ] [ -N squeries ] [ -p port ] [ -t tos ] [ -l flow_label ] [ -w MAX,HERE,NEAR ] [ --deadline=seconds ] [ --ts=MODE ] [ --auto-fallback ] [ -q nqueries ] [ --ecmp=num ] [ -s src_addr ] [ -z sendwait ] [ --fwmark=num ] host [ packetlen ]\n", stderr);
    fputs("Options:\n", stderr);
    fputs("  -4                          Use IPv4\n", stderr);
    fputs("  -6                          Use IPv6\n", stderr);
    fputs("  -d  --debug                 Enable socket level debugging\n", stderr);
    fputs("  --jsonl                     Use JSONL streaming output\n", stderr);
    fputs("  --quiet                     Do not print human-readable output\n", stderr);
    fputs("  --bpf=mode                  Enable eBPF correlation (auto|on|off)\n", stderr);
    fputs("  -F  --dont-fragment         Do not fragment packets\n", stderr);
    fputs("  -f first_ttl  --first=first_ttl\n", stderr);
    fputs("                              Start from the first_ttl hop (instead from 1)\n", stderr);
    fputs("  -g gate,...  --gateway=gate,...\n", stderr);
    fputs("                              Route packets through the specified gateway\n", stderr);
    fputs("                              (maximum 8 for IPv4 and 127 for IPv6)\n", stderr);
    fputs("  -I  --icmp                  Use ICMP ECHO for tracerouting\n", stderr);
    fputs("  -T  --tcp                   Use TCP SYN for tracerouting (default port is 80)\n", stderr);
    fputs("  -i device  --interface=device\n", stderr);
    fputs("                              Specify a network interface to operate with\n", stderr);
    fputs("  --netns=path                Switch to the network namespace specified by path\n", stderr);
    fputs("                              before starting\n", stderr);
    fputs("  -m max_ttl  --max-hops=max_ttl\n", stderr);
    fputs("                              Set the max number of hops (max TTL to be\n", stderr);
    fputs("                              reached). Default is 30\n", stderr);
    fputs("  -N squeries  --sim-queries=squeries\n", stderr);
    fputs("                              Set the number of probes to be tried\n", stderr);
    fputs("                              simultaneously (default is 16)\n", stderr);
    fputs("  -n                          Do not resolve IP addresses to their domain names\n", stderr);
    fputs("  -p port  --port=port        Set the destination port to use. It is either\n", stderr);
    fputs("                              initial udp port value for \"default\" method\n", stderr);
    fputs("                              (incremented by each probe, default is 33434), or\n", stderr);
    fputs("                              initial seq for \"icmp\" (incremented as well,\n", stderr);
    fputs("                              default from 1), or some constant destination\n", stderr);
    fputs("                              port for other methods (with default of 80 for\n", stderr);
    fputs("                              \"tcp\", 53 for \"udp\", etc.)\n", stderr);
    fputs("  -t tos  --tos=tos           Set the TOS (IPv4 type of service) or TC (IPv6\n", stderr);
    fputs("                              traffic class) value for outgoing packets\n", stderr);
    fputs("  -l flow_label  --flowlabel=flow_label\n", stderr);
    fputs("                              Use specified flow_label for IPv6 packets\n", stderr);
    fputs("  -w MAX,HERE,NEAR  --wait=MAX,HERE,NEAR\n", stderr);
    fputs("                              Wait for a probe no more than HERE (default 3)\n", stderr);
    fputs("                              times longer than a response from the same hop,\n", stderr);
    fputs("                              or no more than NEAR (default 10) times than some\n", stderr);
    fputs("                              next hop, or MAX (default 5.0) seconds (float\n", stderr);
    fputs("                              point values allowed too)\n", stderr);
    fputs("  --deadline=seconds          Set the overall deadline for the whole traceroute\n", stderr);
    fputs("                              in seconds (float point values allowed too). If\n", stderr);
    fputs("                              the deadline is reached, the traceroute stops\n", stderr);
    fputs("                              immediately\n", stderr);
    fputs("  --ts=MODE                   Set timestamping MODE (userspace, kernel-sw,\n", stderr);
    fputs("                              kernel-hw). Default is kernel-sw\n", stderr);
    fputs("  --auto-fallback             Automatically switch to TCP SYN probes if UDP is\n", stderr);
    fputs("                              filtered\n", stderr);
    fputs("  -q nqueries  --queries=nqueries\n", stderr);
    fputs("                              Set the number of probes per each hop. Default is\n", stderr);
    fputs("                              3\n", stderr);
    fputs("  --ecmp=num                  Run num distinct flow identities per TTL\n", stderr);
    fputs("  -r                          Bypass the normal routing and send directly to a\n", stderr);
    fputs("                              host on an attached network\n", stderr);
    fputs("  -s src_addr  --source=src_addr\n", stderr);
    fputs("                              Use source src_addr for outgoing packets\n", stderr);
    fputs("  -z sendwait  --sendwait=sendwait\n", stderr);
    fputs("                              Minimal time interval between probes (default 0).\n", stderr);
    fputs("                              If the value is more than 10, then it specifies a\n", stderr);
    fputs("                              number in milliseconds, else it is a number of\n", stderr);
    fputs("                              seconds (float point values allowed too)\n", stderr);
    fputs("  -e  --extensions            Show ICMP extensions (if present), including MPLS\n", stderr);
    fputs("  -A  --as-path-lookups       Perform AS path lookups in routing registries and\n", stderr);
    fputs("                              print results directly after the corresponding\n", stderr);
    fputs("                              addresses\n", stderr);
    fputs("  -M name  --module=name      Use specified builtin method for traceroute\n", stderr);
    fputs("                              operations. Most methods have\n", stderr);
    fputs("                              their shortcuts (`-I' means `-M icmp' etc.)\n", stderr);
    fputs("  -O OPTS,...  --options=OPTS,...\n", stderr);
    fputs("                              Use method-specific option OPTS for the\n", stderr);
    fputs("                              traceroute method. Several OPTS allowed,\n", stderr);
    fputs("                              separated by comma. If OPTS is \"help\", print info\n", stderr);
    fputs("                              about available options\n", stderr);
    fputs("  --sport=num                 Use source port num for outgoing packets. Implies\n", stderr);
    fputs("                              `-N 1'\n", stderr);
    fputs("  --fwmark=num                Set firewall mark for outgoing packets\n", stderr);
    fputs("  -U  --udp                   Use UDP to particular port for tracerouting\n", stderr);
    fputs("                              (instead of increasing the port per each probe),\n", stderr);
    fputs("                              default port is 53\n", stderr);
    fputs("  -UL                         Use UDPLITE for tracerouting (default dest port\n", stderr);
    fputs("                              is 53)\n", stderr);
    fputs("  -D  --dccp                  Use DCCP Request for tracerouting (default port\n", stderr);
    fputs("                              is 33434)\n", stderr);
    fputs("  -P prot  --protocol=prot    Use raw packet of protocol prot for tracerouting\n", stderr);
    fputs("  --mtu                       Discover MTU along the path being traced. Implies\n", stderr);
    fputs("                              `-F -N 1'\n", stderr);
    fputs("  --back                      Guess the number of hops in the backward path and\n", stderr);
    fputs("                              print if it differs\n", stderr);
    fputs("  -V  --version               Print version info and exit\n", stderr);
    fputs("  --help                      Read this help and exit\n", stderr);
    fputs("\n", stderr);
    fputs("Arguments:\n", stderr);
    fputs("+     host          The host to traceroute to\n", stderr);
    fputs("      packetlen     The full packet length (default is the length of an IP\n", stderr);
    fputs("                    header plus 40). Can be ignored or increased to a minimal\n", stderr);
    fputs("                    allowed value\n", stderr);

}

static void print_version(void) {
    fprintf(stderr, "Modern traceroute for Linux, version %s\n"
            "Copyright (c) 2016  Dmitry Butskoy,   License: GPL v2 or any later\n",
            bx_version);
}

bool bx_traceroute_parse_uint(const char* text, unsigned int* value) {
    uintmax_t decimal;
    if (text && text[0] != '0' && bx_size_parse_uint(text, &decimal)) {
        *value = (unsigned int)decimal;
        return true;
    }
    if (!text || !*text)
        return false;
    char* end;
    unsigned long parsed = strtoul(text, &end, 0);
    if (end == text || *end)
        return false;
    *value = (unsigned int)parsed;
    return true;
}

static bool parse_double(const char* text, double* value) {
    char* end;
    *value = strtod(text, &end);
    return end != text && !*end;
}

static bool parse_port(const char* text, unsigned int* value) {
    char* end;
    *value = (unsigned int)strtoul(text, &end, 0);
    if (end != text)
        return true;
    struct servent* service = getservbyname(text, NULL);
    if (!service)
        return false;
    *value = ntohs(service->s_port);
    return true;
}

static char* next_list_word(char* word) {
    /* Preserve the list separators and 79-byte word limit. */
    size_t len = strcspn(word, " \t,");
    if (len >= 80)
        return NULL;
    char* next = word + len;
    if (*next) {
        *next++ = '\0';
        next += strspn(next, " \t,");
    }
    return next;
}

static bool append_method_option(struct bx_traceroute_ctx* ctx, const char* option) {
    if (ctx->options.method_option_count >= sizeof(ctx->options.method_options) / sizeof(*ctx->options.method_options)) {
        fprintf(stderr, "Too many method options\n");
        return false;
    }
    char* copy = strdup(option);
    if (!copy)
        bx_traceroute_error(ctx, "strdup");
    ctx->options.method_options[ctx->options.method_option_count++] = copy;
    return true;
}

static bool add_gateways(struct bx_traceroute_ctx* ctx, const char* text) {
    char* copy = strdup(text);
    if (!copy)
        bx_traceroute_error(ctx, "strdup");
    char* token = copy;
    while (*token) {
        char* next = next_list_word(token);
        if (!next) {
            free(copy);
            return false;
        }
        if (ctx->num_gateways >= MAX_GATEWAYS_6) {
            free(copy);
            fprintf(stderr, "Too many gateways specified.");
            return false;
        }
        char** gateways = realloc(ctx->gateways, (ctx->num_gateways + 1) * sizeof(*gateways));
        if (!gateways) {
            free(copy);
            bx_traceroute_error(ctx, "malloc");
        }
        ctx->gateways = gateways;
        ctx->gateways[ctx->num_gateways] = strdup(token);
        if (!ctx->gateways[ctx->num_gateways]) {
            free(copy);
            bx_traceroute_error(ctx, "strdup");
        }
        ctx->num_gateways++;
        token = next;
    }
    free(copy);
    return true;
}

static bool add_method_options(struct bx_traceroute_ctx* ctx, const char* text) {
    char* copy = strdup(text);
    if (!copy)
        bx_traceroute_error(ctx, "strdup");
    char* token = copy;
    while (*token) {
        char* next = next_list_word(token);
        if (!next) {
            free(copy);
            return false;
        }
        if (!strcmp(token, "help")) {
            const struct bx_traceroute_method* method = bx_traceroute_method_find(ctx->options.method_name);
            if (method && method->print_options)
                method->print_options(stderr);
            else
                fprintf(stderr, "No options for method `%s'\n", ctx->options.method_name);
            free(copy);
            ctx->options.show_method_help = true;
            ctx->diag.exit_status = 0;
            return false;
        }
        if (!append_method_option(ctx, token)) {
            free(copy);
            return false;
        }
        token = next;
    }
    free(copy);
    return true;
}

static bool parse_wait(struct bx_traceroute_ctx* ctx, const char* text) {
    char* end;
    ctx->options.here_factor = ctx->options.near_factor = 0;
    ctx->options.wait_secs = strtod(text, &end);
    if (end == text)
        return false;
    if (!*end)
        return true;
    text = end + 1;
    ctx->options.here_factor = strtod(text, &end);
    if (end == text)
        return false;
    if (!*end)
        return true;
    text = end + 1;
    ctx->options.near_factor = strtod(text, &end);
    return end != text && !*end;
}

static bool parse_ts(struct bx_traceroute_ctx* ctx, const char* text) {
    if (!strcmp(text, "userspace"))
        ctx->options.ts_mode = TS_USERSPACE;
    else if (!strcmp(text, "kernel-sw") || !strcmp(text, "sw"))
        ctx->options.ts_mode = TS_KERNEL_SW;
    else if (!strcmp(text, "kernel-hw") || !strcmp(text, "hw"))
        ctx->options.ts_mode = TS_KERNEL_HW;
    else
        return false;
    return true;
}

static bool parse_bpf(struct bx_traceroute_ctx* ctx, const char* text) {
    if (!strcasecmp(text, "auto"))
        ctx->options.bpf_mode = 0;
    else if (!strcasecmp(text, "on"))
        ctx->options.bpf_mode = 1;
    else if (!strcasecmp(text, "off"))
        ctx->options.bpf_mode = 2;
    else
        return false;
    return true;
}

bool bx_traceroute_parse_method_options(struct bx_traceroute_ctx* ctx) {
    if (!ctx->method->parse_option)
        return true;
    for (unsigned int i = 0; i < ctx->options.method_option_count; i++) {
        const char* option = ctx->options.method_options[i];
        int result = ctx->method->parse_option(ctx, option);
        if (result == BX_TRACEROUTE_OPTION_OK)
            continue;
        const char* eq = strchr(option, '=');
        size_t len = eq ? (size_t)(eq - option) : strlen(option);
        if (result == BX_TRACEROUTE_OPTION_BAD_ARGUMENT)
            bx_diag(&ctx->diag, "Cannot handle `%.*s' keyword with arg `%s' (argc %u)", (int)len, option, eq ? eq + 1 : "", i + 1);
        else if (result == BX_TRACEROUTE_OPTION_NEEDS_ARGUMENT)
            bx_diag(&ctx->diag, "Keyword `%s' (argc %u) requires an argument: `%s=%s'", option, i + 1, option,
                    ctx->method->id == BX_TRACEROUTE_METHOD_RAW ? "PROT" : "NUM");
        else if (result == BX_TRACEROUTE_OPTION_EXCLUSIVE)
            bx_diag(&ctx->diag, "Keyword `%s' (argc %u): Only one of:\n    raw | dgram\nmay be specified.", option, i + 1);
        else
            bx_diag(&ctx->diag, "`%s' (argc %u): arguments are not allowed", option, i + 1);
        ctx->diag.exit_status = 2;
        return false;
    }
    return true;
}

static void select_method(struct bx_traceroute_ctx* ctx, const char* name) {
    ctx->options.method_name = name;
    const struct bx_traceroute_method* method = bx_traceroute_method_find(name);
    ctx->options.method = method ? method->id : BX_TRACEROUTE_METHOD_UNKNOWN;
}

static const char* option_argument_name(int option) {
    switch (option) {
        case 'f': return "first_ttl";
        case 'm': return "max_ttl";
        case 'N': return "squeries";
        case 't': return "tos";
        case 'l': return "flow_label";
        case 'q': return "nqueries";
        case 275: return "num";
        case 276: return "num";
        case 'i': return "device";
        case 278: return "path";
        case 'M': return "name";
        case 'p': return "port";
        case 286: return "num";
        case 'g': return "gate";
        case 's': return "src_addr";
        case 'O': return "OPTS";
        case 'P': return "prot";
        case 'w': return "MAX,HERE,NEAR";
        case 'z': return "sendwait";
        case 293: return "seconds";
        case 294: return "MODE";
        case 295: return "mode";
        default: return NULL;
    }
}

bool bx_traceroute_parse_options(struct bx_traceroute_ctx* ctx, int argc, char** argv) {
    ctx->options.progname = bx_cli_progname(argc ? argv[0] : NULL, "traceroute");
    ctx->diag.progname = ctx->options.progname;
    size_t name_len = strlen(ctx->options.progname);
    if (name_len && ctx->options.progname[name_len - 1] == '6')
        ctx->options.address_family = AF_INET6;
    else if (name_len && ctx->options.progname[name_len - 1] == '4')
        ctx->options.address_family = AF_INET;
    if (!strncmp(ctx->options.progname, "tcp", 3))
        select_method(ctx, "tcp");
    else if (!strncmp(ctx->options.progname, "tracert", 7))
        select_method(ctx, "icmp");
    if (argc <= 1) {
        print_help(ctx);
        return false;
    }
    static const struct bx_option_spec specs[] = {
        {'d', 'd', "debug", BX_OPTION_NO_VALUE},
        {'F', 'F', "dont-fragment", BX_OPTION_NO_VALUE},
        {'e', 'e', "extensions", BX_OPTION_NO_VALUE},
        {'A', 'A', "as-path-lookups", BX_OPTION_NO_VALUE},
        {264, 0, "jsonl", BX_OPTION_NO_VALUE},
        {265, 0, "quiet", BX_OPTION_NO_VALUE},
        {266, 0, "auto-fallback", BX_OPTION_NO_VALUE},
        {267, 0, "mtu", BX_OPTION_NO_VALUE},
        {268, 0, "back", BX_OPTION_NO_VALUE},
        {'f', 'f', "first", BX_OPTION_REQUIRED_VALUE},
        {'m', 'm', "max-hops", BX_OPTION_REQUIRED_VALUE},
        {'N', 'N', "sim-queries", BX_OPTION_REQUIRED_VALUE},
        {'t', 't', "tos", BX_OPTION_REQUIRED_VALUE},
        {'l', 'l', "flowlabel", BX_OPTION_REQUIRED_VALUE},
        {'q', 'q', "queries", BX_OPTION_REQUIRED_VALUE},
        {275, 0, "ecmp", BX_OPTION_REQUIRED_VALUE},
        {276, 0, "fwmark", BX_OPTION_REQUIRED_VALUE},
        {'i', 'i', "interface", BX_OPTION_REQUIRED_VALUE},
        {278, 0, "netns", BX_OPTION_REQUIRED_VALUE},
        {'M', 'M', "module", BX_OPTION_REQUIRED_VALUE},
        {'I', 'I', "icmp", BX_OPTION_NO_VALUE},
        {'T', 'T', "tcp", BX_OPTION_NO_VALUE},
        {'U', 'U', "udp", BX_OPTION_NO_VALUE},
        {283, 0, "UL", BX_OPTION_NO_VALUE},
        {'D', 'D', "dccp", BX_OPTION_NO_VALUE},
        {'p', 'p', "port", BX_OPTION_REQUIRED_VALUE},
        {286, 0, "sport", BX_OPTION_REQUIRED_VALUE},
        {'g', 'g', "gateway", BX_OPTION_REQUIRED_VALUE},
        {'s', 's', "source", BX_OPTION_REQUIRED_VALUE},
        {'O', 'O', "options", BX_OPTION_REQUIRED_VALUE},
        {'P', 'P', "protocol", BX_OPTION_REQUIRED_VALUE},
        {'w', 'w', "wait", BX_OPTION_REQUIRED_VALUE},
        {'z', 'z', "sendwait", BX_OPTION_REQUIRED_VALUE},
        {293, 0, "deadline", BX_OPTION_REQUIRED_VALUE},
        {294, 0, "ts", BX_OPTION_REQUIRED_VALUE},
        {295, 0, "bpf", BX_OPTION_REQUIRED_VALUE},
        {'V', 'V', "version", BX_OPTION_NO_VALUE},
        {297, 0, "help", BX_OPTION_NO_VALUE},
        {'4', '4', NULL, BX_OPTION_NO_VALUE},
        {'6', '6', NULL, BX_OPTION_NO_VALUE},
        {'n', 'n', NULL, BX_OPTION_NO_VALUE},
        {'r', 'r', NULL, BX_OPTION_NO_VALUE},
        {'L', 'L', NULL, BX_OPTION_NO_VALUE},
    };
    struct bx_option_result operands[2];
    size_t operand_count = 0;
    struct bx_option_result extra = {0};
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, 1, (size_t)argc, BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE, specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_OPERAND) {
            if (operand_count < sizeof(operands) / sizeof(operands[0]))
                operands[operand_count++] = option;
            else if (!extra.token)
                extra = option;
            continue;
        }
        const char* spelling = option.token;
        int index = (int)option.index;
        int c = option.spec ? option.spec->id : 0;
        const char* value = option.value;
        const char* argument_name = option_argument_name(c);
        if (option.event != BX_OPTION_MATCH) {
            const char* equals = strchr(spelling, '=');
            if (option.event == BX_OPTION_MISSING_VALUE)
                bx_diag(&ctx->diag, "Option `%s' (argc %d) requires an argument: `%s%s%s'", spelling, index, spelling, option.short_name ? " " : "=", argument_name);
            else if (option.short_name)
                bx_diag(&ctx->diag, "Bad option `-%c' (argc %d)", option.short_name, index);
            else if (equals)
                bx_diag(&ctx->diag, "Bad option `%s' (with arg `%s') (argc %d)", spelling, equals + 1, index);
            else
                bx_diag(&ctx->diag, "Bad option `%s' (argc %d)", spelling, index);
            ctx->diag.exit_status = 2;
            return false;
        }
        bool detached = value && cursor.index == option.index + 2;
        /* Allow -nf 1, but recognize an attached value only in standalone -f1. */
        if (option.short_name && value && !detached && spelling[1] != c) {
            bx_diag(&ctx->diag, "Option `-%c' (argc %d) requires an argument: `-%c %s'", c, index, c, argument_name);
            ctx->diag.exit_status = 2;
            return false;
        }
        bool ok = true;
        switch (c) {
            case '4': {
                ctx->options.address_family = AF_INET;
                break;
            }
            case '6': {
                ctx->options.address_family = AF_INET6;
                break;
            }
            case 'd': {
                ctx->options.debug = 1;
                break;
            }
            case 'F': {
                ctx->options.dont_fragment = 1;
                break;
            }
            case 'n': {
                ctx->options.noresolve = 1;
                break;
            }
            case 'r': {
                ctx->options.noroute = 1;
                break;
            }
            case 'e': {
                ctx->options.extension = 1;
                break;
            }
            case 'A': {
                ctx->options.as_lookups = 1;
                break;
            }
            case 264: {
                ctx->options.jsonl = 1;
                break;
            }
            case 265: {
                ctx->options.quiet = 1;
                break;
            }
            case 266: {
                ctx->options.auto_fallback = 1;
                break;
            }
            case 267: {
                ctx->options.mtu_discovery = 1;
                break;
            }
            case 268: {
                ctx->options.backward = 1;
                break;
            }
            case 'f': {
                ok = bx_traceroute_parse_uint(value, &ctx->options.first_hop);
                break;
            }
            case 'm': {
                ok = bx_traceroute_parse_uint(value, &ctx->options.max_hops);
                break;
            }
            case 'N': {
                ok = bx_traceroute_parse_uint(value, &ctx->options.simultaneous_probes);
                break;
            }
            case 't': {
                ok = bx_traceroute_parse_uint(value, &ctx->options.tos);
                break;
            }
            case 'l': {
                ok = bx_traceroute_parse_uint(value, &ctx->options.flow_label);
                break;
            }
            case 'q': {
                ok = bx_traceroute_parse_uint(value, &ctx->options.probes_per_hop);
                break;
            }
            case 275: {
                ok = bx_traceroute_parse_uint(value, &ctx->options.ecmp);
                break;
            }
            case 276: {
                ok = bx_traceroute_parse_uint(value, &ctx->options.fwmark);
                break;
            }
            case 'i': {
                ctx->options.interface = value;
                break;
            }
            case 278: {
                ctx->options.netns = value;
                break;
            }
            case 'M': {
                select_method(ctx, value);
                break;
            }
            case 'I': {
                select_method(ctx, "icmp");
                break;
            }
            case 'T': {
                select_method(ctx, "tcp");
                break;
            }
            case 'U': {
                select_method(ctx, "udp");
                break;
            }
            case 'L': {
                if (strcmp(spelling, "-UL")) {
                    bx_diag(&ctx->diag, "Bad option `-L' (argc %d)", index);
                    ctx->diag.exit_status = 2;
                    return false;
                }
                select_method(ctx, "udplite");
                break;
            }
            case 283: {
                select_method(ctx, "udplite");
                break;
            }
            case 'D': {
                select_method(ctx, "dccp");
                break;
            }
            case 'p': {
                ok = parse_port(value, &ctx->options.destination_port);
                break;
            }
            case 286: {
                ok = parse_port(value, &ctx->options.source_port);
                break;
            }
            case 'g': {
                ok = add_gateways(ctx, value);
                break;
            }
            case 's': {
                ok = bx_traceroute_getaddr(ctx, value, &ctx->source) == 0;
                break;
            }
            case 'O': {
                ok = add_method_options(ctx, value);
                break;
            }
            case 'P': {
                select_method(ctx, "raw");
                char raw_option[1024];
                snprintf(raw_option, sizeof(raw_option), "protocol=%s", value);
                ok = append_method_option(ctx, raw_option);
                break;
            }
            case 'w': {
                ok = parse_wait(ctx, value);
                break;
            }
            case 'z': {
                ok = parse_double(value, &ctx->options.send_secs);
                break;
            }
            case 293: {
                ok = parse_double(value, &ctx->options.deadline);
                break;
            }
            case 294: {
                ok = parse_ts(ctx, value);
                break;
            }
            case 295: {
                ok = parse_bpf(ctx, value);
                break;
            }
            case 'V': {
                print_version();
                return false;
            }
            case 297: {
                print_help(ctx);
                return false;
            }
        }
        if (!ok) {
            if (ctx->options.show_method_help)
                return false;
            char option_name[128];
            int argument_index = index + detached;
            size_t len = spelling[0] == '-' && spelling[1] != '-'
                             ? 2 : strcspn(spelling, "=");
            if (len >= sizeof(option_name))
                len = sizeof(option_name) - 1;
            memcpy(option_name, spelling, len);
            if (spelling[0] == '-' && spelling[1] != '-')
                option_name[1] = (char)c;
            option_name[len] = '\0';
            bx_diag(&ctx->diag, "Cannot handle `%s' option with arg `%s' (argc %d)", option_name, value ? value : argument_name, argument_index);
            ctx->diag.exit_status = 2;
            return false;
        }
    }
    if (operand_count == 0) {
        bx_diag(&ctx->diag, "Specify \"host\" missing argument.");
        ctx->diag.exit_status = 2;
        return false;
    }
    if (extra.token) {
        bx_diag(&ctx->diag, "Extra arg `%s' (position 3, argc %zu)", extra.token, extra.index);
        ctx->diag.exit_status = 2;
        return false;
    }
    if (operand_count == 2) {
        const char* packetlen = operands[1].token;
        char* end;
        errno = 0;
        long length = strtol(packetlen, &end, 0);
        if (errno == ERANGE || end == packetlen || *end || length < INT_MIN || length > INT_MAX) {
            bx_diag(&ctx->diag, "Cannot handle \"packetlen\" cmdline arg `%s' on position 2 (argc %zu)", packetlen, operands[1].index);
            ctx->diag.exit_status = 2;
            return false;
        }
        ctx->options.packet_len = (int)length;
    }
    const char* host = operands[0].token;
    if (bx_traceroute_getaddr(ctx, host, &ctx->destination) < 0) {
        bx_diag(&ctx->diag, "Cannot handle \"host\" cmdline arg `%s' on position 1 (argc %zu)", host, operands[0].index);
        ctx->diag.exit_status = 2;
        return false;
    }
    ctx->options.dst_name = host;
    if (!ctx->options.address_family)
        ctx->options.address_family = ctx->destination.sa.sa_family;
    return true;
}
