#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <math.h>
#include <errno.h>
#include <float.h>
#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/line_writer.h"
#include "lib/option_scan.h"

#define SEQ_MAX_FORMAT_FIELD 4096

static bool seq_skip_format_field(const char** cursor) {
    unsigned value = 0;
    const char* p = *cursor;
    while (*p >= '0' && *p <= '9') {
        value = value * 10 + (unsigned)(*p++ - '0');
        if (value > SEQ_MAX_FORMAT_FIELD)
            return false;
    }
    *cursor = p;
    return true;
}

/* snprintf receives one double. Allow at most one conversion, and only
 * if it consumes that argument without reading any others. */
static bool seq_valid_format(const char* p) {
    bool converted = false;
    while (*p) {
        if (*p++ != '%')
            continue;
        if (*p == '%') {
            p++;
            continue;
        }
        if (converted)
            return false;
        if (p[0] == '1' && p[1] == '$')
            p += 2;
        p += strspn(p, "#0- +'");
        if (!seq_skip_format_field(&p))
            return false;
        if (*p == '.') {
            p++;
            if (!seq_skip_format_field(&p))
                return false;
        }
        if (*p == 'l')
            p++;
        if (!*p || !strchr("aAeEfFgG", *p))
            return false;
        p++;
        converted = true;
    }
    return true;
}

static int count_decimal_places(const char* s) {
    const char* dot = strchr(s, '.');
    if (!dot) return 0;
    int n = 0;
    for (const char* p = dot + 1; *p >= '0' && *p <= '9'; p++)
        n++;
    return n;
}

static bool seq_emit_number(struct bx_line_writer* writer, const char* format, double value) {
    char stack_buffer[256];
    int len = snprintf(stack_buffer, sizeof(stack_buffer), format, value);
    if (len < 0) {
        return false;
    }

    if ((size_t)len < sizeof(stack_buffer)) {
        return bx_line_writer_write(writer, stack_buffer, (size_t)len);
    }

    char* heap_buffer = malloc((size_t)len + 1u);
    if (!heap_buffer) {
        return false;
    }
    int heap_len = snprintf(heap_buffer, (size_t)len + 1u, format, value);
    bool ok = heap_len == len && bx_line_writer_write(writer, heap_buffer, (size_t)len);
    free(heap_buffer);
    return ok;
}

static bool seq_emit_range(
    struct bx_line_writer* writer,
    double first,
    double inc,
    double last,
    const char* separator,
    const char* format
) {
    double val = first;
    bool first_out = true;

    if (inc > 0 ? first > last : first < last)
        return true;
    double bound = last + inc / 1000.0;
    while (inc > 0 ? val <= bound : val >= bound) {
        if (!first_out && !bx_line_writer_puts(writer, separator))
            return false;
        if (!seq_emit_number(writer, format, val))
            return false;
        first_out = false;
        if (inc > 0 ? val >= last : val <= last)
            break;
        double next = val + inc;
        if (next == val) {
            fprintf(stderr, "seq: increment too small to advance\n");
            return false;
        }
        val = next;
    }

    if (first_out) {
        return true;
    }
    return bx_line_writer_putc(writer, '\n') && bx_line_writer_flush(writer);
}

static bool seq_negative_operand(const char* token) {
    char* end;
    (void)strtod(token, &end);
    return end != token && *end == '\0';
}

int bx_seq_main(int argc, char** argv) {
    static const struct bx_option_spec specs[] = {
        {'f', 'f', "format", BX_OPTION_REQUIRED_VALUE}, {'s', 's', "separator", BX_OPTION_REQUIRED_VALUE}, {'w', 'w', "equal-width", BX_OPTION_NO_VALUE},
        {'h', 'h', "help", BX_OPTION_NO_VALUE},         {'v', 'v', "version", BX_OPTION_NO_VALUE},
    };
    struct bx_diag_ctx diag = {
        .progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "seq"),
        .exit_status = 1,
    };

    const char* format = NULL;
    const char* separator = "\n";
    bool equal_width = false;
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, argc > 0 ? 1u : 0u, (size_t)argc, BX_OPTION_PREFIX | BX_OPTION_ABBREVIATE, specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_UNKNOWN && option.short_name && seq_negative_operand(option.token))
            break;
        if (option.event != BX_OPTION_MATCH) {
            if (option.event == BX_OPTION_MISSING_VALUE && option.short_name)
                bx_diag(&diag, "option requires an argument -- '%c'", option.short_name);
            else if (option.event == BX_OPTION_MISSING_VALUE)
                bx_diag(&diag, "option requires an argument -- '%s'", option.token);
            else if (option.short_name)
                bx_diag(&diag, "invalid option -- '%c'", option.short_name);
            else
                bx_diag(&diag, "unrecognized option '%s'", option.token);
            return 1;
        }
        switch (option.spec->id) {
            case 'f':
                format = option.value;
                break;
            case 's':
                separator = option.value;
                break;
            case 'w':
                equal_width = true;
                break;
            case 'h':
                printf("Usage: %s [OPTION]... LAST\n", argv[0]);
                printf("  or:  %s [OPTION]... FIRST LAST\n", argv[0]);
                printf("  or:  %s [OPTION]... FIRST INCREMENT LAST\n", argv[0]);
                printf("Print numbers from FIRST to LAST, in steps of INCREMENT.\n");
                printf("\n");
                printf("  -f, --format=FORMAT    use at most one printf style floating-point conversion\n");
                printf("                         width and precision at most %d; incompatible with -w\n", SEQ_MAX_FORMAT_FIELD);
                printf("  -s, --separator=STRING use STRING to separate numbers (default: \\n)\n");
                printf("  -w, --equal-width      equalize width by padding with leading zeroes\n");
                printf("      --help              display this help and exit\n");
                printf("      --version           output version information and exit\n");
                return 0;
            case 'v':
                printf("seq (bx) %s\n", BX_VERSION);
                return 0;
        }
    }

    size_t first_operand = option.index;
    size_t num_args = (size_t)argc - first_operand;
    if (num_args < 1 || num_args > 3) {
        bx_cli_diag_missing_operand(&diag);
        return 1;
    }
    if (format != NULL) {
        if (equal_width) {
            bx_diag(&diag, "--format and --equal-width are incompatible");
            return 1;
        }
        if (!seq_valid_format(format)) {
            bx_diag(&diag, "invalid floating-point format");
            return 1;
        }
    }
    double values[3];
    for (size_t i = 0; i < num_args; i++) {
        char* end;
        const char* arg = argv[first_operand + i];
        errno = 0;
        values[i] = strtod(arg, &end);
        /* Beyond this boundary, adjacent integer operands can parse alike. */
        if (end == arg || *end || errno == ERANGE || !isfinite(values[i]) ||
            fabs(values[i]) >= ldexp(1.0, DBL_MANT_DIG)) {
            bx_diag(&diag, "invalid or out-of-range number: '%s'", arg);
            return 1;
        }
    }
    double first = num_args == 1 ? 1 : values[0];
    double inc = num_args == 3 ? values[1] : 1;
    double last = values[num_args - 1];

    if (inc == 0) {
        bx_diag(&diag, "invalid increment '0'");
        return 1;
    }

    int max_prec = 0;
    if (format == NULL) {
        for (size_t i = 0; i < num_args; i++) {
            int precision = count_decimal_places(argv[first_operand + i]);
            if (precision > max_prec)
                max_prec = precision;
        }
    }

    char fmt_buf[64];
    if (format == NULL) {
        if (max_prec > 0)
            snprintf(fmt_buf, sizeof(fmt_buf), "%%.%df", max_prec);
        else if (trunc(first) == first && trunc(inc) == inc && trunc(last) == last)
            snprintf(fmt_buf, sizeof(fmt_buf), "%%.0f");
        else
            snprintf(fmt_buf, sizeof(fmt_buf), "%%g");
        format = fmt_buf;
    }

    if (equal_width) {
        double max_v = fabs(first) > fabs(last) ? fabs(first) : fabs(last);
        int int_digits = 0;
        long long n = (long long)max_v;
        if (n == 0) int_digits = 1;
        else { while (n) { int_digits++; n /= 10; } }

        int total_width = int_digits + (max_prec > 0 ? 1 + max_prec : 0);
        snprintf(fmt_buf, sizeof(fmt_buf), "%%0%d.%df", total_width, max_prec);
    }

    char output_buffer[8192];
    struct bx_line_writer writer;
    bx_line_writer_init(&writer, STDOUT_FILENO, output_buffer, sizeof(output_buffer));
    return seq_emit_range(&writer, first, inc, last, separator, format) ? 0 : 1;
}
