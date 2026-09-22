#include "repository_json.h"
#include "lib/fetch/exit_code.h"
#include <stdio.h>

int bx_mira_repository_json(struct bx_fetch_config* config, BxFetchBudget* budget, const char* url, jv* value) {
    if (value)
        *value = jv_invalid();
    BxFetchJsonDocument* document = bx_fetch_json_document_new();
    if (!document)
        return BX_FETCH_EXIT_FILE_IO;
    int result = bx_mira_repository_fetch(config, budget, url, bx_fetch_json_document_path(document));
    if (result == 0) {
        result = value ? bx_fetch_json_document_read(document, MIRA_REPOSITORY_JSON_LIMIT, value)
                       : bx_fetch_json_document_publish(document, MIRA_REPOSITORY_JSON_LIMIT);
        if (result == BX_FETCH_EXIT_PROTOCOL || (value && !jv_is_valid(*value)))
            fputs("mira: repository returned invalid JSON\n", stderr);
    }
    if (bx_fetch_json_document_free(document) != 0 && result == 0)
        result = BX_FETCH_EXIT_FILE_IO;
    return result;
}

int bx_mira_repository_print_json(jv value, const char* program) {
    int result = bx_fetch_json_print(value, program, MIRA_REPOSITORY_JSON_LIMIT);
    if (result != 0)
        fputs("mira: repository JSON output failed; no complete result published\n", stderr);
    return result;
}
