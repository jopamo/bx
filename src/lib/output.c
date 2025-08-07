#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "output.h"

static int depth = 0;
static bool first_item = true;

static void indent(void) {
    for (int i = 0; i < depth; i++) fputs("  ", stdout);
}

static void comma(void) {
    if (!first_item) putchar(',');
    first_item = false;
}

void bx_json_begin(void) { depth = 0; first_item = true; }
void bx_json_end(void)   { putchar('\n'); }

void bx_json_object_begin(void) {
    if (depth > 0) putchar('\n');
    indent(); putchar('{'); putchar('\n');
    depth++; first_item = true;
}

void bx_json_object_end(void) {
    depth--; putchar('\n');
    indent(); putchar('}');
}

void bx_json_array_begin(const char *key) {
    comma(); putchar('\n');
    indent(); printf("\"%s\": [", key);
    depth++; first_item = true;
}

void bx_json_array_end(void) {
    depth--;
}

void bx_json_string(const char *key, const char *value) {
    comma(); putchar('\n');
    indent(); printf("\"%s\": \"", key);
    for (const char *c = value; *c; c++) {
        switch (*c) {
        case '"':  fputs("\\\"", stdout); break;
        case '\\': fputs("\\\\", stdout); break;
        case '\n': fputs("\\n", stdout); break;
        case '\r': fputs("\\r", stdout); break;
        case '\t': fputs("\\t", stdout); break;
        default:   putchar(*c); break;
        }
    }
    putchar('"');
}

void bx_json_number(const char *key, long long value) {
    comma(); putchar('\n');
    indent(); printf("\"%s\": %lld", key, value);
}

void bx_json_bool(const char *key, bool value) {
    comma(); putchar('\n');
    indent(); printf("\"%s\": %s", key, value ? "true" : "false");
}

void bx_json_null(const char *key) {
    comma(); putchar('\n');
    indent(); printf("\"%s\": null", key);
}

/* search-specific helpers */

void bx_json_match_begin(void) {
    comma(); putchar('\n');
    indent(); putchar('{');
    depth++; first_item = true;
}

void bx_json_match_path(const char *path) {
    bx_json_string("path", path);
}

void bx_json_match_line_number(size_t n) {
    bx_json_number("line_number", (long long)n);
}

void bx_json_match_byte_offset(size_t n) {
    bx_json_number("byte_offset", (long long)n);
}

void bx_json_match_text(const char *text, size_t len) {
    comma(); putchar('\n');
    indent(); fputs("\"lines\": \"", stdout);
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        switch (c) {
        case '"':  fputs("\\\"", stdout); break;
        case '\\': fputs("\\\\", stdout); break;
        case '\n': fputs("\\n", stdout); break;
        case '\r': fputs("\\r", stdout); break;
        case '\t': fputs("\\t", stdout); break;
        default:   putchar(c); break;
        }
    }
    putchar('"');
}

void bx_json_match_end(void) {
    depth--;
    putchar('\n'); indent(); putchar('}');
}
