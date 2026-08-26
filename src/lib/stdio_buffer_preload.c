#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "lib/size_parse.h"

static bool bx_stdio_buffer_parse_decimal(const char *text, size_t *size_out) {
    while (isspace((unsigned char)*text)) {
        text++;
    }
    if (*text == '+') {
        text++;
    }

    uintmax_t value = 0;
    if (!bx_size_parse_uint(text, &value) || value > SIZE_MAX) {
        return false;
    }

    *size_out = (size_t)value;
    return true;
}

static void bx_stdio_buffer_apply(FILE *stream, const char *stream_name, const char *env_name) {
    const char *mode = getenv(env_name);
    if (mode == NULL) {
        return;
    }

    int setvbuf_mode;
    size_t size = 0u;
    char *buffer = NULL;

    if (mode[0] == '0') {
        setvbuf_mode = _IONBF;
    }
    else if (mode[0] == 'L') {
        setvbuf_mode = _IOLBF;
    }
    else {
        setvbuf_mode = _IOFBF;
        if (!bx_stdio_buffer_parse_decimal(mode, &size) || size == 0u) {
            fprintf(stderr, "invalid buffering mode %s for %s\n", mode, stream_name);
            return;
        }

        if (size <= SIZE_MAX / 2u) {
            buffer = malloc(size);
        }
        if (buffer == NULL) {
            fprintf(stderr, "failed to allocate a %zu byte stdio buffer\n", size);
            return;
        }
    }

    if (setvbuf(stream, buffer, setvbuf_mode, size) != 0) {
        fprintf(stderr, "could not set buffering of %s to mode %s\n", stream_name, mode);
        free(buffer);
    }
}

static void __attribute__((constructor)) bx_stdio_buffer_preload(void) {
    bx_stdio_buffer_apply(stderr, "stderr", "_STDBUF_E");
    bx_stdio_buffer_apply(stdin, "stdin", "_STDBUF_I");
    bx_stdio_buffer_apply(stdout, "stdout", "_STDBUF_O");
}
