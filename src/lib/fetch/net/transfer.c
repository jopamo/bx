#define _GNU_SOURCE
#include "transfer.h"
#include <stdlib.h>
#include <string.h>

BxFetchTransfer* bx_fetch_transfer_new(BxFetchRequest* req, BxFetchWriter* writer) {
    BxFetchTransfer* t = calloc(1, sizeof(BxFetchTransfer));
    if (!t)
        return NULL;

    t->req = req;
    t->writer = writer;
    t->state = BX_FETCH_TRANSFER_STATE_INIT;
    t->resp = bx_fetch_response_new();

    if (!t->resp) {
        free(t);
        return NULL;
    }

    t->easy = curl_easy_init();
    if (!t->easy) {
        bx_fetch_response_free(t->resp);
        free(t);
        return NULL;
    }

    if (req && req->target) {
        t->current_target = bx_fetch_prepared_url_clone(req->target);
        if (!t->current_target) {
            curl_easy_cleanup(t->easy);
            bx_fetch_response_free(t->resp);
            free(t);
            return NULL;
        }
    }

    return t;
}

void bx_fetch_transfer_free(BxFetchTransfer* t) {
    if (!t)
        return;

    if (t->easy) {
        curl_easy_cleanup(t->easy);
    }

    if (t->headers) {
        curl_slist_free_all(t->headers);
    }

    if (t->resp) {
        bx_fetch_response_free(t->resp);
    }

    if (t->req) {
        bx_fetch_request_free(t->req);
    }

    free(t->save_headers_buf);
    bx_fetch_prepared_url_free(t->current_target);
    bx_fetch_prepared_url_free(t->pending_redirect_target);

    free(t);
}
