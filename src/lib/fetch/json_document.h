#ifndef BX_FETCH_JSON_DOCUMENT_H
#define BX_FETCH_JSON_DOCUMENT_H

/* BX_FETCH_HEADER_OWNER: core */
/* BX_FETCH_HEADER_CONSUMERS: core, applet */
#include "vendor/jq/src/jv.h"
#include <stddef.h>

typedef struct BxFetchJsonDocument BxFetchJsonDocument;
BxFetchJsonDocument* bx_fetch_json_document_new(void);
const char* bx_fetch_json_document_path(const BxFetchJsonDocument* document);
int bx_fetch_json_document_read(const BxFetchJsonDocument* document, size_t max_bytes, jv* value);
int bx_fetch_json_document_publish(const BxFetchJsonDocument* document, size_t max_bytes);
int bx_fetch_json_document_free(BxFetchJsonDocument* document);
/* Consumes value. Returns a fetch exit code; filter failures publish nothing. */
int bx_fetch_json_print(jv value, const char* program, size_t max_bytes);

#endif
