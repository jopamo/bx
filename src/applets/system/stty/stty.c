#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/fd_ops.h"
#include "lib/size_parse.h"
#include "lib/tty_speed.h"

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

enum bx_stty_special_mode {
    BX_STTY_SPECIAL_NONE = 0,
    BX_STTY_SPECIAL_HELP,
    BX_STTY_SPECIAL_VERSION,
};

enum bx_stty_mode {
    BX_STTY_MODE_DEFAULT = 0,
    BX_STTY_MODE_ALL,
    BX_STTY_MODE_SAVE,
};

enum bx_stty_field {
    BX_STTY_FIELD_IFLAG = 0,
    BX_STTY_FIELD_OFLAG,
    BX_STTY_FIELD_CFLAG,
    BX_STTY_FIELD_LFLAG,
};

enum bx_stty_composite {
    BX_STTY_COMPOSITE_RAW = 0,
    BX_STTY_COMPOSITE_COOKED,
    BX_STTY_COMPOSITE_CBREAK,
    BX_STTY_COMPOSITE_UNCBREAK,
    BX_STTY_COMPOSITE_SANE,
    BX_STTY_COMPOSITE_EK,
    BX_STTY_COMPOSITE_NL,
    BX_STTY_COMPOSITE_UNNL,
    BX_STTY_COMPOSITE_PASS8,
    BX_STTY_COMPOSITE_UNPASS8,
    BX_STTY_COMPOSITE_LITOUT,
    BX_STTY_COMPOSITE_UNLITOUT,
    BX_STTY_COMPOSITE_EVENP,
    BX_STTY_COMPOSITE_UNEVENP,
    BX_STTY_COMPOSITE_ODDP,
    BX_STTY_COMPOSITE_UNODDP,
    BX_STTY_COMPOSITE_CRT,
    BX_STTY_COMPOSITE_DEC,
    BX_STTY_COMPOSITE_DECCTLQ,
    BX_STTY_COMPOSITE_UNDECCTLQ,
    BX_STTY_COMPOSITE_LCASE,
    BX_STTY_COMPOSITE_UNLCASE,
};

enum bx_stty_op_kind {
    BX_STTY_OP_FLAG = 0,
    BX_STTY_OP_FLAG_MASK,
    BX_STTY_OP_CC,
    BX_STTY_OP_ISPEED,
    BX_STTY_OP_OSPEED,
    BX_STTY_OP_SPEED_BOTH,
    BX_STTY_OP_ROWS,
    BX_STTY_OP_COLS,
    BX_STTY_OP_LINE_DISC,
    BX_STTY_OP_PRINT_SIZE,
    BX_STTY_OP_PRINT_SPEED,
    BX_STTY_OP_COMPOSITE,
    BX_STTY_OP_GSTATE,
};

struct bx_stty_cc_spec {
    const char* name;
    unsigned index;
    bool numeric;
};

struct bx_stty_flag_spec {
    const char* name;
    enum bx_stty_field field;
    tcflag_t bit;
};

struct bx_stty_g_state {
    tcflag_t iflag;
    tcflag_t oflag;
    tcflag_t cflag;
    tcflag_t lflag;
    cc_t cc[NCCS];
    bool has_ispeed;
    speed_t ispeed;
    bool has_ospeed;
    speed_t ospeed;
};

struct bx_stty_op {
    enum bx_stty_op_kind kind;
    union {
        struct {
            enum bx_stty_field field;
            tcflag_t bit;
            bool enable;
        } flag;
        struct {
            enum bx_stty_field field;
            tcflag_t mask;
            tcflag_t value;
        } flag_mask;
        struct {
            unsigned cc_index;
            cc_t value;
            bool disable;
        } cc;
        struct {
            speed_t speed;
        } speed;
        struct {
            unsigned value;
        } num;
        struct {
            int value;
        } line;
        struct {
            enum bx_stty_composite id;
        } composite;
        struct bx_stty_g_state gstate;
    } u;
};

struct bx_stty_plan {
    enum bx_stty_special_mode special_mode;
    enum bx_stty_mode mode;
    bool mode_conflict;
    bool drain;
    const char* device_path;
    struct bx_stty_op* ops;
    size_t op_count;
    size_t op_cap;
};

struct bx_stty_setting_tokens {
    char** values;
    size_t count;
    size_t cap;
};

struct bx_stty_state {
    struct termios tio;
    bool have_tio;

    struct winsize ws;
    bool have_ws;

    int ldisc;
    bool have_ldisc;

    bool need_tio_commit;
    bool need_ws_commit;
    bool need_ldisc_commit;
};

struct bx_stty_requirements {
    bool need_tio;
    bool need_ws;
    bool need_ldisc;
    bool want_default_print;
    bool want_all_print;
    bool want_ldisc_print;
};

static const struct bx_stty_cc_spec bx_stty_cc_table[] = {
#ifdef VDISCARD
    {"discard", VDISCARD, false},
#endif
#ifdef VEOF
    {"eof", VEOF, false},
#endif
#ifdef VEOL
    {"eol", VEOL, false},
#endif
#ifdef VEOL2
    {"eol2", VEOL2, false},
#endif
#ifdef VERASE
    {"erase", VERASE, false},
#endif
#ifdef VINTR
    {"intr", VINTR, false},
#endif
#ifdef VKILL
    {"kill", VKILL, false},
#endif
#ifdef VLNEXT
    {"lnext", VLNEXT, false},
#endif
#ifdef VMIN
    {"min", VMIN, true},
#endif
#ifdef VQUIT
    {"quit", VQUIT, false},
#endif
#ifdef VREPRINT
    {"rprnt", VREPRINT, false},
#endif
#ifdef VSTART
    {"start", VSTART, false},
#endif
#ifdef VSTOP
    {"stop", VSTOP, false},
#endif
#ifdef VSUSP
    {"susp", VSUSP, false},
#endif
#ifdef VSWTC
    {"swtch", VSWTC, false},
#endif
#ifdef VTIME
    {"time", VTIME, true},
#endif
#ifdef VWERASE
    {"werase", VWERASE, false},
#endif
};

static const struct bx_stty_flag_spec bx_stty_flag_table[] = {
#ifdef IGNBRK
    {"ignbrk", BX_STTY_FIELD_IFLAG, IGNBRK},
#endif
#ifdef BRKINT
    {"brkint", BX_STTY_FIELD_IFLAG, BRKINT},
#endif
#ifdef IGNPAR
    {"ignpar", BX_STTY_FIELD_IFLAG, IGNPAR},
#endif
#ifdef PARMRK
    {"parmrk", BX_STTY_FIELD_IFLAG, PARMRK},
#endif
#ifdef INPCK
    {"inpck", BX_STTY_FIELD_IFLAG, INPCK},
#endif
#ifdef ISTRIP
    {"istrip", BX_STTY_FIELD_IFLAG, ISTRIP},
#endif
#ifdef INLCR
    {"inlcr", BX_STTY_FIELD_IFLAG, INLCR},
#endif
#ifdef IGNCR
    {"igncr", BX_STTY_FIELD_IFLAG, IGNCR},
#endif
#ifdef ICRNL
    {"icrnl", BX_STTY_FIELD_IFLAG, ICRNL},
#endif
#ifdef IUCLC
    {"iuclc", BX_STTY_FIELD_IFLAG, IUCLC},
#endif
#ifdef IXON
    {"ixon", BX_STTY_FIELD_IFLAG, IXON},
#endif
#ifdef IXANY
    {"ixany", BX_STTY_FIELD_IFLAG, IXANY},
#endif
#ifdef IXOFF
    {"ixoff", BX_STTY_FIELD_IFLAG, IXOFF},
#endif
#ifdef IMAXBEL
    {"imaxbel", BX_STTY_FIELD_IFLAG, IMAXBEL},
#endif
#ifdef IUTF8
    {"iutf8", BX_STTY_FIELD_IFLAG, IUTF8},
#endif
#ifdef OPOST
    {"opost", BX_STTY_FIELD_OFLAG, OPOST},
#endif
#ifdef OLCUC
    {"olcuc", BX_STTY_FIELD_OFLAG, OLCUC},
#endif
#ifdef ONLCR
    {"onlcr", BX_STTY_FIELD_OFLAG, ONLCR},
#endif
#ifdef OCRNL
    {"ocrnl", BX_STTY_FIELD_OFLAG, OCRNL},
#endif
#ifdef ONOCR
    {"onocr", BX_STTY_FIELD_OFLAG, ONOCR},
#endif
#ifdef ONLRET
    {"onlret", BX_STTY_FIELD_OFLAG, ONLRET},
#endif
#ifdef OFILL
    {"ofill", BX_STTY_FIELD_OFLAG, OFILL},
#endif
#ifdef OFDEL
    {"ofdel", BX_STTY_FIELD_OFLAG, OFDEL},
#endif
#ifdef CLOCAL
    {"clocal", BX_STTY_FIELD_CFLAG, CLOCAL},
#endif
#ifdef CREAD
    {"cread", BX_STTY_FIELD_CFLAG, CREAD},
#endif
#ifdef CSTOPB
    {"cstopb", BX_STTY_FIELD_CFLAG, CSTOPB},
#endif
#ifdef HUPCL
    {"hupcl", BX_STTY_FIELD_CFLAG, HUPCL},
#endif
#ifdef PARENB
    {"parenb", BX_STTY_FIELD_CFLAG, PARENB},
#endif
#ifdef PARODD
    {"parodd", BX_STTY_FIELD_CFLAG, PARODD},
#endif
#ifdef CMSPAR
    {"cmspar", BX_STTY_FIELD_CFLAG, CMSPAR},
#endif
#ifdef CRTSCTS
    {"crtscts", BX_STTY_FIELD_CFLAG, CRTSCTS},
#endif
#ifdef ISIG
    {"isig", BX_STTY_FIELD_LFLAG, ISIG},
#endif
#ifdef ICANON
    {"icanon", BX_STTY_FIELD_LFLAG, ICANON},
#endif
#ifdef IEXTEN
    {"iexten", BX_STTY_FIELD_LFLAG, IEXTEN},
#endif
#ifdef ECHO
    {"echo", BX_STTY_FIELD_LFLAG, ECHO},
#endif
#ifdef ECHOE
    {"echoe", BX_STTY_FIELD_LFLAG, ECHOE},
#endif
#ifdef ECHOK
    {"echok", BX_STTY_FIELD_LFLAG, ECHOK},
#endif
#ifdef ECHONL
    {"echonl", BX_STTY_FIELD_LFLAG, ECHONL},
#endif
#ifdef NOFLSH
    {"noflsh", BX_STTY_FIELD_LFLAG, NOFLSH},
#endif
#ifdef TOSTOP
    {"tostop", BX_STTY_FIELD_LFLAG, TOSTOP},
#endif
#ifdef ECHOCTL
    {"echoctl", BX_STTY_FIELD_LFLAG, ECHOCTL},
#endif
#ifdef ECHOPRT
    {"echoprt", BX_STTY_FIELD_LFLAG, ECHOPRT},
#endif
#ifdef ECHOKE
    {"echoke", BX_STTY_FIELD_LFLAG, ECHOKE},
#endif
#ifdef FLUSHO
    {"flusho", BX_STTY_FIELD_LFLAG, FLUSHO},
#endif
#ifdef PENDIN
    {"pendin", BX_STTY_FIELD_LFLAG, PENDIN},
#endif
#ifdef XCASE
    {"xcase", BX_STTY_FIELD_LFLAG, XCASE},
#endif
#ifdef EXTPROC
    {"extproc", BX_STTY_FIELD_LFLAG, EXTPROC},
#endif
};

static cc_t bx_stty_vdisable(void) {
#ifdef _POSIX_VDISABLE
    return (cc_t)_POSIX_VDISABLE;
#else
    return (cc_t)0;
#endif
}

static void bx_stty_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [-F DEVICE | --file=DEVICE] [SETTING]...\n", progname);
    fprintf(stream, "  or:  %s [-F DEVICE | --file=DEVICE] [-a|--all]\n", progname);
    fprintf(stream, "  or:  %s [-F DEVICE | --file=DEVICE] [-g|--save]\n", progname);
    fprintf(stream, "Print or change terminal characteristics.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -a, --all          print all current settings in human-readable form\n");
    fprintf(stream, "  -g, --save         print all current settings in stty-readable form\n");
    fprintf(stream, "  -F, --file=DEVICE  open and use DEVICE instead of standard input\n");
    fprintf(stream, "      --help         display this help and exit\n");
    fprintf(stream, "      --version      output version information and exit\n");
}

static void bx_stty_plan_init(struct bx_stty_plan* plan) {
    memset(plan, 0, sizeof(*plan));
    plan->mode = BX_STTY_MODE_DEFAULT;
    plan->drain = true;
}

static void bx_stty_plan_free(struct bx_stty_plan* plan) {
    free(plan->ops);
    plan->ops = NULL;
    plan->op_count = 0;
    plan->op_cap = 0;
}

static bool bx_stty_plan_append_op(struct bx_stty_plan* plan, const struct bx_stty_op* op) {
    if (plan->op_count == plan->op_cap) {
        size_t new_cap = (plan->op_cap == 0) ? 16 : plan->op_cap * 2;
        struct bx_stty_op* grown = xrealloc(plan->ops, new_cap * sizeof(*grown));
        plan->ops = grown;
        plan->op_cap = new_cap;
    }

    plan->ops[plan->op_count++] = *op;
    return true;
}

static struct bx_stty_op bx_stty_make_op(enum bx_stty_op_kind kind) {
    struct bx_stty_op op;
    memset(&op, 0, sizeof(op));
    op.kind = kind;
    return op;
}

static bool bx_stty_plan_append_simple_op(struct bx_stty_plan* plan, enum bx_stty_op_kind kind) {
    struct bx_stty_op op = bx_stty_make_op(kind);
    return bx_stty_plan_append_op(plan, &op);
}

static bool bx_stty_plan_append_numeric_op(struct bx_stty_plan* plan, enum bx_stty_op_kind kind, unsigned value) {
    struct bx_stty_op op = bx_stty_make_op(kind);
    op.u.num.value = value;
    return bx_stty_plan_append_op(plan, &op);
}

static bool bx_stty_plan_append_line_op(struct bx_stty_plan* plan, int value) {
    struct bx_stty_op op = bx_stty_make_op(BX_STTY_OP_LINE_DISC);
    op.u.line.value = value;
    return bx_stty_plan_append_op(plan, &op);
}

static bool bx_stty_plan_append_speed_op(struct bx_stty_plan* plan, enum bx_stty_op_kind kind, speed_t speed) {
    struct bx_stty_op op = bx_stty_make_op(kind);
    op.u.speed.speed = speed;
    return bx_stty_plan_append_op(plan, &op);
}

static bool bx_stty_plan_append_composite_op(struct bx_stty_plan* plan, enum bx_stty_composite id) {
    struct bx_stty_op op = bx_stty_make_op(BX_STTY_OP_COMPOSITE);
    op.u.composite.id = id;
    return bx_stty_plan_append_op(plan, &op);
}

static bool bx_stty_plan_append_flag_op(struct bx_stty_plan* plan, enum bx_stty_field field, tcflag_t bit, bool enable) {
    struct bx_stty_op op = bx_stty_make_op(BX_STTY_OP_FLAG);
    op.u.flag.field = field;
    op.u.flag.bit = bit;
    op.u.flag.enable = enable;
    return bx_stty_plan_append_op(plan, &op);
}

static bool bx_stty_plan_append_flag_mask_op(struct bx_stty_plan* plan, enum bx_stty_field field, tcflag_t mask, tcflag_t value) {
    struct bx_stty_op op = bx_stty_make_op(BX_STTY_OP_FLAG_MASK);
    op.u.flag_mask.field = field;
    op.u.flag_mask.mask = mask;
    op.u.flag_mask.value = value;
    return bx_stty_plan_append_op(plan, &op);
}

static void bx_stty_tokens_init(struct bx_stty_setting_tokens* tokens) {
    memset(tokens, 0, sizeof(*tokens));
}

static void bx_stty_tokens_free(struct bx_stty_setting_tokens* tokens) {
    if (tokens->values != NULL) {
        for (size_t i = 0; i < tokens->count; i++) {
            free(tokens->values[i]);
        }
    }
    free(tokens->values);
    tokens->values = NULL;
    tokens->count = 0;
    tokens->cap = 0;
}

static bool bx_stty_tokens_add(struct bx_stty_setting_tokens* tokens, const char* value) {
    if (tokens->count == tokens->cap) {
        size_t new_cap = (tokens->cap == 0) ? 16 : tokens->cap * 2;
        char** grown = xrealloc(tokens->values, new_cap * sizeof(*grown));
        tokens->values = grown;
        tokens->cap = new_cap;
    }

    tokens->values[tokens->count++] = xstrdup(value);
    return true;
}

static unsigned int bx_stty_speed_to_baud(speed_t speed) {
    return bx_tty_speed_to_baud(speed);
}

static bool bx_stty_parse_speed(const char* token, speed_t* speed_out) {
    return bx_tty_speed_parse(token, speed_out);
}

static int bx_stty_set_input_speed(struct termios* tio, speed_t speed) {
#ifdef B0
    if (speed == B0) {
        return cfsetispeed(tio, speed);
    }
#endif
    return cfsetspeed(tio, speed);
}

static int bx_stty_set_both_speeds(struct termios* tio, speed_t speed) {
    return cfsetspeed(tio, speed);
}

static bool bx_stty_parse_unsigned(const char* token, unsigned long max_value, unsigned* out) {
    if (token == NULL || token[0] == '\0' || token[0] == '+' || token[0] == '-') {
        return false;
    }

    const char* digits = token;
    while (isspace((unsigned char)*digits)) {
        digits++;
    }
    if (digits[0] == '-') {
        return false;
    }
    if (digits[0] == '+') {
        digits++;
    }

    uintmax_t value = 0;
    if (!bx_size_parse_uint(digits, &value) || value > max_value) {
        return false;
    }

    *out = (unsigned)value;
    return true;
}

static bool bx_stty_digit_for_base(unsigned char ch, unsigned base, unsigned* digit_out) {
    unsigned digit = 0;

    if (ch >= '0' && ch <= '9') {
        digit = (unsigned)(ch - '0');
    }
    else if (ch >= 'a' && ch <= 'f') {
        digit = 10u + (unsigned)(ch - 'a');
    }
    else if (ch >= 'A' && ch <= 'F') {
        digit = 10u + (unsigned)(ch - 'A');
    }
    else {
        return false;
    }

    if (digit >= base) {
        return false;
    }

    *digit_out = digit;
    return true;
}

static bool bx_stty_parse_based_uint(const char* token, unsigned base, uintmax_t max_value, uintmax_t* out) {
    if (token == NULL || token[0] == '\0' || token[0] == '+' || token[0] == '-' || out == NULL) {
        return false;
    }
    if (base < 2u || base > 16u) {
        return false;
    }

    const char* digits = token;
    if (base == 16u && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        digits += 2;
        if (digits[0] == '\0') {
            return false;
        }
    }

    uintmax_t value = 0;
    for (; *digits != '\0'; digits++) {
        unsigned digit = 0;
        if (!bx_stty_digit_for_base((unsigned char)*digits, base, &digit)) {
            return false;
        }
        if (value > (max_value - digit) / base) {
            return false;
        }
        value = (value * base) + digit;
    }

    *out = value;
    return true;
}

static bool bx_stty_parse_int_nonnegative(const char* token, int* out) {
    unsigned parsed = 0;
    if (!bx_stty_parse_unsigned(token, (unsigned long)INT_MAX, &parsed)) {
        return false;
    }

    *out = (int)parsed;
    return true;
}

static bool bx_stty_parse_cc_value(const char* s, cc_t* out, bool* disable) {
    if (s == NULL || s[0] == '\0') {
        return false;
    }

    *disable = false;

    if (strcmp(s, "undef") == 0 || strcmp(s, "^-") == 0) {
        *disable = true;
        *out = bx_stty_vdisable();
        return true;
    }

    if (s[0] == '^' && s[1] != '\0' && s[2] == '\0') {
        unsigned char c = (unsigned char)s[1];
        if (c == '?') {
            *out = (cc_t)0x7f;
            return true;
        }

        if (c == '-') {
            *disable = true;
            *out = bx_stty_vdisable();
            return true;
        }

        *out = (cc_t)(c & 0x1fU);
        return true;
    }

    if (s[0] == '0' || isdigit((unsigned char)s[0])) {
        int base = 10;
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            base = 16;
        }
        else if (s[0] == '0' && s[1] != '\0') {
            base = 8;
        }

        uintmax_t value = 0;
        if (bx_stty_parse_based_uint(s, (unsigned)base, UCHAR_MAX, &value)) {
            *out = (cc_t)value;
            return true;
        }
    }

    if (s[0] != '\0' && s[1] == '\0') {
        *out = (cc_t)(unsigned char)s[0];
        return true;
    }

    return false;
}

static const struct bx_stty_cc_spec* bx_stty_lookup_cc_spec(const char* name) {
    for (size_t i = 0; i < sizeof(bx_stty_cc_table) / sizeof(bx_stty_cc_table[0]); i++) {
        if (strcmp(name, bx_stty_cc_table[i].name) == 0) {
            return &bx_stty_cc_table[i];
        }
    }
    return NULL;
}

static const struct bx_stty_flag_spec* bx_stty_lookup_flag_spec(const char* name) {
    for (size_t i = 0; i < sizeof(bx_stty_flag_table) / sizeof(bx_stty_flag_table[0]); i++) {
        if (strcmp(name, bx_stty_flag_table[i].name) == 0) {
            return &bx_stty_flag_table[i];
        }
    }
    return NULL;
}

static const char* bx_stty_resolve_alias(const char* name) {
    if (strcmp(name, "hup") == 0) {
        return "hupcl";
    }
    if (strcmp(name, "tandem") == 0) {
        return "ixoff";
    }
    if (strcmp(name, "ctlecho") == 0) {
        return "echoctl";
    }
    if (strcmp(name, "prterase") == 0) {
        return "echoprt";
    }
    if (strcmp(name, "crtkill") == 0) {
        return "echoke";
    }
    if (strcmp(name, "crterase") == 0) {
        return "echoe";
    }

    return name;
}

static bool bx_stty_parse_save_state(const char* token, struct bx_stty_g_state* out) {
    if (token == NULL || strchr(token, ':') == NULL) {
        return false;
    }

    const size_t required_fields = (size_t)4 + (size_t)NCCS;
    const size_t max_fields = required_fields + (size_t)2;
    unsigned long values[4 + NCCS + 2];
    size_t count = 0;

    char* copy = xstrdup(token);
    char* saveptr = NULL;

    for (char* part = strtok_r(copy, ":", &saveptr); part != NULL; part = strtok_r(NULL, ":", &saveptr)) {
        if (part[0] == '\0' || count >= max_fields) {
            free(copy);
            return false;
        }

        uintmax_t value = 0;
        if (!bx_stty_parse_based_uint(part, 16u, ULONG_MAX, &value)) {
            free(copy);
            return false;
        }

        values[count++] = (unsigned long)value;
    }

    free(copy);

    if (count != required_fields && count != max_fields) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->iflag = (tcflag_t)values[0];
    out->oflag = (tcflag_t)values[1];
    out->cflag = (tcflag_t)values[2];
    out->lflag = (tcflag_t)values[3];

    for (size_t i = 0; i < (size_t)NCCS; i++) {
        if (values[4 + i] > UCHAR_MAX) {
            return false;
        }
        out->cc[i] = (cc_t)values[4 + i];
    }

    if (count == max_fields) {
        out->has_ispeed = true;
        out->ispeed = (speed_t)values[4 + (size_t)NCCS];
        out->has_ospeed = true;
        out->ospeed = (speed_t)values[4 + (size_t)NCCS + 1];
    }

    return true;
}

static void bx_stty_set_mode(struct bx_stty_plan* plan, enum bx_stty_mode mode) {
    if (plan->mode == BX_STTY_MODE_DEFAULT) {
        plan->mode = mode;
        return;
    }

    if (plan->mode != mode) {
        plan->mode_conflict = true;
    }
}

static bool bx_stty_parse_short_argument(int argc, char** argv, int* index, const char* arg, struct bx_stty_plan* plan, struct bx_stty_setting_tokens* settings) {
    size_t length = strlen(arg);
    for (size_t j = 1; j < length; j++) {
        char c = arg[j];

        if (c == 'a') {
            bx_stty_set_mode(plan, BX_STTY_MODE_ALL);
            continue;
        }

        if (c == 'g') {
            bx_stty_set_mode(plan, BX_STTY_MODE_SAVE);
            continue;
        }

        if (c == 'F') {
            if (j + 1 < length) {
                plan->device_path = arg + j + 1;
                return true;
            }

            if (*index + 1 < argc) {
                *index += 1;
                plan->device_path = argv[*index];
                return true;
            }

            return bx_stty_tokens_add(settings, "-F");
        }

        char* synthesized = xmalloc((length - j) + 2);
        synthesized[0] = '-';
        memcpy(synthesized + 1, arg + j, length - j + 1);

        bool ok = bx_stty_tokens_add(settings, synthesized);
        free(synthesized);
        return ok;
    }

    return true;
}

static bool bx_stty_collect_arguments(int argc, char** argv, struct bx_stty_plan* plan, struct bx_stty_setting_tokens* settings, struct bx_diag_ctx* diag) {
    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];

        if (strcmp(arg, "--") == 0) {
            for (i = i + 1; i < argc; i++) {
                if (!bx_stty_tokens_add(settings, argv[i])) {
                    return false;
                }
            }
            break;
        }

        if (strcmp(arg, "--help") == 0) {
            plan->special_mode = BX_STTY_SPECIAL_HELP;
            continue;
        }

        if (strcmp(arg, "--version") == 0) {
            if (plan->special_mode == BX_STTY_SPECIAL_NONE) {
                plan->special_mode = BX_STTY_SPECIAL_VERSION;
            }
            continue;
        }

        if (plan->special_mode != BX_STTY_SPECIAL_NONE) {
            continue;
        }

        if (strcmp(arg, "--all") == 0) {
            bx_stty_set_mode(plan, BX_STTY_MODE_ALL);
            continue;
        }

        if (strcmp(arg, "--save") == 0) {
            bx_stty_set_mode(plan, BX_STTY_MODE_SAVE);
            continue;
        }

        if (strcmp(arg, "--file") == 0) {
            if (i + 1 < argc) {
                i++;
                plan->device_path = argv[i];
            }
            else if (!bx_stty_tokens_add(settings, "--file")) {
                return false;
            }
            continue;
        }

        if (strncmp(arg, "--file=", 7) == 0) {
            plan->device_path = arg + 7;
            continue;
        }

        if (arg[0] == '-' && arg[1] != '\0') {
            if (!bx_stty_parse_short_argument(argc, argv, &i, arg, plan, settings)) {
                return false;
            }
            continue;
        }

        if (!bx_stty_tokens_add(settings, arg)) {
            return false;
        }
    }

    if (plan->mode_conflict) {
        bx_diag(diag, "the options for verbose and stty-readable output styles are\nmutually exclusive");
        return false;
    }

    if (plan->mode != BX_STTY_MODE_DEFAULT && settings->count > 0) {
        bx_diag(diag, "when specifying an output style, modes may not be set");
        return false;
    }

    return true;
}

static bool bx_stty_parse_setting_token(const struct bx_stty_setting_tokens* settings, size_t* index, struct bx_stty_plan* plan, struct bx_diag_ctx* diag) {
    const char* token = settings->values[*index];
    if (token == NULL || token[0] == '\0') {
        bx_diag(diag, "invalid argument '%s'", token ? token : "");
        return false;
    }

    struct bx_stty_g_state gstate;
    if (bx_stty_parse_save_state(token, &gstate)) {
        struct bx_stty_op op = bx_stty_make_op(BX_STTY_OP_GSTATE);
        op.u.gstate = gstate;
        return bx_stty_plan_append_op(plan, &op);
    }

    bool negated = false;
    const char* name = token;
    if (name[0] == '-' && name[1] != '\0') {
        negated = true;
        name++;
    }

    if (strcmp(name, "drain") == 0) {
        plan->drain = !negated;
        return true;
    }

    if (!negated && strcmp(name, "size") == 0) {
        return bx_stty_plan_append_simple_op(plan, BX_STTY_OP_PRINT_SIZE);
    }

    if (!negated && strcmp(name, "speed") == 0) {
        return bx_stty_plan_append_simple_op(plan, BX_STTY_OP_PRINT_SPEED);
    }

    if (!negated && (strcmp(name, "rows") == 0 || strcmp(name, "row") == 0)) {
        if (*index + 1 >= settings->count) {
            bx_diag(diag, "missing argument to '%s'", token);
            return false;
        }

        unsigned value = 0;
        if (!bx_stty_parse_unsigned(settings->values[*index + 1], USHRT_MAX, &value)) {
            bx_diag(diag, "invalid argument '%s'", settings->values[*index + 1]);
            return false;
        }

        *index += 1;
        return bx_stty_plan_append_numeric_op(plan, BX_STTY_OP_ROWS, value);
    }

    if (!negated && (strcmp(name, "cols") == 0 || strcmp(name, "columns") == 0)) {
        if (*index + 1 >= settings->count) {
            bx_diag(diag, "missing argument to '%s'", token);
            return false;
        }

        unsigned value = 0;
        if (!bx_stty_parse_unsigned(settings->values[*index + 1], USHRT_MAX, &value)) {
            bx_diag(diag, "invalid argument '%s'", settings->values[*index + 1]);
            return false;
        }

        *index += 1;
        return bx_stty_plan_append_numeric_op(plan, BX_STTY_OP_COLS, value);
    }

    if (!negated && strcmp(name, "line") == 0) {
#ifdef TIOCSETD
        if (*index + 1 >= settings->count) {
            bx_diag(diag, "missing argument to '%s'", token);
            return false;
        }

        int value = 0;
        if (!bx_stty_parse_int_nonnegative(settings->values[*index + 1], &value)) {
            bx_diag(diag, "invalid argument '%s'", settings->values[*index + 1]);
            return false;
        }

        *index += 1;
        return bx_stty_plan_append_line_op(plan, value);
#else
        bx_diag(diag, "line discipline setting is not supported on this platform");
        return false;
#endif
    }

    if (!negated && strcmp(name, "ispeed") == 0) {
        if (*index + 1 >= settings->count) {
            bx_diag(diag, "missing argument to '%s'", token);
            return false;
        }

        speed_t speed = 0;
        if (!bx_stty_parse_speed(settings->values[*index + 1], &speed)) {
            bx_diag(diag, "invalid argument '%s'", settings->values[*index + 1]);
            return false;
        }

        *index += 1;
        return bx_stty_plan_append_speed_op(plan, BX_STTY_OP_ISPEED, speed);
    }

    if (!negated && strcmp(name, "ospeed") == 0) {
        if (*index + 1 >= settings->count) {
            bx_diag(diag, "missing argument to '%s'", token);
            return false;
        }

        speed_t speed = 0;
        if (!bx_stty_parse_speed(settings->values[*index + 1], &speed)) {
            bx_diag(diag, "invalid argument '%s'", settings->values[*index + 1]);
            return false;
        }

        *index += 1;
        return bx_stty_plan_append_speed_op(plan, BX_STTY_OP_OSPEED, speed);
    }

    const struct bx_stty_cc_spec* cc_spec = bx_stty_lookup_cc_spec(name);
    if (cc_spec != NULL) {
        if (negated) {
            bx_diag(diag, "invalid argument '%s'", token);
            return false;
        }

        if (*index + 1 >= settings->count) {
            bx_diag(diag, "missing argument to '%s'", token);
            return false;
        }

        const char* value_token = settings->values[*index + 1];
        cc_t cc_value = 0;
        bool disable = false;

        if (cc_spec->numeric) {
            unsigned numeric = 0;
            if (!bx_stty_parse_unsigned(value_token, UCHAR_MAX, &numeric)) {
                bx_diag(diag, "invalid argument '%s'", value_token);
                return false;
            }
            cc_value = (cc_t)numeric;
        }
        else {
            if (!bx_stty_parse_cc_value(value_token, &cc_value, &disable)) {
                bx_diag(diag, "invalid argument '%s'", value_token);
                return false;
            }
        }

        *index += 1;
        struct bx_stty_op op;
        memset(&op, 0, sizeof(op));
        op.kind = BX_STTY_OP_CC;
        op.u.cc.cc_index = cc_spec->index;
        op.u.cc.value = cc_value;
        op.u.cc.disable = disable;
        return bx_stty_plan_append_op(plan, &op);
    }

    if (strcmp(name, "raw") == 0) {
        return bx_stty_plan_append_composite_op(plan, negated ? BX_STTY_COMPOSITE_COOKED : BX_STTY_COMPOSITE_RAW);
    }

    if (!negated && strcmp(name, "cooked") == 0) {
        return bx_stty_plan_append_composite_op(plan, BX_STTY_COMPOSITE_COOKED);
    }

    if (negated && strcmp(name, "cooked") == 0) {
        return bx_stty_plan_append_composite_op(plan, BX_STTY_COMPOSITE_RAW);
    }

    if (strcmp(name, "cbreak") == 0) {
        return bx_stty_plan_append_composite_op(plan, negated ? BX_STTY_COMPOSITE_UNCBREAK : BX_STTY_COMPOSITE_CBREAK);
    }

    if (!negated && strcmp(name, "sane") == 0) {
        return bx_stty_plan_append_composite_op(plan, BX_STTY_COMPOSITE_SANE);
    }

    if (!negated && strcmp(name, "ek") == 0) {
        return bx_stty_plan_append_composite_op(plan, BX_STTY_COMPOSITE_EK);
    }

    if (strcmp(name, "nl") == 0) {
        return bx_stty_plan_append_composite_op(plan, negated ? BX_STTY_COMPOSITE_UNNL : BX_STTY_COMPOSITE_NL);
    }

    if (strcmp(name, "pass8") == 0) {
        return bx_stty_plan_append_composite_op(plan, negated ? BX_STTY_COMPOSITE_UNPASS8 : BX_STTY_COMPOSITE_PASS8);
    }

    if (strcmp(name, "litout") == 0) {
        return bx_stty_plan_append_composite_op(plan, negated ? BX_STTY_COMPOSITE_UNLITOUT : BX_STTY_COMPOSITE_LITOUT);
    }

    if (strcmp(name, "evenp") == 0 || strcmp(name, "parity") == 0) {
        return bx_stty_plan_append_composite_op(plan, negated ? BX_STTY_COMPOSITE_UNEVENP : BX_STTY_COMPOSITE_EVENP);
    }

    if (strcmp(name, "oddp") == 0) {
        return bx_stty_plan_append_composite_op(plan, negated ? BX_STTY_COMPOSITE_UNODDP : BX_STTY_COMPOSITE_ODDP);
    }

    if (!negated && strcmp(name, "crt") == 0) {
        return bx_stty_plan_append_composite_op(plan, BX_STTY_COMPOSITE_CRT);
    }

    if (!negated && strcmp(name, "dec") == 0) {
        return bx_stty_plan_append_composite_op(plan, BX_STTY_COMPOSITE_DEC);
    }

    if (strcmp(name, "decctlq") == 0) {
        return bx_stty_plan_append_composite_op(plan, negated ? BX_STTY_COMPOSITE_UNDECCTLQ : BX_STTY_COMPOSITE_DECCTLQ);
    }

    if (strcmp(name, "lcase") == 0 || strcmp(name, "LCASE") == 0) {
        return bx_stty_plan_append_composite_op(plan, negated ? BX_STTY_COMPOSITE_UNLCASE : BX_STTY_COMPOSITE_LCASE);
    }

#ifdef CSIZE
    if (!negated && strcmp(name, "cs5") == 0) {
#ifdef CS5
        return bx_stty_plan_append_flag_mask_op(plan, BX_STTY_FIELD_CFLAG, CSIZE, CS5);
#else
        bx_diag(diag, "cs5 is not supported on this platform");
        return false;
#endif
    }

    if (!negated && strcmp(name, "cs6") == 0) {
#ifdef CS6
        return bx_stty_plan_append_flag_mask_op(plan, BX_STTY_FIELD_CFLAG, CSIZE, CS6);
#else
        bx_diag(diag, "cs6 is not supported on this platform");
        return false;
#endif
    }

    if (!negated && strcmp(name, "cs7") == 0) {
#ifdef CS7
        return bx_stty_plan_append_flag_mask_op(plan, BX_STTY_FIELD_CFLAG, CSIZE, CS7);
#else
        bx_diag(diag, "cs7 is not supported on this platform");
        return false;
#endif
    }

    if (!negated && strcmp(name, "cs8") == 0) {
#ifdef CS8
        return bx_stty_plan_append_flag_mask_op(plan, BX_STTY_FIELD_CFLAG, CSIZE, CS8);
#else
        bx_diag(diag, "cs8 is not supported on this platform");
        return false;
#endif
    }
#endif

#if defined(TABDLY)
    if (strcmp(name, "tabs") == 0) {
        tcflag_t value = (tcflag_t)0;
#if defined(TAB3)
        value = negated ? TAB3 : (tcflag_t)0;
#elif defined(XTABS)
        value = negated ? XTABS : (tcflag_t)0;
#else
        if (negated) {
            bx_diag(diag, "-tabs is not supported on this platform");
            return false;
        }
#endif

        return bx_stty_plan_append_flag_mask_op(plan, BX_STTY_FIELD_OFLAG, TABDLY, value);
    }
#endif

    const char* flag_name = bx_stty_resolve_alias(name);
    const struct bx_stty_flag_spec* flag_spec = bx_stty_lookup_flag_spec(flag_name);
    if (flag_spec != NULL) {
        return bx_stty_plan_append_flag_op(plan, flag_spec->field, flag_spec->bit, !negated);
    }

    if (!negated) {
        speed_t speed = 0;
        if (bx_stty_parse_speed(name, &speed)) {
            return bx_stty_plan_append_speed_op(plan, BX_STTY_OP_SPEED_BOTH, speed);
        }
    }

    bx_diag(diag, "invalid argument '%s'", token);
    return false;
}

static bool bx_stty_parse_plan(int argc, char** argv, struct bx_stty_plan* plan, struct bx_diag_ctx* diag) {
    struct bx_stty_setting_tokens settings;
    bx_stty_tokens_init(&settings);

    bool ok = bx_stty_collect_arguments(argc, argv, plan, &settings, diag);
    if (!ok) {
        bx_stty_tokens_free(&settings);
        return false;
    }

    if (plan->special_mode != BX_STTY_SPECIAL_NONE) {
        bx_stty_tokens_free(&settings);
        return true;
    }

    for (size_t i = 0; i < settings.count; i++) {
        if (!bx_stty_parse_setting_token(&settings, &i, plan, diag)) {
            bx_stty_tokens_free(&settings);
            return false;
        }
    }

    bx_stty_tokens_free(&settings);
    return true;
}

static tcflag_t* bx_stty_field_ptr(struct termios* tio, enum bx_stty_field field) {
    switch (field) {
        case BX_STTY_FIELD_IFLAG:
            return &tio->c_iflag;
        case BX_STTY_FIELD_OFLAG:
            return &tio->c_oflag;
        case BX_STTY_FIELD_CFLAG:
            return &tio->c_cflag;
        case BX_STTY_FIELD_LFLAG:
            return &tio->c_lflag;
    }

    return NULL;
}

static const tcflag_t* bx_stty_const_field_ptr(const struct termios* tio, enum bx_stty_field field) {
    switch (field) {
        case BX_STTY_FIELD_IFLAG:
            return &tio->c_iflag;
        case BX_STTY_FIELD_OFLAG:
            return &tio->c_oflag;
        case BX_STTY_FIELD_CFLAG:
            return &tio->c_cflag;
        case BX_STTY_FIELD_LFLAG:
            return &tio->c_lflag;
    }

    return NULL;
}

static bool bx_stty_write_newline(struct bx_diag_ctx* diag) {
    if (fputc('\n', stdout) == EOF) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }

    return true;
}

static bool bx_stty_print_speed_line(const struct termios* tio, struct bx_diag_ctx* diag) {
    const unsigned int speed = bx_stty_speed_to_baud(cfgetospeed(tio));
    if (printf("%u\n", speed) < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }

    return true;
}

static bool bx_stty_print_size_line(const struct winsize* ws, struct bx_diag_ctx* diag) {
    if (printf("%u %u\n", (unsigned)ws->ws_row, (unsigned)ws->ws_col) < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }

    return true;
}

static bool bx_stty_print_default(const struct bx_stty_state* state, struct bx_diag_ctx* diag) {
    const unsigned int speed = bx_stty_speed_to_baud(cfgetospeed(&state->tio));
    if (printf("speed %u baud;", speed) < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }

    if (state->have_ws) {
        if (printf(" rows %u; columns %u;", (unsigned)state->ws.ws_row, (unsigned)state->ws.ws_col) < 0) {
            bx_diag(diag, "write error: %s", strerror(errno));
            return false;
        }
    }

    if (state->have_ldisc) {
        if (printf(" line = %d;", state->ldisc) < 0) {
            bx_diag(diag, "write error: %s", strerror(errno));
            return false;
        }
    }

    return bx_stty_write_newline(diag);
}

static void bx_stty_format_cc_value(const struct bx_stty_cc_spec* spec, cc_t value, char* buffer, size_t buffer_size) {
    if (spec->numeric) {
        snprintf(buffer, buffer_size, "%u", (unsigned)value);
        return;
    }

    if (value == bx_stty_vdisable()) {
        snprintf(buffer, buffer_size, "<undef>");
        return;
    }

    if (value == 0x7f) {
        snprintf(buffer, buffer_size, "^?");
        return;
    }

    if (value < 0x20) {
        snprintf(buffer, buffer_size, "^%c", (int)(value + '@'));
        return;
    }

    if (isprint((unsigned char)value)) {
        snprintf(buffer, buffer_size, "%c", (int)value);
        return;
    }

    snprintf(buffer, buffer_size, "%u", (unsigned)value);
}

static bool bx_stty_print_all(const struct bx_stty_state* state, struct bx_diag_ctx* diag) {
    const unsigned int speed = bx_stty_speed_to_baud(cfgetospeed(&state->tio));
    if (printf("speed %u baud;", speed) < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }

    if (state->have_ws) {
        if (printf(" rows %u; columns %u;", (unsigned)state->ws.ws_row, (unsigned)state->ws.ws_col) < 0) {
            bx_diag(diag, "write error: %s", strerror(errno));
            return false;
        }
    }

    if (state->have_ldisc) {
        if (printf(" line = %d;", state->ldisc) < 0) {
            bx_diag(diag, "write error: %s", strerror(errno));
            return false;
        }
    }

    if (!bx_stty_write_newline(diag)) {
        return false;
    }

    for (size_t i = 0; i < sizeof(bx_stty_cc_table) / sizeof(bx_stty_cc_table[0]); i++) {
        const struct bx_stty_cc_spec* spec = &bx_stty_cc_table[i];
        char value_buf[32];
        bx_stty_format_cc_value(spec, state->tio.c_cc[spec->index], value_buf, sizeof(value_buf));

        if (printf("%s = %s;", spec->name, value_buf) < 0) {
            bx_diag(diag, "write error: %s", strerror(errno));
            return false;
        }

        if ((i + 1) % 4 == 0) {
            if (!bx_stty_write_newline(diag)) {
                return false;
            }
        }
        else if (fputc(' ', stdout) == EOF) {
            bx_diag(diag, "write error: %s", strerror(errno));
            return false;
        }
    }

    if (sizeof(bx_stty_cc_table) / sizeof(bx_stty_cc_table[0]) % 4 != 0) {
        if (!bx_stty_write_newline(diag)) {
            return false;
        }
    }

    for (size_t i = 0; i < sizeof(bx_stty_flag_table) / sizeof(bx_stty_flag_table[0]); i++) {
        const struct bx_stty_flag_spec* spec = &bx_stty_flag_table[i];
        const tcflag_t* field = bx_stty_const_field_ptr(&state->tio, spec->field);
        bool enabled = (field != NULL) && ((*field & spec->bit) != 0);

        if (!enabled && fputc('-', stdout) == EOF) {
            bx_diag(diag, "write error: %s", strerror(errno));
            return false;
        }

        if (fputs(spec->name, stdout) == EOF) {
            bx_diag(diag, "write error: %s", strerror(errno));
            return false;
        }

        if (i + 1 < sizeof(bx_stty_flag_table) / sizeof(bx_stty_flag_table[0])) {
            if (fputc(' ', stdout) == EOF) {
                bx_diag(diag, "write error: %s", strerror(errno));
                return false;
            }
        }
    }

#ifdef CSIZE
    tcflag_t csize = state->tio.c_cflag & CSIZE;
#ifdef CS5
    if (csize == CS5 && printf(" cs5") < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
#endif
#ifdef CS6
    if (csize == CS6 && printf(" cs6") < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
#endif
#ifdef CS7
    if (csize == CS7 && printf(" cs7") < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
#endif
#ifdef CS8
    if (csize == CS8 && printf(" cs8") < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
#endif
#endif

#if defined(TABDLY)
#ifdef TAB3
    if (printf(" %s", ((state->tio.c_oflag & TABDLY) == TAB3) ? "tab3" : "tab0") < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
#elif defined(XTABS)
    if (printf(" %s", ((state->tio.c_oflag & TABDLY) == XTABS) ? "tab3" : "tab0") < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
#endif
#endif

    return bx_stty_write_newline(diag);
}

static bool bx_stty_print_save(const struct bx_stty_state* state, struct bx_diag_ctx* diag) {
    if (printf("%lx:%lx:%lx:%lx", (unsigned long)state->tio.c_iflag, (unsigned long)state->tio.c_oflag, (unsigned long)state->tio.c_cflag, (unsigned long)state->tio.c_lflag) < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }

    for (size_t i = 0; i < (size_t)NCCS; i++) {
        if (printf(":%x", (unsigned)state->tio.c_cc[i]) < 0) {
            bx_diag(diag, "write error: %s", strerror(errno));
            return false;
        }
    }

    return bx_stty_write_newline(diag);
}

static void bx_stty_set_cs7(struct termios* tio) {
#ifdef CSIZE
    tio->c_cflag &= ~CSIZE;
#ifdef CS7
    tio->c_cflag |= CS7;
#endif
#else
    (void)tio;
#endif
}

static void bx_stty_set_cs8(struct termios* tio) {
#ifdef CSIZE
    tio->c_cflag &= ~CSIZE;
#ifdef CS8
    tio->c_cflag |= CS8;
#endif
#else
    (void)tio;
#endif
}

static void bx_stty_apply_cooked(struct termios* tio) {
#ifdef IGNBRK
    tio->c_iflag &= ~IGNBRK;
#endif
#ifdef BRKINT
    tio->c_iflag |= BRKINT;
#endif
#ifdef IGNPAR
    tio->c_iflag |= IGNPAR;
#endif
#ifdef PARMRK
    tio->c_iflag &= ~PARMRK;
#endif
#ifdef INPCK
    tio->c_iflag &= ~INPCK;
#endif
#ifdef ISTRIP
    tio->c_iflag |= ISTRIP;
#endif
#ifdef INLCR
    tio->c_iflag &= ~INLCR;
#endif
#ifdef IGNCR
    tio->c_iflag &= ~IGNCR;
#endif
#ifdef ICRNL
    tio->c_iflag |= ICRNL;
#endif
#ifdef IXON
    tio->c_iflag |= IXON;
#endif
#ifdef IXOFF
    tio->c_iflag &= ~IXOFF;
#endif
#ifdef OPOST
    tio->c_oflag |= OPOST;
#endif
#ifdef ISIG
    tio->c_lflag |= ISIG;
#endif
#ifdef ICANON
    tio->c_lflag |= ICANON;
#endif
#ifdef VMIN
    tio->c_cc[VMIN] = (cc_t)1;
#endif
#ifdef VTIME
    tio->c_cc[VTIME] = (cc_t)0;
#endif
}

static void bx_stty_apply_raw(struct termios* tio) {
#ifdef IGNBRK
    tio->c_iflag &= ~IGNBRK;
#endif
#ifdef BRKINT
    tio->c_iflag &= ~BRKINT;
#endif
#ifdef IGNPAR
    tio->c_iflag &= ~IGNPAR;
#endif
#ifdef PARMRK
    tio->c_iflag &= ~PARMRK;
#endif
#ifdef INPCK
    tio->c_iflag &= ~INPCK;
#endif
#ifdef ISTRIP
    tio->c_iflag &= ~ISTRIP;
#endif
#ifdef INLCR
    tio->c_iflag &= ~INLCR;
#endif
#ifdef IGNCR
    tio->c_iflag &= ~IGNCR;
#endif
#ifdef ICRNL
    tio->c_iflag &= ~ICRNL;
#endif
#ifdef IXON
    tio->c_iflag &= ~IXON;
#endif
#ifdef IXOFF
    tio->c_iflag &= ~IXOFF;
#endif
#ifdef OPOST
    tio->c_oflag &= ~OPOST;
#endif
#ifdef ISIG
    tio->c_lflag &= ~ISIG;
#endif
#ifdef ICANON
    tio->c_lflag &= ~ICANON;
#endif
    bx_stty_set_cs8(tio);
#ifdef VMIN
    tio->c_cc[VMIN] = (cc_t)1;
#endif
#ifdef VTIME
    tio->c_cc[VTIME] = (cc_t)0;
#endif
}

static void bx_stty_apply_sane(struct termios* tio) {
#ifdef IGNBRK
    tio->c_iflag &= ~IGNBRK;
#endif
#ifdef BRKINT
    tio->c_iflag |= BRKINT;
#endif
#ifdef IGNPAR
    tio->c_iflag &= ~IGNPAR;
#endif
#ifdef PARMRK
    tio->c_iflag &= ~PARMRK;
#endif
#ifdef INPCK
    tio->c_iflag &= ~INPCK;
#endif
#ifdef ISTRIP
    tio->c_iflag &= ~ISTRIP;
#endif
#ifdef INLCR
    tio->c_iflag &= ~INLCR;
#endif
#ifdef IGNCR
    tio->c_iflag &= ~IGNCR;
#endif
#ifdef ICRNL
    tio->c_iflag |= ICRNL;
#endif
#ifdef IXON
    tio->c_iflag |= IXON;
#endif
#ifdef IXOFF
    tio->c_iflag &= ~IXOFF;
#endif
#ifdef IXANY
    tio->c_iflag &= ~IXANY;
#endif
#ifdef IMAXBEL
    tio->c_iflag |= IMAXBEL;
#endif
#ifdef OPOST
    tio->c_oflag |= OPOST;
#endif
#ifdef ONLCR
    tio->c_oflag |= ONLCR;
#endif
#ifdef CREAD
    tio->c_cflag |= CREAD;
#endif
    bx_stty_set_cs8(tio);
#ifdef ISIG
    tio->c_lflag |= ISIG;
#endif
#ifdef ICANON
    tio->c_lflag |= ICANON;
#endif
#ifdef IEXTEN
    tio->c_lflag |= IEXTEN;
#endif
#ifdef ECHO
    tio->c_lflag |= ECHO;
#endif
#ifdef ECHOE
    tio->c_lflag |= ECHOE;
#endif
#ifdef ECHOK
    tio->c_lflag |= ECHOK;
#endif
#ifdef ECHONL
    tio->c_lflag &= ~ECHONL;
#endif
#ifdef NOFLSH
    tio->c_lflag &= ~NOFLSH;
#endif
#ifdef TOSTOP
    tio->c_lflag &= ~TOSTOP;
#endif

#ifdef VINTR
    tio->c_cc[VINTR] = (cc_t)3;
#endif
#ifdef VQUIT
    tio->c_cc[VQUIT] = (cc_t)28;
#endif
#ifdef VERASE
    tio->c_cc[VERASE] = (cc_t)127;
#endif
#ifdef VKILL
    tio->c_cc[VKILL] = (cc_t)21;
#endif
#ifdef VEOF
    tio->c_cc[VEOF] = (cc_t)4;
#endif
#ifdef VEOL
    tio->c_cc[VEOL] = bx_stty_vdisable();
#endif
#ifdef VEOL2
    tio->c_cc[VEOL2] = bx_stty_vdisable();
#endif
#ifdef VSTART
    tio->c_cc[VSTART] = (cc_t)17;
#endif
#ifdef VSTOP
    tio->c_cc[VSTOP] = (cc_t)19;
#endif
#ifdef VSUSP
    tio->c_cc[VSUSP] = (cc_t)26;
#endif
#ifdef VREPRINT
    tio->c_cc[VREPRINT] = (cc_t)18;
#endif
#ifdef VWERASE
    tio->c_cc[VWERASE] = (cc_t)23;
#endif
#ifdef VLNEXT
    tio->c_cc[VLNEXT] = (cc_t)22;
#endif
#ifdef VDISCARD
    tio->c_cc[VDISCARD] = (cc_t)15;
#endif
#ifdef VMIN
    tio->c_cc[VMIN] = (cc_t)1;
#endif
#ifdef VTIME
    tio->c_cc[VTIME] = (cc_t)0;
#endif
}

static bool bx_stty_apply_composite(struct termios* tio, enum bx_stty_composite id) {
    switch (id) {
        case BX_STTY_COMPOSITE_RAW:
            bx_stty_apply_raw(tio);
            return true;

        case BX_STTY_COMPOSITE_COOKED:
            bx_stty_apply_cooked(tio);
            return true;

        case BX_STTY_COMPOSITE_CBREAK:
#ifdef ICANON
            tio->c_lflag &= ~ICANON;
#endif
#ifdef VMIN
            tio->c_cc[VMIN] = (cc_t)1;
#endif
#ifdef VTIME
            tio->c_cc[VTIME] = (cc_t)0;
#endif
            return true;

        case BX_STTY_COMPOSITE_UNCBREAK:
#ifdef ICANON
            tio->c_lflag |= ICANON;
#endif
            return true;

        case BX_STTY_COMPOSITE_SANE:
            bx_stty_apply_sane(tio);
            return true;

        case BX_STTY_COMPOSITE_EK:
#ifdef VERASE
            tio->c_cc[VERASE] = (cc_t)127;
#endif
#ifdef VKILL
            tio->c_cc[VKILL] = (cc_t)21;
#endif
            return true;

        case BX_STTY_COMPOSITE_NL:
#ifdef ICRNL
            tio->c_iflag &= ~ICRNL;
#endif
#ifdef ONLCR
            tio->c_oflag &= ~ONLCR;
#endif
            return true;

        case BX_STTY_COMPOSITE_UNNL:
#ifdef ICRNL
            tio->c_iflag |= ICRNL;
#endif
#ifdef INLCR
            tio->c_iflag &= ~INLCR;
#endif
#ifdef IGNCR
            tio->c_iflag &= ~IGNCR;
#endif
#ifdef ONLCR
            tio->c_oflag |= ONLCR;
#endif
#ifdef OCRNL
            tio->c_oflag &= ~OCRNL;
#endif
#ifdef ONLRET
            tio->c_oflag &= ~ONLRET;
#endif
            return true;

        case BX_STTY_COMPOSITE_PASS8:
#ifdef PARENB
            tio->c_cflag &= ~PARENB;
#endif
#ifdef ISTRIP
            tio->c_iflag &= ~ISTRIP;
#endif
            bx_stty_set_cs8(tio);
            return true;

        case BX_STTY_COMPOSITE_UNPASS8:
#ifdef PARENB
            tio->c_cflag |= PARENB;
#endif
#ifdef ISTRIP
            tio->c_iflag |= ISTRIP;
#endif
            bx_stty_set_cs7(tio);
            return true;

        case BX_STTY_COMPOSITE_LITOUT:
#ifdef PARENB
            tio->c_cflag &= ~PARENB;
#endif
#ifdef ISTRIP
            tio->c_iflag &= ~ISTRIP;
#endif
#ifdef OPOST
            tio->c_oflag &= ~OPOST;
#endif
            bx_stty_set_cs8(tio);
            return true;

        case BX_STTY_COMPOSITE_UNLITOUT:
#ifdef PARENB
            tio->c_cflag |= PARENB;
#endif
#ifdef ISTRIP
            tio->c_iflag |= ISTRIP;
#endif
#ifdef OPOST
            tio->c_oflag |= OPOST;
#endif
            bx_stty_set_cs7(tio);
            return true;

        case BX_STTY_COMPOSITE_EVENP:
#ifdef PARENB
            tio->c_cflag |= PARENB;
#endif
#ifdef PARODD
            tio->c_cflag &= ~PARODD;
#endif
            bx_stty_set_cs7(tio);
            return true;

        case BX_STTY_COMPOSITE_UNEVENP:
            bx_stty_set_cs8(tio);
#ifdef PARENB
            tio->c_cflag &= ~PARENB;
#endif
            return true;

        case BX_STTY_COMPOSITE_ODDP:
#ifdef PARENB
            tio->c_cflag |= PARENB;
#endif
#ifdef PARODD
            tio->c_cflag |= PARODD;
#endif
            bx_stty_set_cs7(tio);
            return true;

        case BX_STTY_COMPOSITE_UNODDP:
            bx_stty_set_cs8(tio);
#ifdef PARENB
            tio->c_cflag &= ~PARENB;
#endif
            return true;

        case BX_STTY_COMPOSITE_CRT:
#ifdef ECHOE
            tio->c_lflag |= ECHOE;
#endif
#ifdef ECHOCTL
            tio->c_lflag |= ECHOCTL;
#endif
#ifdef ECHOKE
            tio->c_lflag |= ECHOKE;
#endif
            return true;

        case BX_STTY_COMPOSITE_DEC:
#ifdef ECHOE
            tio->c_lflag |= ECHOE;
#endif
#ifdef ECHOCTL
            tio->c_lflag |= ECHOCTL;
#endif
#ifdef ECHOKE
            tio->c_lflag |= ECHOKE;
#endif
#ifdef IXANY
            tio->c_iflag &= ~IXANY;
#endif
#ifdef VINTR
            tio->c_cc[VINTR] = (cc_t)3;
#endif
#ifdef VERASE
            tio->c_cc[VERASE] = (cc_t)127;
#endif
#ifdef VKILL
            tio->c_cc[VKILL] = (cc_t)21;
#endif
            return true;

        case BX_STTY_COMPOSITE_DECCTLQ:
#ifdef IXANY
            tio->c_iflag &= ~IXANY;
#endif
            return true;

        case BX_STTY_COMPOSITE_UNDECCTLQ:
#ifdef IXANY
            tio->c_iflag |= IXANY;
#endif
            return true;

        case BX_STTY_COMPOSITE_LCASE:
#ifdef XCASE
            tio->c_lflag |= XCASE;
#endif
#ifdef IUCLC
            tio->c_iflag |= IUCLC;
#endif
#ifdef OLCUC
            tio->c_oflag |= OLCUC;
#endif
            return true;

        case BX_STTY_COMPOSITE_UNLCASE:
#ifdef XCASE
            tio->c_lflag &= ~XCASE;
#endif
#ifdef IUCLC
            tio->c_iflag &= ~IUCLC;
#endif
#ifdef OLCUC
            tio->c_oflag &= ~OLCUC;
#endif
            return true;
    }

    return false;
}

static void bx_stty_compute_requirements(const struct bx_stty_plan* plan, struct bx_stty_requirements* req) {
    memset(req, 0, sizeof(*req));

    req->want_default_print = (plan->mode == BX_STTY_MODE_DEFAULT && plan->op_count == 0);
    req->want_all_print = (plan->mode == BX_STTY_MODE_ALL);

    if (plan->mode == BX_STTY_MODE_SAVE || req->want_default_print || req->want_all_print) {
        req->need_tio = true;
    }

    if (req->want_default_print || req->want_all_print) {
        req->need_ws = true;
        req->want_ldisc_print = true;
    }

    for (size_t i = 0; i < plan->op_count; i++) {
        switch (plan->ops[i].kind) {
            case BX_STTY_OP_FLAG:
            case BX_STTY_OP_FLAG_MASK:
            case BX_STTY_OP_CC:
            case BX_STTY_OP_ISPEED:
            case BX_STTY_OP_OSPEED:
            case BX_STTY_OP_SPEED_BOTH:
            case BX_STTY_OP_COMPOSITE:
            case BX_STTY_OP_GSTATE:
            case BX_STTY_OP_PRINT_SPEED:
                req->need_tio = true;
                break;

            case BX_STTY_OP_ROWS:
            case BX_STTY_OP_COLS:
            case BX_STTY_OP_PRINT_SIZE:
                req->need_ws = true;
                break;

            case BX_STTY_OP_LINE_DISC:
                req->need_ldisc = true;
                break;
        }
    }
}

static int bx_stty_diag_target_errno(struct bx_diag_ctx* diag, const char* target, int errnum) {
    const char* message = errnum == ENOTTY ? "Inappropriate ioctl for device" : strerror(errnum);

    bx_diag(diag, "%s: %s", target, message);
    return diag->exit_status;
}

static int bx_stty_open_target(const struct bx_stty_plan* plan, bool* close_fd, struct bx_diag_ctx* diag) {
    *close_fd = false;

    if (plan->device_path == NULL) {
        return STDIN_FILENO;
    }

    int fd = bx_fd_open_cloexec(plan->device_path, O_RDONLY | O_NONBLOCK, 0);
    if (fd < 0) {
        bx_stty_diag_target_errno(diag, plan->device_path, errno);
        return -1;
    }

    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) {
        (void)fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }

    *close_fd = true;
    return fd;
}

static int bx_stty_prepare_state(int fd, const struct bx_stty_plan* plan, const struct bx_stty_requirements* req, struct bx_stty_state* state, struct bx_diag_ctx* diag) {
    const char* target_name = (plan->device_path != NULL) ? plan->device_path : "'standard input'";
    memset(state, 0, sizeof(*state));

    if (req->need_tio) {
        if (tcgetattr(fd, &state->tio) != 0) {
            return bx_stty_diag_target_errno(diag, target_name, errno);
        }
        state->have_tio = true;
    }

    if (req->need_ws) {
        if (ioctl(fd, TIOCGWINSZ, &state->ws) != 0) {
            if (req->want_default_print || req->want_all_print) {
                state->have_ws = false;
            }
            else {
                return bx_stty_diag_target_errno(diag, target_name, errno);
            }
        }
        else {
            state->have_ws = true;
        }
    }

#ifdef TIOCGETD
    if (req->need_ldisc || req->want_ldisc_print) {
        if (ioctl(fd, TIOCGETD, &state->ldisc) != 0) {
            if (req->need_ldisc) {
                return bx_stty_diag_target_errno(diag, target_name, errno);
            }
            state->have_ldisc = false;
        }
        else {
            state->have_ldisc = true;
        }
    }
#else
    (void)fd;
    if (req->need_ldisc) {
        bx_diag(diag, "line discipline operations are not supported on this platform");
        return diag->exit_status;
    }
#endif

    return 0;
}

static int bx_stty_apply_ops(int fd, const struct bx_stty_plan* plan, struct bx_stty_state* state, struct bx_diag_ctx* diag) {
    (void)fd;

    for (size_t i = 0; i < plan->op_count; i++) {
        const struct bx_stty_op* op = &plan->ops[i];

        switch (op->kind) {
            case BX_STTY_OP_FLAG: {
                tcflag_t* field = bx_stty_field_ptr(&state->tio, op->u.flag.field);
                if (field == NULL) {
                    bx_diag(diag, "internal error: invalid flag field");
                    return diag->exit_status;
                }

                if (op->u.flag.enable) {
                    *field |= op->u.flag.bit;
                }
                else {
                    *field &= ~op->u.flag.bit;
                }
                state->need_tio_commit = true;
                break;
            }

            case BX_STTY_OP_FLAG_MASK: {
                tcflag_t* field = bx_stty_field_ptr(&state->tio, op->u.flag_mask.field);
                if (field == NULL) {
                    bx_diag(diag, "internal error: invalid flag mask field");
                    return diag->exit_status;
                }

                *field &= ~op->u.flag_mask.mask;
                *field |= op->u.flag_mask.value;
                state->need_tio_commit = true;
                break;
            }

            case BX_STTY_OP_CC:
                if (op->u.cc.cc_index >= (unsigned)NCCS) {
                    bx_diag(diag, "internal error: invalid cc index");
                    return diag->exit_status;
                }

                state->tio.c_cc[op->u.cc.cc_index] = op->u.cc.disable ? bx_stty_vdisable() : op->u.cc.value;
                state->need_tio_commit = true;
                break;

            case BX_STTY_OP_ISPEED:
                if (bx_stty_set_input_speed(&state->tio, op->u.speed.speed) != 0) {
                    bx_diag(diag, "unable to set input speed: %s", strerror(errno));
                    return diag->exit_status;
                }
                state->need_tio_commit = true;
                break;

            case BX_STTY_OP_OSPEED:
                if (cfsetospeed(&state->tio, op->u.speed.speed) != 0) {
                    bx_diag(diag, "unable to set output speed: %s", strerror(errno));
                    return diag->exit_status;
                }
                state->need_tio_commit = true;
                break;

            case BX_STTY_OP_SPEED_BOTH:
                if (bx_stty_set_both_speeds(&state->tio, op->u.speed.speed) != 0) {
                    bx_diag(diag, "unable to set speed: %s", strerror(errno));
                    return diag->exit_status;
                }
                state->need_tio_commit = true;
                break;

            case BX_STTY_OP_ROWS:
                state->ws.ws_row = (unsigned short)op->u.num.value;
                state->need_ws_commit = true;
                break;

            case BX_STTY_OP_COLS:
                state->ws.ws_col = (unsigned short)op->u.num.value;
                state->need_ws_commit = true;
                break;

            case BX_STTY_OP_LINE_DISC:
                state->ldisc = op->u.line.value;
                state->need_ldisc_commit = true;
                break;

            case BX_STTY_OP_PRINT_SIZE:
                if (!bx_stty_print_size_line(&state->ws, diag)) {
                    return diag->exit_status;
                }
                break;

            case BX_STTY_OP_PRINT_SPEED:
                if (!bx_stty_print_speed_line(&state->tio, diag)) {
                    return diag->exit_status;
                }
                break;

            case BX_STTY_OP_COMPOSITE:
                if (!bx_stty_apply_composite(&state->tio, op->u.composite.id)) {
                    bx_diag(diag, "internal error: unsupported composite operation");
                    return diag->exit_status;
                }
                state->need_tio_commit = true;
                break;

            case BX_STTY_OP_GSTATE:
                state->tio.c_iflag = op->u.gstate.iflag;
                state->tio.c_oflag = op->u.gstate.oflag;
                state->tio.c_cflag = op->u.gstate.cflag;
                state->tio.c_lflag = op->u.gstate.lflag;
                memcpy(state->tio.c_cc, op->u.gstate.cc, sizeof(state->tio.c_cc));
                if (op->u.gstate.has_ispeed && cfsetispeed(&state->tio, op->u.gstate.ispeed) != 0) {
                    bx_diag(diag, "unable to set input speed: %s", strerror(errno));
                    return diag->exit_status;
                }
                if (op->u.gstate.has_ospeed && cfsetospeed(&state->tio, op->u.gstate.ospeed) != 0) {
                    bx_diag(diag, "unable to set output speed: %s", strerror(errno));
                    return diag->exit_status;
                }
                state->need_tio_commit = true;
                break;
        }
    }

    return 0;
}

static int bx_stty_commit_changes(int fd, const struct bx_stty_plan* plan, struct bx_stty_state* state, struct bx_diag_ctx* diag) {
    const char* target_name = (plan->device_path != NULL) ? plan->device_path : "'standard input'";

    if (state->need_tio_commit) {
        int action = plan->drain ? TCSADRAIN : TCSANOW;
        if (tcsetattr(fd, action, &state->tio) != 0) {
            return bx_stty_diag_target_errno(diag, target_name, errno);
        }
    }

    if (state->need_ws_commit) {
        if (ioctl(fd, TIOCSWINSZ, &state->ws) != 0) {
            return bx_stty_diag_target_errno(diag, target_name, errno);
        }
    }

#ifdef TIOCSETD
    if (state->need_ldisc_commit) {
        if (ioctl(fd, TIOCSETD, &state->ldisc) != 0) {
            return bx_stty_diag_target_errno(diag, target_name, errno);
        }
    }
#else
    if (state->need_ldisc_commit) {
        bx_diag(diag, "line discipline operations are not supported on this platform");
        return diag->exit_status;
    }
#endif

    return 0;
}

static int bx_stty_execute_plan(int fd, const struct bx_stty_plan* plan, struct bx_diag_ctx* diag) {
    struct bx_stty_requirements req;
    bx_stty_compute_requirements(plan, &req);

    struct bx_stty_state state;
    int rc = bx_stty_prepare_state(fd, plan, &req, &state, diag);
    if (rc != 0) {
        return rc;
    }

    if (plan->mode == BX_STTY_MODE_ALL) {
        return bx_stty_print_all(&state, diag) ? 0 : diag->exit_status;
    }

    if (plan->mode == BX_STTY_MODE_SAVE) {
        return bx_stty_print_save(&state, diag) ? 0 : diag->exit_status;
    }

    if (plan->op_count == 0) {
        return bx_stty_print_default(&state, diag) ? 0 : diag->exit_status;
    }

    rc = bx_stty_apply_ops(fd, plan, &state, diag);
    if (rc != 0) {
        return rc;
    }

    return bx_stty_commit_changes(fd, plan, &state, diag);
}

int bx_stty_main(int argc, char** argv) {
    struct bx_diag_ctx diag = {
        .progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "stty"),
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    struct bx_stty_plan plan;
    bx_stty_plan_init(&plan);

    int rc = 0;
    bool close_fd = false;
    int fd = -1;

    if (!bx_stty_parse_plan(argc, argv, &plan, &diag)) {
        if (diag.exit_status == 0) {
            diag.exit_status = 1;
        }
        if (plan.special_mode == BX_STTY_SPECIAL_NONE) {
            bx_cli_print_try_help(diag.progname);
        }
        rc = diag.exit_status;
        goto out;
    }

    if (plan.special_mode == BX_STTY_SPECIAL_HELP) {
        bx_stty_print_help(stdout, diag.progname);
        rc = 0;
        goto out;
    }

    if (plan.special_mode == BX_STTY_SPECIAL_VERSION) {
        bx_cli_print_version(diag.progname);
        rc = 0;
        goto out;
    }

    fd = bx_stty_open_target(&plan, &close_fd, &diag);
    if (fd < 0) {
        rc = diag.exit_status;
        goto out;
    }

    rc = bx_stty_execute_plan(fd, &plan, &diag);

out:
    if (close_fd && fd >= 0) {
        close(fd);
    }
    bx_stty_plan_free(&plan);
    return rc;
}
