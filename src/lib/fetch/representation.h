#ifndef BX_FETCH_REPRESENTATION_H
#define BX_FETCH_REPRESENTATION_H

/* BX_FETCH_HEADER_OWNER: policy */
/* BX_FETCH_HEADER_CONSUMERS: policy, net, runtime, core, applet */
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    BX_FETCH_EXPECT_ANY = 0,
    BX_FETCH_EXPECT_JSON,
    BX_FETCH_EXPECT_ARCHIVE,
} BxFetchExpectedRepresentation;

bool bx_fetch_representation_matches(BxFetchExpectedRepresentation expected,
                                    const char* type, const char* prefix, size_t length);

typedef enum {
    BX_FETCH_REPRESENTATION_UNSUPPORTED = 0,
    BX_FETCH_REPRESENTATION_TEXT,
    BX_FETCH_REPRESENTATION_HTML,
} BxFetchTextRepresentation;

/* Missing or unrecognized MIME types require an explicit raw-output policy. */
BxFetchTextRepresentation bx_fetch_text_representation(const char* content_type);

#endif
