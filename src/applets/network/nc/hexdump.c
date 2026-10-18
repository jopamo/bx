#include "netcat.h"
#include <stdio.h>
#include <ctype.h>

int hexdump(FILE* fp, const char* prefix, const unsigned char* buf, size_t len, size_t total) {
    size_t i, j;
    for (i = 0; i < len; i += 16) {
        if (fprintf(fp, "%s %08zx ", prefix, total + i) < 0)
            return -1;
        for (j = 0; j < 16; j++) {
            if (i + j < len) {
                if (fprintf(fp, "%02x ", buf[i + j]) < 0)
                    return -1;
            }
            else if (fputs("   ", fp) == EOF)
                return -1;
            if (j == 7 && fputc(' ', fp) == EOF)
                return -1;
        }
        if (fputs(" |", fp) == EOF)
            return -1;
        for (j = 0; j < 16; j++) {
            int c = i + j < len ? (isprint(buf[i + j]) ? buf[i + j] : '.') : ' ';
            if (fputc(c, fp) == EOF)
                return -1;
        }
        if (fputs("|\n", fp) == EOF)
            return -1;
    }
    return fflush(fp) == EOF ? -1 : 0;
}
