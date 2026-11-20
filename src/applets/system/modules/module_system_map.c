/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "module_system_map.h"

#ifdef __linux__
#include <string.h>

static bool space(char ch) {
    return ch == ' ' || ch == '\t';
}

static bool hex_digit(char ch) {
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
           (ch >= 'A' && ch <= 'F');
}

bool bx_module_system_map_exports(const char* data, size_t length,
                                  bx_module_elf_symbol_fn visit, void* user) {
    static const char prefix[] = "__ksymtab_";
    enum { MAP_BYTES_MAX = 64 * 1024 * 1024, SYMBOL_NAME_MAX = 4096 };
    if (!data || !visit || length == 0 || length > MAP_BYTES_MAX)
        return false;
    const char* limit = data + length;
    bool records = false;
    for (const char* line = data; line < limit;) {
        const char* end = memchr(line, '\n', (size_t)(limit - line));
        if (!end)
            end = limit;
        const char* p = line;
        line = end < limit ? end + 1 : end;
        if (end > p && end[-1] == '\r')
            end--;
        while (p < end && space(*p))
            p++;
        if (p == end)
            continue;
        const char* address = p;
        while (p < end && hex_digit(*p))
            p++;
        if (p == address || p - address > 16 || p == end || !space(*p))
            return false;
        while (p < end && space(*p))
            p++;
        if (end - p < 2 || !space(p[1]) ||
            !((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '?'))
            return false;
        char type = *p++;
        while (p < end && space(*p))
            p++;
        const char* name = p;
        while (p < end && *p >= '!' && *p <= '~')
            p++;
        size_t size = (size_t)(p - name);
        while (p < end && space(*p))
            p++;
        if (!size || size > SYMBOL_NAME_MAX || p != end)
            return false;
        records = true;
        if (size >= sizeof(prefix) - 1 && memcmp(name, prefix, sizeof(prefix) - 1) == 0) {
            if (size == sizeof(prefix) - 1)
                return false;
            if ((type == 'r' || type == 'R') &&
                !visit(name + sizeof(prefix) - 1, size - (sizeof(prefix) - 1),
                       BX_MODULE_ELF_EXPORT, user))
                return false;
        }
    }
    return records;
}
#endif
