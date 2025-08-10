#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <regex.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"

enum expr_token_kind {
    TOK_END = 0,
    TOK_WORD,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_OR,
    TOK_AND,
    TOK_LT,
    TOK_LE,
    TOK_EQ,
    TOK_NE,
    TOK_GE,
    TOK_GT,
    TOK_PLUS,
    TOK_MINUS,
    TOK_MUL,
    TOK_DIV,
    TOK_MOD,
    TOK_COLON,
    TOK_KW_MATCH,
    TOK_KW_SUBSTR,
    TOK_KW_INDEX,
    TOK_KW_LENGTH,
};

struct expr_token {
    enum expr_token_kind kind;
    const char* text;
};

enum expr_node_kind {
    NODE_LITERAL = 0,
    NODE_BINARY,
    NODE_PREFIX_LENGTH,
    NODE_PREFIX_MATCH,
    NODE_PREFIX_INDEX,
    NODE_PREFIX_SUBSTR,
};

enum expr_binary_op {
    OP_OR = 0,
    OP_AND,
    OP_LT,
    OP_LE,
    OP_EQ,
    OP_NE,
    OP_GE,
    OP_GT,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_COLON,
};

struct expr_node {
    enum expr_node_kind kind;
    union {
        struct {
            char* text;
        } literal;
        struct {
            enum expr_binary_op op;
            struct expr_node* left;
            struct expr_node* right;
        } binary;
        struct {
            struct expr_node* arg;
        } unary;
        struct {
            struct expr_node* a;
            struct expr_node* b;
        } pair;
        struct {
            struct expr_node* a;
            struct expr_node* b;
            struct expr_node* c;
        } triple;
    } u;
};

struct expr_parser {
    const struct expr_token* tokens;
    size_t count;
    size_t pos;
    char* error;
};

enum expr_value_type {
    VALUE_INT = 0,
    VALUE_STRING,
};

struct expr_value {
    enum expr_value_type type;
    intmax_t i;
    char* s;
};

struct int_view {
    int sign;
    const char* digits;
    size_t len;
};

struct bigint {
    int sign;
    char* digits;
    size_t len;
};

enum int_parse_status {
    INT_PARSE_OK = 0,
    INT_PARSE_INVALID,
    INT_PARSE_RANGE,
};

struct expr_eval_ctx {
    char* error;
};

struct char_span {
    const char* ptr;
    size_t len;
};

static const int PREFIX_BINDING_POWER = 70;

static char* vformat_alloc(const char* fmt, va_list ap) {
    va_list copy;
    va_copy(copy, ap);
    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);

    if (needed < 0) {
        return xstrdup("formatting error");
    }

    char* buf = xmalloc((size_t)needed + 1);
    (void)vsnprintf(buf, (size_t)needed + 1, fmt, ap);
    return buf;
}

static void parser_set_error(struct expr_parser* parser, const char* fmt, ...) {
    if (parser->error) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    parser->error = vformat_alloc(fmt, ap);
    va_end(ap);
}

static void eval_set_error(struct expr_eval_ctx* ctx, const char* fmt, ...) {
    if (ctx->error) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    ctx->error = vformat_alloc(fmt, ap);
    va_end(ap);
}

static enum expr_token_kind classify_token(const char* text) {
    if (strcmp(text, "(") == 0) {
        return TOK_LPAREN;
    }
    if (strcmp(text, ")") == 0) {
        return TOK_RPAREN;
    }
    if (strcmp(text, "|") == 0) {
        return TOK_OR;
    }
    if (strcmp(text, "&") == 0) {
        return TOK_AND;
    }
    if (strcmp(text, "<") == 0) {
        return TOK_LT;
    }
    if (strcmp(text, "<=") == 0) {
        return TOK_LE;
    }
    if (strcmp(text, "=") == 0 || strcmp(text, "==") == 0) {
        return TOK_EQ;
    }
    if (strcmp(text, "!=") == 0) {
        return TOK_NE;
    }
    if (strcmp(text, ">=") == 0) {
        return TOK_GE;
    }
    if (strcmp(text, ">") == 0) {
        return TOK_GT;
    }
    if (strcmp(text, "+") == 0) {
        return TOK_PLUS;
    }
    if (strcmp(text, "-") == 0) {
        return TOK_MINUS;
    }
    if (strcmp(text, "*") == 0) {
        return TOK_MUL;
    }
    if (strcmp(text, "/") == 0) {
        return TOK_DIV;
    }
    if (strcmp(text, "%") == 0) {
        return TOK_MOD;
    }
    if (strcmp(text, ":") == 0) {
        return TOK_COLON;
    }
    if (strcmp(text, "match") == 0) {
        return TOK_KW_MATCH;
    }
    if (strcmp(text, "substr") == 0) {
        return TOK_KW_SUBSTR;
    }
    if (strcmp(text, "index") == 0) {
        return TOK_KW_INDEX;
    }
    if (strcmp(text, "length") == 0) {
        return TOK_KW_LENGTH;
    }

    return TOK_WORD;
}

static struct expr_token* tokenize_args(int argc, char** argv, size_t* count_out) {
    size_t count = (size_t)argc + 1;
    struct expr_token* tokens = xmalloc(count * sizeof(*tokens));

    for (int i = 0; i < argc; i++) {
        tokens[i].kind = classify_token(argv[i]);
        tokens[i].text = argv[i];
    }

    tokens[argc].kind = TOK_END;
    tokens[argc].text = NULL;
    *count_out = count;
    return tokens;
}

static const struct expr_token* parser_peek(const struct expr_parser* parser) {
    if (parser->count == 0) {
        return NULL;
    }
    if (parser->pos >= parser->count) {
        return &parser->tokens[parser->count - 1];
    }
    return &parser->tokens[parser->pos];
}

static const struct expr_token* parser_consume(struct expr_parser* parser) {
    const struct expr_token* tok = parser_peek(parser);

    if (parser->pos + 1 < parser->count) {
        parser->pos++;
    }

    return tok;
}

static bool parser_accept(struct expr_parser* parser, enum expr_token_kind kind) {
    const struct expr_token* tok = parser_peek(parser);

    if (!tok || tok->kind != kind) {
        return false;
    }

    parser_consume(parser);
    return true;
}

static int token_lbp(enum expr_token_kind kind) {
    switch (kind) {
        case TOK_OR:
            return 10;
        case TOK_AND:
            return 20;
        case TOK_LT:
        case TOK_LE:
        case TOK_EQ:
        case TOK_NE:
        case TOK_GE:
        case TOK_GT:
            return 30;
        case TOK_PLUS:
        case TOK_MINUS:
            return 40;
        case TOK_MUL:
        case TOK_DIV:
        case TOK_MOD:
            return 50;
        case TOK_COLON:
            return 60;
        default:
            return 0;
    }
}

static struct expr_node* node_new(enum expr_node_kind kind) {
    struct expr_node* node = xmalloc(sizeof(*node));
    memset(node, 0, sizeof(*node));
    node->kind = kind;
    return node;
}

static struct expr_node* node_literal(const char* text) {
    struct expr_node* node = node_new(NODE_LITERAL);
    node->u.literal.text = xstrdup(text ? text : "");
    return node;
}

static struct expr_node* node_binary(enum expr_binary_op op, struct expr_node* left, struct expr_node* right) {
    struct expr_node* node = node_new(NODE_BINARY);
    node->u.binary.op = op;
    node->u.binary.left = left;
    node->u.binary.right = right;
    return node;
}

static void node_free(struct expr_node* node) {
    if (!node) {
        return;
    }

    switch (node->kind) {
        case NODE_LITERAL:
            free(node->u.literal.text);
            break;
        case NODE_BINARY:
            node_free(node->u.binary.left);
            node_free(node->u.binary.right);
            break;
        case NODE_PREFIX_LENGTH:
            node_free(node->u.unary.arg);
            break;
        case NODE_PREFIX_MATCH:
        case NODE_PREFIX_INDEX:
            node_free(node->u.pair.a);
            node_free(node->u.pair.b);
            break;
        case NODE_PREFIX_SUBSTR:
            node_free(node->u.triple.a);
            node_free(node->u.triple.b);
            node_free(node->u.triple.c);
            break;
    }

    free(node);
}

static struct expr_node* parse_expr(struct expr_parser* parser, int rbp);

static struct expr_node* parse_nud(struct expr_parser* parser, struct expr_token tok) {
    struct expr_node* a = NULL;
    struct expr_node* b = NULL;
    struct expr_node* c = NULL;
    struct expr_node* node = NULL;

    switch (tok.kind) {
        case TOK_WORD:
            return node_literal(tok.text);

        case TOK_LPAREN:
            node = parse_expr(parser, 0);
            if (!node) {
                return NULL;
            }
            if (!parser_accept(parser, TOK_RPAREN)) {
                parser_set_error(parser, "syntax error: expected ')'");
                node_free(node);
                return NULL;
            }
            return node;

        case TOK_PLUS: {
            const struct expr_token* quoted = parser_consume(parser);
            if (!quoted || quoted->kind == TOK_END) {
                parser_set_error(parser, "syntax error: missing operand after '+'");
                return NULL;
            }
            return node_literal(quoted->text);
        }

        case TOK_KW_LENGTH:
            a = parse_expr(parser, PREFIX_BINDING_POWER);
            if (!a) {
                return NULL;
            }
            node = node_new(NODE_PREFIX_LENGTH);
            node->u.unary.arg = a;
            return node;

        case TOK_KW_MATCH:
            a = parse_expr(parser, PREFIX_BINDING_POWER);
            b = parse_expr(parser, PREFIX_BINDING_POWER);
            if (!a || !b) {
                node_free(a);
                node_free(b);
                return NULL;
            }
            node = node_new(NODE_PREFIX_MATCH);
            node->u.pair.a = a;
            node->u.pair.b = b;
            return node;

        case TOK_KW_INDEX:
            a = parse_expr(parser, PREFIX_BINDING_POWER);
            b = parse_expr(parser, PREFIX_BINDING_POWER);
            if (!a || !b) {
                node_free(a);
                node_free(b);
                return NULL;
            }
            node = node_new(NODE_PREFIX_INDEX);
            node->u.pair.a = a;
            node->u.pair.b = b;
            return node;

        case TOK_KW_SUBSTR:
            a = parse_expr(parser, PREFIX_BINDING_POWER);
            b = parse_expr(parser, PREFIX_BINDING_POWER);
            c = parse_expr(parser, PREFIX_BINDING_POWER);
            if (!a || !b || !c) {
                node_free(a);
                node_free(b);
                node_free(c);
                return NULL;
            }
            node = node_new(NODE_PREFIX_SUBSTR);
            node->u.triple.a = a;
            node->u.triple.b = b;
            node->u.triple.c = c;
            return node;

        default:
            parser_set_error(parser, "syntax error near %s", tok.text ? tok.text : "end of expression");
            return NULL;
    }
}

static struct expr_node* parse_led(struct expr_parser* parser, struct expr_token tok, struct expr_node* left) {
    struct expr_node* right = parse_expr(parser, token_lbp(tok.kind));
    enum expr_binary_op op;

    if (!right) {
        return NULL;
    }

    switch (tok.kind) {
        case TOK_OR:
            op = OP_OR;
            break;
        case TOK_AND:
            op = OP_AND;
            break;
        case TOK_LT:
            op = OP_LT;
            break;
        case TOK_LE:
            op = OP_LE;
            break;
        case TOK_EQ:
            op = OP_EQ;
            break;
        case TOK_NE:
            op = OP_NE;
            break;
        case TOK_GE:
            op = OP_GE;
            break;
        case TOK_GT:
            op = OP_GT;
            break;
        case TOK_PLUS:
            op = OP_ADD;
            break;
        case TOK_MINUS:
            op = OP_SUB;
            break;
        case TOK_MUL:
            op = OP_MUL;
            break;
        case TOK_DIV:
            op = OP_DIV;
            break;
        case TOK_MOD:
            op = OP_MOD;
            break;
        case TOK_COLON:
            op = OP_COLON;
            break;
        default:
            parser_set_error(parser, "syntax error near %s", tok.text ? tok.text : "operator");
            node_free(right);
            return NULL;
    }

    return node_binary(op, left, right);
}

static struct expr_node* parse_expr(struct expr_parser* parser, int rbp) {
    const struct expr_token* tok_ptr = parser_consume(parser);
    if (!tok_ptr || tok_ptr->kind == TOK_END) {
        parser_set_error(parser, "missing operand");
        return NULL;
    }

    struct expr_node* left = parse_nud(parser, *tok_ptr);
    if (!left) {
        return NULL;
    }

    for (;;) {
        const struct expr_token* next = parser_peek(parser);
        if (!next || token_lbp(next->kind) <= rbp) {
            break;
        }

        struct expr_token op = *parser_consume(parser);
        struct expr_node* combined = parse_led(parser, op, left);
        if (!combined) {
            node_free(left);
            return NULL;
        }
        left = combined;
    }

    return left;
}

static struct expr_value value_make_int(intmax_t n) {
    struct expr_value v;
    v.type = VALUE_INT;
    v.i = n;
    v.s = NULL;
    return v;
}

static struct expr_value value_make_string_take(char* s) {
    struct expr_value v;
    v.type = VALUE_STRING;
    v.i = 0;
    v.s = s;
    return v;
}

static struct expr_value value_make_string_dup(const char* s) {
    return value_make_string_take(xstrdup(s ? s : ""));
}

static void value_destroy(struct expr_value* value) {
    if (!value) {
        return;
    }

    if (value->type == VALUE_STRING) {
        free(value->s);
        value->s = NULL;
    }
}

static char* value_to_string_dup(const struct expr_value* value) {
    if (value->type == VALUE_STRING) {
        return xstrdup(value->s ? value->s : "");
    }

    int needed = snprintf(NULL, 0, "%jd", value->i);
    if (needed < 0) {
        return xstrdup("0");
    }
    char* buf = xmalloc((size_t)needed + 1);
    (void)snprintf(buf, (size_t)needed + 1, "%jd", value->i);
    return buf;
}

static bool parse_integer_view(const char* s, struct int_view* out) {
    if (!s || s[0] == '\0') {
        return false;
    }

    const char* p = s;
    int sign = 1;

    if (*p == '-') {
        sign = -1;
        p++;
    }
    else if (*p == '+') {
        return false;
    }

    if (!isdigit((unsigned char)*p)) {
        return false;
    }

    while (*p == '0') {
        p++;
    }

    const char* digits = p;
    while (isdigit((unsigned char)*p)) {
        p++;
    }

    if (*p != '\0') {
        return false;
    }

    size_t len = (size_t)(p - digits);
    if (len == 0) {
        out->sign = 0;
        out->digits = "0";
        out->len = 1;
        return true;
    }

    out->sign = sign;
    out->digits = digits;
    out->len = len;
    return true;
}

static int compare_integer_views(const struct int_view* a, const struct int_view* b) {
    if (a->sign != b->sign) {
        return (a->sign < b->sign) ? -1 : 1;
    }

    if (a->sign == 0) {
        return 0;
    }

    if (a->len != b->len) {
        if (a->sign > 0) {
            return (a->len < b->len) ? -1 : 1;
        }
        return (a->len < b->len) ? 1 : -1;
    }

    int cmp = memcmp(a->digits, b->digits, a->len);
    if (cmp == 0) {
        return 0;
    }

    if (a->sign > 0) {
        return (cmp < 0) ? -1 : 1;
    }
    return (cmp < 0) ? 1 : -1;
}

static bool int_view_from_value(const struct expr_value* value, struct int_view* view, char* scratch, size_t scratch_size) {
    if (value->type == VALUE_INT) {
        (void)snprintf(scratch, scratch_size, "%jd", value->i);
        return parse_integer_view(scratch, view);
    }

    return parse_integer_view(value->s ? value->s : "", view);
}

static enum int_parse_status parse_intmax_string(const char* s, intmax_t* out) {
    struct int_view ignored;
    if (!parse_integer_view(s, &ignored)) {
        return INT_PARSE_INVALID;
    }

    errno = 0;
    char* end = NULL;
    intmax_t v = strtoimax(s, &end, 10);

    if (end == s || !end || *end != '\0') {
        return INT_PARSE_INVALID;
    }
    if (errno == ERANGE) {
        return INT_PARSE_RANGE;
    }

    *out = v;
    return INT_PARSE_OK;
}

static bool value_to_int_for_substr(const struct expr_value* value, intmax_t* out) {
    if (value->type == VALUE_INT) {
        *out = value->i;
        return true;
    }

    return parse_intmax_string(value->s ? value->s : "", out) == INT_PARSE_OK;
}

static bool value_is_null(const struct expr_value* value) {
    if (value->type == VALUE_INT) {
        return value->i == 0;
    }

    const char* s = value->s ? value->s : "";
    if (s[0] == '\0') {
        return true;
    }

    struct int_view view;
    if (parse_integer_view(s, &view)) {
        return view.sign == 0;
    }

    return false;
}

static int compare_values(const struct expr_value* a, const struct expr_value* b) {
    char a_buf[64];
    char b_buf[64];
    struct int_view av;
    struct int_view bv;

    if (int_view_from_value(a, &av, a_buf, sizeof(a_buf)) && int_view_from_value(b, &bv, b_buf, sizeof(b_buf))) {
        return compare_integer_views(&av, &bv);
    }

    char* as = value_to_string_dup(a);
    char* bs = value_to_string_dup(b);
    int cmp = strcoll(as, bs);
    free(as);
    free(bs);

    if (cmp < 0) {
        return -1;
    }
    if (cmp > 0) {
        return 1;
    }
    return 0;
}

static size_t mbs_next_len(const char* s, size_t remaining, mbstate_t* state) {
    if (remaining == 0 || *s == '\0') {
        return 0;
    }

    size_t n = mbrtowc(NULL, s, remaining, state);
    if (n == (size_t)-1 || n == (size_t)-2) {
        memset(state, 0, sizeof(*state));
        return 1;
    }
    if (n == 0) {
        return 0;
    }
    return n;
}

static size_t mbs_count_chars_n(const char* s, size_t bytes) {
    const char* p = s;
    size_t remaining = bytes;
    size_t count = 0;
    mbstate_t state;
    memset(&state, 0, sizeof(state));

    while (remaining > 0) {
        size_t n = mbs_next_len(p, remaining, &state);
        if (n == 0 || n > remaining) {
            break;
        }
        p += n;
        remaining -= n;
        count++;
    }

    return count;
}

static size_t mbs_count_chars(const char* s) {
    return mbs_count_chars_n(s, strlen(s));
}

static bool mbs_char_to_byte_offset(const char* s, size_t char_index, size_t* out_offset) {
    const char* p = s;
    size_t remaining = strlen(s);
    size_t offset = 0;
    size_t index = 0;
    mbstate_t state;
    memset(&state, 0, sizeof(state));

    while (remaining > 0) {
        if (index == char_index) {
            *out_offset = offset;
            return true;
        }

        size_t n = mbs_next_len(p, remaining, &state);
        if (n == 0 || n > remaining) {
            break;
        }

        p += n;
        remaining -= n;
        offset += n;
        index++;
    }

    if (index == char_index) {
        *out_offset = offset;
        return true;
    }

    return false;
}

static char* mbs_substr(const char* s, intmax_t pos, intmax_t len) {
    if (pos <= 0 || len <= 0) {
        return xstrdup("");
    }

    if ((uintmax_t)(pos - 1) > SIZE_MAX || (uintmax_t)len > SIZE_MAX) {
        return xstrdup("");
    }

    size_t start_char = (size_t)(pos - 1);
    size_t length_chars = (size_t)len;

    size_t start_byte = 0;
    if (!mbs_char_to_byte_offset(s, start_char, &start_byte)) {
        return xstrdup("");
    }

    size_t end_byte = 0;
    if (!mbs_char_to_byte_offset(s, start_char + length_chars, &end_byte)) {
        end_byte = strlen(s);
    }

    if (end_byte < start_byte) {
        end_byte = start_byte;
    }

    size_t out_len = end_byte - start_byte;
    char* out = xmalloc(out_len + 1);
    memcpy(out, s + start_byte, out_len);
    out[out_len] = '\0';
    return out;
}

static intmax_t mbs_index_any(const char* s, const char* chars) {
    if (!chars || chars[0] == '\0') {
        return 0;
    }

    size_t chars_bytes = strlen(chars);
    struct char_span* needles = xmalloc((chars_bytes + 1) * sizeof(*needles));
    size_t needle_count = 0;

    const char* np = chars;
    size_t n_remaining = chars_bytes;
    mbstate_t n_state;
    memset(&n_state, 0, sizeof(n_state));

    while (n_remaining > 0) {
        size_t n = mbs_next_len(np, n_remaining, &n_state);
        if (n == 0 || n > n_remaining) {
            break;
        }

        needles[needle_count].ptr = np;
        needles[needle_count].len = n;
        needle_count++;

        np += n;
        n_remaining -= n;
    }

    if (needle_count == 0) {
        free(needles);
        return 0;
    }

    const char* hp = s;
    size_t h_remaining = strlen(s);
    mbstate_t h_state;
    memset(&h_state, 0, sizeof(h_state));
    intmax_t position = 1;

    while (h_remaining > 0) {
        size_t h_len = mbs_next_len(hp, h_remaining, &h_state);
        if (h_len == 0 || h_len > h_remaining) {
            break;
        }

        for (size_t i = 0; i < needle_count; i++) {
            if (needles[i].len == h_len && memcmp(needles[i].ptr, hp, h_len) == 0) {
                free(needles);
                return position;
            }
        }

        hp += h_len;
        h_remaining -= h_len;
        position++;
    }

    free(needles);
    return 0;
}

static void bigint_free(struct bigint* n) {
    if (!n) {
        return;
    }
    free(n->digits);
    n->digits = NULL;
    n->sign = 0;
    n->len = 0;
}

static void bigint_set_zero(struct bigint* n) {
    n->sign = 0;
    n->digits = xstrdup("0");
    n->len = 1;
}

static void bigint_set_from_digits(struct bigint* n, int sign, char* digits) {
    size_t len = strlen(digits);
    if (len == 1 && digits[0] == '0') {
        sign = 0;
    }
    n->sign = sign;
    n->digits = digits;
    n->len = len;
}

static bool bigint_from_string(const char* s, struct bigint* out) {
    struct int_view view;
    if (!parse_integer_view(s, &view)) {
        return false;
    }

    char* digits = xmalloc(view.len + 1);
    memcpy(digits, view.digits, view.len);
    digits[view.len] = '\0';
    bigint_set_from_digits(out, view.sign, digits);
    return true;
}

static void bigint_copy(struct bigint* out, const struct bigint* in) {
    out->sign = in->sign;
    out->digits = xstrdup(in->digits);
    out->len = in->len;
}

static int digits_compare_abs(const char* a, size_t a_len, const char* b, size_t b_len) {
    if (a_len != b_len) {
        return (a_len < b_len) ? -1 : 1;
    }

    int cmp = memcmp(a, b, a_len);
    if (cmp < 0) {
        return -1;
    }
    if (cmp > 0) {
        return 1;
    }
    return 0;
}

static char* digits_add_abs(const char* a, size_t a_len, const char* b, size_t b_len) {
    size_t cap = ((a_len > b_len) ? a_len : b_len) + 2;
    char* rev = xmalloc(cap);
    size_t pos = 0;
    int carry = 0;

    while (pos < a_len || pos < b_len || carry != 0) {
        int da = (pos < a_len) ? (a[a_len - 1 - pos] - '0') : 0;
        int db = (pos < b_len) ? (b[b_len - 1 - pos] - '0') : 0;
        int sum = da + db + carry;

        rev[pos] = (char)('0' + (sum % 10));
        carry = sum / 10;
        pos++;
    }

    char* out = xmalloc(pos + 1);
    for (size_t i = 0; i < pos; i++) {
        out[i] = rev[pos - 1 - i];
    }
    out[pos] = '\0';

    free(rev);
    return out;
}

static char* digits_sub_abs(const char* a, size_t a_len, const char* b, size_t b_len) {
    char* rev = xmalloc(a_len + 1);
    size_t pos = 0;
    int borrow = 0;

    while (pos < a_len) {
        int da = a[a_len - 1 - pos] - '0' - borrow;
        int db = (pos < b_len) ? (b[b_len - 1 - pos] - '0') : 0;

        if (da < db) {
            da += 10;
            borrow = 1;
        }
        else {
            borrow = 0;
        }

        rev[pos] = (char)('0' + (da - db));
        pos++;
    }

    while (pos > 1 && rev[pos - 1] == '0') {
        pos--;
    }

    char* out = xmalloc(pos + 1);
    for (size_t i = 0; i < pos; i++) {
        out[i] = rev[pos - 1 - i];
    }
    out[pos] = '\0';

    free(rev);
    return out;
}

static char* digits_mul_abs(const char* a, size_t a_len, const char* b, size_t b_len) {
    size_t size = a_len + b_len + 1;
    int* acc = calloc(size, sizeof(*acc));
    if (!acc) {
        bx_pfatal(3, "calloc failure");
    }

    for (size_t i = 0; i < a_len; i++) {
        int da = a[a_len - 1 - i] - '0';
        for (size_t j = 0; j < b_len; j++) {
            int db = b[b_len - 1 - j] - '0';
            acc[i + j] += da * db;
        }
    }

    for (size_t k = 0; k + 1 < size; k++) {
        acc[k + 1] += acc[k] / 10;
        acc[k] %= 10;
    }

    size_t top = size;
    while (top > 1 && acc[top - 1] == 0) {
        top--;
    }

    char* out = xmalloc(top + 1);
    for (size_t i = 0; i < top; i++) {
        out[i] = (char)('0' + acc[top - 1 - i]);
    }
    out[top] = '\0';

    free(acc);
    return out;
}

static char* digits_mul10_add_digit(const char* value, char digit) {
    if (value[0] == '0' && value[1] == '\0') {
        if (digit == '0') {
            return xstrdup("0");
        }
        char* out = xmalloc(2);
        out[0] = digit;
        out[1] = '\0';
        return out;
    }

    size_t len = strlen(value);
    char* out = xmalloc(len + 2);
    memcpy(out, value, len);
    out[len] = digit;
    out[len + 1] = '\0';
    return out;
}

static char* digits_trim_leading_zeros(const char* value) {
    while (value[0] == '0' && value[1] != '\0') {
        value++;
    }
    return xstrdup(value);
}

static void digits_divmod_abs(const char* numerator, const char* denominator, char** quotient_out, char** remainder_out) {
    size_t n_len = strlen(numerator);
    size_t d_len = strlen(denominator);

    if (digits_compare_abs(numerator, n_len, denominator, d_len) < 0) {
        *quotient_out = xstrdup("0");
        *remainder_out = xstrdup(numerator);
        return;
    }

    char* quotient = xmalloc(n_len + 1);
    size_t q_len = 0;
    char* remainder = xstrdup("0");

    for (size_t i = 0; i < n_len; i++) {
        char* extended = digits_mul10_add_digit(remainder, numerator[i]);
        free(remainder);
        remainder = extended;

        int qdigit = 0;
        for (;;) {
            size_t r_len = strlen(remainder);
            int cmp = digits_compare_abs(remainder, r_len, denominator, d_len);
            if (cmp < 0) {
                break;
            }

            char* reduced = digits_sub_abs(remainder, r_len, denominator, d_len);
            free(remainder);
            remainder = reduced;
            qdigit++;
        }

        quotient[q_len++] = (char)('0' + qdigit);
    }

    quotient[q_len] = '\0';
    *quotient_out = digits_trim_leading_zeros(quotient);
    *remainder_out = digits_trim_leading_zeros(remainder);

    free(quotient);
    free(remainder);
}

static void bigint_add(struct bigint* out, const struct bigint* a, const struct bigint* b) {
    if (a->sign == 0) {
        bigint_copy(out, b);
        return;
    }
    if (b->sign == 0) {
        bigint_copy(out, a);
        return;
    }

    if (a->sign == b->sign) {
        char* digits = digits_add_abs(a->digits, a->len, b->digits, b->len);
        bigint_set_from_digits(out, a->sign, digits);
        return;
    }

    int cmp = digits_compare_abs(a->digits, a->len, b->digits, b->len);
    if (cmp == 0) {
        bigint_set_zero(out);
        return;
    }
    if (cmp > 0) {
        char* digits = digits_sub_abs(a->digits, a->len, b->digits, b->len);
        bigint_set_from_digits(out, a->sign, digits);
        return;
    }

    char* digits = digits_sub_abs(b->digits, b->len, a->digits, a->len);
    bigint_set_from_digits(out, b->sign, digits);
}

static void bigint_sub(struct bigint* out, const struct bigint* a, const struct bigint* b) {
    struct bigint neg_b = *b;
    neg_b.sign = -neg_b.sign;
    bigint_add(out, a, &neg_b);
}

static void bigint_mul(struct bigint* out, const struct bigint* a, const struct bigint* b) {
    if (a->sign == 0 || b->sign == 0) {
        bigint_set_zero(out);
        return;
    }

    char* digits = digits_mul_abs(a->digits, a->len, b->digits, b->len);
    bigint_set_from_digits(out, a->sign * b->sign, digits);
}

static bool bigint_divmod(struct bigint* quotient, struct bigint* remainder, const struct bigint* a, const struct bigint* b) {
    if (b->sign == 0) {
        return false;
    }

    if (a->sign == 0) {
        bigint_set_zero(quotient);
        bigint_set_zero(remainder);
        return true;
    }

    char* q_digits = NULL;
    char* r_digits = NULL;
    digits_divmod_abs(a->digits, b->digits, &q_digits, &r_digits);

    bigint_set_from_digits(quotient, a->sign * b->sign, q_digits);
    bigint_set_from_digits(remainder, a->sign, r_digits);
    return true;
}

static char* bigint_to_string(const struct bigint* n) {
    if (n->sign < 0) {
        char* out = xmalloc(n->len + 2);
        out[0] = '-';
        memcpy(out + 1, n->digits, n->len + 1);
        return out;
    }
    return xstrdup(n->digits);
}

static struct expr_value value_from_bigint(const struct bigint* n) {
    char* text = bigint_to_string(n);
    intmax_t parsed = 0;

    if (parse_intmax_string(text, &parsed) == INT_PARSE_OK) {
        free(text);
        return value_make_int(parsed);
    }

    return value_make_string_take(text);
}

static bool value_to_bigint_required(const struct expr_value* value, struct expr_eval_ctx* ctx, struct bigint* out) {
    if (value->type == VALUE_INT) {
        char buf[64];
        (void)snprintf(buf, sizeof(buf), "%jd", value->i);
        return bigint_from_string(buf, out);
    }

    if (!bigint_from_string(value->s ? value->s : "", out)) {
        eval_set_error(ctx, "non-integer argument");
        return false;
    }
    return true;
}

static bool regex_set_error(int rc, regex_t* re, struct expr_eval_ctx* ctx, const char* prefix) {
    size_t needed = regerror(rc, re, NULL, 0);
    char* buf = xmalloc(needed > 0 ? needed : 1);
    (void)regerror(rc, re, buf, needed > 0 ? needed : 1);
    eval_set_error(ctx, "%s: %s", prefix, buf);
    free(buf);
    return false;
}

static bool eval_regex_match(const char* input, const char* pattern, struct expr_eval_ctx* ctx, struct expr_value* out) {
    size_t pattern_len = strlen(pattern);
    char* anchored = xmalloc(pattern_len + 2);
    anchored[0] = '^';
    memcpy(anchored + 1, pattern, pattern_len + 1);

    regex_t re;
    memset(&re, 0, sizeof(re));
    int rc = regcomp(&re, anchored, 0);
    free(anchored);
    if (rc != 0) {
        return regex_set_error(rc, &re, ctx, "invalid regular expression");
    }

    size_t nmatch = re.re_nsub > 0 ? re.re_nsub + 1 : 1;
    regmatch_t* matches = xmalloc(nmatch * sizeof(*matches));

    rc = regexec(&re, input, nmatch, matches, 0);
    if (rc == REG_NOMATCH) {
        if (re.re_nsub > 0) {
            *out = value_make_string_dup("");
        }
        else {
            *out = value_make_int(0);
        }
        free(matches);
        regfree(&re);
        return true;
    }
    if (rc != 0) {
        bool ok = regex_set_error(rc, &re, ctx, "regular expression error");
        free(matches);
        regfree(&re);
        return ok;
    }

    if (re.re_nsub > 0) {
        regmatch_t cap = matches[1];
        if (cap.rm_so < 0 || cap.rm_eo < cap.rm_so) {
            *out = value_make_string_dup("");
        }
        else {
            size_t len = (size_t)(cap.rm_eo - cap.rm_so);
            char* captured = xmalloc(len + 1);
            memcpy(captured, input + cap.rm_so, len);
            captured[len] = '\0';
            *out = value_make_string_take(captured);
        }
    }
    else {
        regmatch_t whole = matches[0];
        size_t start = (whole.rm_so < 0) ? 0 : (size_t)whole.rm_so;
        size_t end = (whole.rm_eo < 0 || whole.rm_eo < whole.rm_so) ? start : (size_t)whole.rm_eo;
        size_t bytes = end - start;
        size_t chars = mbs_count_chars_n(input + start, bytes);

        if (chars > (size_t)INTMAX_MAX) {
            eval_set_error(ctx, "result out of range");
            free(matches);
            regfree(&re);
            return false;
        }

        *out = value_make_int((intmax_t)chars);
    }

    free(matches);
    regfree(&re);
    return true;
}

static bool eval_node(const struct expr_node* node, struct expr_eval_ctx* ctx, struct expr_value* out);

static bool eval_binary_op(const struct expr_node* node, struct expr_eval_ctx* ctx, struct expr_value* out) {
    struct expr_value left;
    struct expr_value right;
    bool ok = false;

    if (node->u.binary.op == OP_OR) {
        if (!eval_node(node->u.binary.left, ctx, &left)) {
            return false;
        }
        if (!value_is_null(&left)) {
            *out = left;
            return true;
        }

        value_destroy(&left);

        if (!eval_node(node->u.binary.right, ctx, &right)) {
            return false;
        }
        if (value_is_null(&right)) {
            value_destroy(&right);
            *out = value_make_int(0);
            return true;
        }

        *out = right;
        return true;
    }

    if (node->u.binary.op == OP_AND) {
        if (!eval_node(node->u.binary.left, ctx, &left)) {
            return false;
        }
        if (value_is_null(&left)) {
            value_destroy(&left);
            *out = value_make_int(0);
            return true;
        }

        if (!eval_node(node->u.binary.right, ctx, &right)) {
            value_destroy(&left);
            return false;
        }

        if (value_is_null(&right)) {
            value_destroy(&left);
            value_destroy(&right);
            *out = value_make_int(0);
            return true;
        }

        value_destroy(&right);
        *out = left;
        return true;
    }

    if (!eval_node(node->u.binary.left, ctx, &left)) {
        return false;
    }
    if (!eval_node(node->u.binary.right, ctx, &right)) {
        value_destroy(&left);
        return false;
    }

    switch (node->u.binary.op) {
        case OP_LT:
        case OP_LE:
        case OP_EQ:
        case OP_NE:
        case OP_GE:
        case OP_GT: {
            int cmp = compare_values(&left, &right);
            intmax_t result = 0;

            switch (node->u.binary.op) {
                case OP_LT:
                    result = cmp < 0;
                    break;
                case OP_LE:
                    result = cmp <= 0;
                    break;
                case OP_EQ:
                    result = cmp == 0;
                    break;
                case OP_NE:
                    result = cmp != 0;
                    break;
                case OP_GE:
                    result = cmp >= 0;
                    break;
                case OP_GT:
                    result = cmp > 0;
                    break;
                default:
                    break;
            }

            *out = value_make_int(result);
            ok = true;
            break;
        }

        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
        case OP_DIV:
        case OP_MOD: {
            struct bigint lnum = {0, NULL, 0};
            struct bigint rnum = {0, NULL, 0};
            struct bigint tmp = {0, NULL, 0};
            struct bigint quotient = {0, NULL, 0};
            struct bigint remainder = {0, NULL, 0};

            if (!value_to_bigint_required(&left, ctx, &lnum) || !value_to_bigint_required(&right, ctx, &rnum)) {
                bigint_free(&lnum);
                bigint_free(&rnum);
                ok = false;
                break;
            }

            if (node->u.binary.op == OP_ADD) {
                bigint_add(&tmp, &lnum, &rnum);
                *out = value_from_bigint(&tmp);
                bigint_free(&tmp);
                ok = true;
            }
            else if (node->u.binary.op == OP_SUB) {
                bigint_sub(&tmp, &lnum, &rnum);
                *out = value_from_bigint(&tmp);
                bigint_free(&tmp);
                ok = true;
            }
            else if (node->u.binary.op == OP_MUL) {
                bigint_mul(&tmp, &lnum, &rnum);
                *out = value_from_bigint(&tmp);
                bigint_free(&tmp);
                ok = true;
            }
            else {
                if (rnum.sign == 0) {
                    eval_set_error(ctx, "division by zero");
                    bigint_free(&lnum);
                    bigint_free(&rnum);
                    ok = false;
                    break;
                }

                if (!bigint_divmod(&quotient, &remainder, &lnum, &rnum)) {
                    eval_set_error(ctx, "division by zero");
                    bigint_free(&lnum);
                    bigint_free(&rnum);
                    ok = false;
                    break;
                }

                if (node->u.binary.op == OP_DIV) {
                    *out = value_from_bigint(&quotient);
                }
                else {
                    *out = value_from_bigint(&remainder);
                }
                bigint_free(&quotient);
                bigint_free(&remainder);
                ok = true;
            }

            bigint_free(&lnum);
            bigint_free(&rnum);
            break;
        }

        case OP_COLON: {
            char* lhs = value_to_string_dup(&left);
            char* rhs = value_to_string_dup(&right);
            ok = eval_regex_match(lhs, rhs, ctx, out);
            free(lhs);
            free(rhs);
            break;
        }

        case OP_OR:
        case OP_AND:
            ok = false;
            break;
    }

    value_destroy(&left);
    value_destroy(&right);
    return ok;
}

static bool eval_length_node(const struct expr_node* node, struct expr_eval_ctx* ctx, struct expr_value* out) {
    struct expr_value arg;
    if (!eval_node(node->u.unary.arg, ctx, &arg)) {
        return false;
    }

    char* s = value_to_string_dup(&arg);
    value_destroy(&arg);

    size_t count = mbs_count_chars(s);
    free(s);

    if (count > (size_t)INTMAX_MAX) {
        eval_set_error(ctx, "result out of range");
        return false;
    }

    *out = value_make_int((intmax_t)count);
    return true;
}

static bool eval_match_node(const struct expr_node* node, struct expr_eval_ctx* ctx, struct expr_value* out) {
    struct expr_value lhs;
    struct expr_value rhs;

    if (!eval_node(node->u.pair.a, ctx, &lhs)) {
        return false;
    }
    if (!eval_node(node->u.pair.b, ctx, &rhs)) {
        value_destroy(&lhs);
        return false;
    }

    char* left_s = value_to_string_dup(&lhs);
    char* right_s = value_to_string_dup(&rhs);
    value_destroy(&lhs);
    value_destroy(&rhs);

    bool ok = eval_regex_match(left_s, right_s, ctx, out);
    free(left_s);
    free(right_s);
    return ok;
}

static bool eval_index_node(const struct expr_node* node, struct expr_eval_ctx* ctx, struct expr_value* out) {
    struct expr_value lhs;
    struct expr_value rhs;

    if (!eval_node(node->u.pair.a, ctx, &lhs)) {
        return false;
    }
    if (!eval_node(node->u.pair.b, ctx, &rhs)) {
        value_destroy(&lhs);
        return false;
    }

    char* left_s = value_to_string_dup(&lhs);
    char* right_s = value_to_string_dup(&rhs);
    value_destroy(&lhs);
    value_destroy(&rhs);

    *out = value_make_int(mbs_index_any(left_s, right_s));
    free(left_s);
    free(right_s);
    return true;
}

static bool eval_substr_node(const struct expr_node* node, struct expr_eval_ctx* ctx, struct expr_value* out) {
    struct expr_value sv;
    struct expr_value pv;
    struct expr_value lv;

    if (!eval_node(node->u.triple.a, ctx, &sv)) {
        return false;
    }
    if (!eval_node(node->u.triple.b, ctx, &pv)) {
        value_destroy(&sv);
        return false;
    }
    if (!eval_node(node->u.triple.c, ctx, &lv)) {
        value_destroy(&sv);
        value_destroy(&pv);
        return false;
    }

    char* source = value_to_string_dup(&sv);
    intmax_t pos = 0;
    intmax_t len = 0;
    bool pos_ok = value_to_int_for_substr(&pv, &pos);
    bool len_ok = value_to_int_for_substr(&lv, &len);

    value_destroy(&sv);
    value_destroy(&pv);
    value_destroy(&lv);

    if (!pos_ok || !len_ok || pos <= 0 || len <= 0) {
        free(source);
        *out = value_make_string_dup("");
        return true;
    }

    char* slice = mbs_substr(source, pos, len);
    free(source);
    *out = value_make_string_take(slice);
    return true;
}

static bool eval_node(const struct expr_node* node, struct expr_eval_ctx* ctx, struct expr_value* out) {
    switch (node->kind) {
        case NODE_LITERAL:
            *out = value_make_string_dup(node->u.literal.text);
            return true;
        case NODE_BINARY:
            return eval_binary_op(node, ctx, out);
        case NODE_PREFIX_LENGTH:
            return eval_length_node(node, ctx, out);
        case NODE_PREFIX_MATCH:
            return eval_match_node(node, ctx, out);
        case NODE_PREFIX_INDEX:
            return eval_index_node(node, ctx, out);
        case NODE_PREFIX_SUBSTR:
            return eval_substr_node(node, ctx, out);
    }

    eval_set_error(ctx, "internal evaluator error");
    return false;
}

static const char* expr_progname(const char* argv0) {
    return (argv0 && argv0[0] != '\0') ? argv0 : "expr";
}

static void expr_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "usage: %s EXPRESSION\n", progname);
    fprintf(stream, "   or: %s OPTION\n", progname);
    fprintf(stream, "\n");
    fprintf(stream, "Evaluate EXPRESSION and print the result.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  --help        display this help and exit\n");
    fprintf(stream, "  --version     output version information and exit\n");
}

static void expr_print_version(void) {
    printf("bx expr version %s\n", BX_VERSION);
}

int bx_expr_main(int argc, char** argv) {
    (void)setlocale(LC_ALL, "");

    const char* progname = expr_progname((argc > 0) ? argv[0] : NULL);

    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        expr_print_help(stdout, progname);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        expr_print_version();
        return 0;
    }

    int argi = 1;
    if (argc > 1 && strcmp(argv[1], "--") == 0) {
        argi = 2;
    }

    int expr_argc = argc - argi;
    char** expr_argv = argv + argi;

    if (expr_argc <= 0) {
        fprintf(stderr, "%s: missing operand\n", progname);
        return 2;
    }

    size_t token_count = 0;
    struct expr_token* tokens = tokenize_args(expr_argc, expr_argv, &token_count);

    struct expr_parser parser;
    parser.tokens = tokens;
    parser.count = token_count;
    parser.pos = 0;
    parser.error = NULL;

    struct expr_node* root = parse_expr(&parser, 0);
    if (root && parser_peek(&parser) && parser_peek(&parser)->kind != TOK_END) {
        parser_set_error(&parser, "syntax error: unexpected argument '%s'", parser_peek(&parser)->text ? parser_peek(&parser)->text : "");
        node_free(root);
        root = NULL;
    }

    free(tokens);

    if (!root) {
        fprintf(stderr, "%s: %s\n", progname, parser.error ? parser.error : "syntax error");
        free(parser.error);
        return 2;
    }

    struct expr_eval_ctx eval_ctx;
    eval_ctx.error = NULL;

    struct expr_value result;
    if (!eval_node(root, &eval_ctx, &result)) {
        fprintf(stderr, "%s: %s\n", progname, eval_ctx.error ? eval_ctx.error : "evaluation error");
        free(eval_ctx.error);
        node_free(root);
        return 2;
    }

    node_free(root);

    if (result.type == VALUE_INT) {
        printf("%jd\n", result.i);
    }
    else {
        printf("%s\n", result.s ? result.s : "");
    }

    int exit_code = value_is_null(&result) ? 1 : 0;
    value_destroy(&result);
    free(eval_ctx.error);
    return exit_code;
}
