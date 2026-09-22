#include "lib/fetch/representation.h"
#include <stddef.h>
#include <string.h>
#include <strings.h>

bool bx_fetch_representation_matches(BxFetchExpectedRepresentation expected,
                                    const char* type, const char* prefix, size_t length) {
    if (expected == BX_FETCH_EXPECT_ANY)
        return true;
    if (bx_fetch_text_representation(type) == BX_FETCH_REPRESENTATION_HTML || !length)
        return false;
    while (length && (*prefix == ' ' || *prefix == '\t' || *prefix == '\r' || *prefix == '\n')) {
        prefix++;
        length--;
    }
    if (!length)
        return false;
    if (expected == BX_FETCH_EXPECT_JSON)
        return strchr("{[\"-0123456789tfn", (unsigned char)*prefix) && !memchr(prefix, '\0', length);
    if (expected != BX_FETCH_EXPECT_ARCHIVE || bx_fetch_text_representation(type) == BX_FETCH_REPRESENTATION_TEXT)
        return false;
    /* Archive formats differ; reject recognizable error documents without
     * claiming to validate archive contents or infer a replacement resource. */
    return !((length >= 5 && strncasecmp(prefix, "<html", 5) == 0) ||
             (length >= 9 && strncasecmp(prefix, "<!doctype", 9) == 0) ||
             (length >= 5 && strncasecmp(prefix, "<?xml", 5) == 0) ||
             *prefix == '{' || *prefix == '[');
}

BxFetchTextRepresentation bx_fetch_text_representation(const char* type) {
    if (!type)
        return BX_FETCH_REPRESENTATION_UNSUPPORTED;
    while (*type == ' ' || *type == '\t')
        type++;
    size_t length = strcspn(type, ";");
    while (length && (type[length - 1] == ' ' || type[length - 1] == '\t'))
        length--;
    if ((length == 9 && strncasecmp(type, "text/html", length) == 0) ||
        (length == 21 && strncasecmp(type, "application/xhtml+xml", length) == 0))
        return BX_FETCH_REPRESENTATION_HTML;
    if (length > 5 && strncasecmp(type, "text/", 5) == 0)
        return BX_FETCH_REPRESENTATION_TEXT;
    if (length > 12 && strncasecmp(type, "application/", 12) == 0) {
        const char* subtype = type + 12;
        size_t size = length - 12;
        if ((size == 4 && strncasecmp(subtype, "json", size) == 0) ||
            (size == 3 && strncasecmp(subtype, "xml", size) == 0) ||
            (size > 5 && strncasecmp(subtype + size - 5, "+json", 5) == 0) ||
            (size > 4 && strncasecmp(subtype + size - 4, "+xml", 4) == 0))
            return BX_FETCH_REPRESENTATION_TEXT;
    }
    return BX_FETCH_REPRESENTATION_UNSUPPORTED;
}
