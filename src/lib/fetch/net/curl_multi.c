#define _GNU_SOURCE
#include "engine_internal.h"
#include "lib/fetch/html.h"
#include "lib/fetch/resume_validation.h"
#include "lib/fetch/url.h"
#include "lib/time_parse.h"
#include <curl/curl.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>

#define BX_FETCH_MAX_EPOLL_EVENTS 64

static void save_headers_reset(BxFetchTransfer* t) {
    if (!t)
        return;
    t->save_headers_len = 0;
    t->save_headers_written = false;
}

static int save_headers_append(BxFetchTransfer* t, const char* ptr, size_t len) {
    if (!t || !ptr || len == 0)
        return 0;
    if (len > BX_FETCH_RESPONSE_HEADER_BLOCK_MAX_BYTES || t->save_headers_len > BX_FETCH_RESPONSE_HEADER_BLOCK_MAX_BYTES - len) {
        errno = EFBIG;
        return -1;
    }

    size_t need = t->save_headers_len + len;
    if (need > t->save_headers_cap) {
        size_t next_cap = t->save_headers_cap ? t->save_headers_cap : 256;
        while (next_cap < need) {
            if (next_cap > (SIZE_MAX / 2)) {
                next_cap = need;
                break;
            }
            next_cap *= 2;
        }
        char* grown = realloc(t->save_headers_buf, next_cap);
        if (!grown) {
            errno = ENOMEM;
            return -1;
        }
        t->save_headers_buf = grown;
        t->save_headers_cap = next_cap;
    }

    memcpy(t->save_headers_buf + t->save_headers_len, ptr, len);
    t->save_headers_len += len;
    return 0;
}

static size_t reject_response_header(BxFetchTransfer* t, BxFetchResponseHeaderPolicyFailure failure) {
    if (t && t->resp && t->resp->header_policy_failure == BX_FETCH_RESPONSE_HEADER_POLICY_OK) {
        t->resp->header_policy_failure = failure;
    }
    errno = EFBIG;
    return 0;
}

static bool account_response_header_line(BxFetchTransfer* t, size_t line_bytes, bool starts_response) {
    if (!t || !t->resp)
        return false;
    if (line_bytes > BX_FETCH_RESPONSE_HEADER_LINE_MAX_BYTES) {
        reject_response_header(t, BX_FETCH_RESPONSE_HEADER_POLICY_LINE_TOO_LARGE);
        return false;
    }
    if (starts_response)
        t->response_header_bytes = 0;
    if (t->response_header_bytes > BX_FETCH_RESPONSE_HEADER_BLOCK_MAX_BYTES - line_bytes) {
        reject_response_header(t, BX_FETCH_RESPONSE_HEADER_POLICY_BLOCK_TOO_LARGE);
        return false;
    }
    t->response_header_bytes += line_bytes;
    return true;
}

static size_t handle_save_headers_append_failure(BxFetchTransfer* t) {
    if (errno == EFBIG) {
        return reject_response_header(t, BX_FETCH_RESPONSE_HEADER_POLICY_BLOCK_TOO_LARGE);
    }
    bx_fetch_transfer_mark_io_failure(t, ENOMEM);
    return 0;
}

static bool status_is_redirect(int status) {
    return status >= 300 && status < 400 && status != 304;
}

static BxFetchPreparedUrl* resolve_redirect_target(BxFetchTransfer* t, const char* location) {
    if (!t || !location || location[0] == '\0')
        return NULL;

    const BxFetchPreparedUrl* base = t->current_target;
    if (!base && t->req)
        base = bx_fetch_request_target(t->req);
    if (!base)
        return NULL;

    BxFetchPreparedUrl* target = bx_fetch_prepared_url_resolve(base, location);
    if (!target)
        return NULL;

    if (bx_fetch_prepared_url_policy(target, bx_fetch_config_requires_https(t->engine ? t->engine->cfg : NULL)) != BX_FETCH_PROTOCOL_DECISION_ALLOW) {
        t->redirect_protocol_unsupported = true;
        bx_fetch_prepared_url_free(target);
        errno = EPROTONOSUPPORT;
        return NULL;
    }
    t->redirect_target_policy = bx_fetch_net_target_policy(t->engine ? t->engine->cfg : NULL, target);
    if (t->redirect_target_policy != BX_FETCH_NET_TARGET_ALLOWED) {
        bx_fetch_prepared_url_free(target);
        errno = ENOTSUP;
        return NULL;
    }
    return target;
}

static bool bx_fetch_transfer_refresh_effective_url(BxFetchTransfer* t) {
    if (!t || !t->easy || !t->resp)
        return false;

    char* effective_url = NULL;
    if (curl_easy_getinfo(t->easy, CURLINFO_EFFECTIVE_URL, &effective_url) == CURLE_OK && effective_url && effective_url[0] != '\0') {
        BxFetchPreparedUrl* prepared = NULL;
        if (t->current_target && strcmp(effective_url, bx_fetch_prepared_url_transport(t->current_target)) == 0) {
            prepared = bx_fetch_prepared_url_clone(t->current_target);
        }
        else {
            prepared = bx_fetch_url_prepare(effective_url);
        }
        if (!prepared) {
            t->url_canonicalization_failed = true;
            return false;
        }
        bx_fetch_prepared_url_free(t->resp->effective_target);
        t->resp->effective_target = prepared;
        return true;
    }

    if (t->current_target) {
        BxFetchPreparedUrl* prepared = bx_fetch_prepared_url_clone(t->current_target);
        if (prepared) {
            bx_fetch_prepared_url_free(t->resp->effective_target);
            t->resp->effective_target = prepared;
            return true;
        }
    }

    t->url_canonicalization_failed = true;
    return false;
}

static bool transfer_uses_ftp(const BxFetchTransfer* transfer) {
    const BxFetchPreparedUrl* target = transfer->pending_redirect_target ? transfer->pending_redirect_target : transfer->current_target;
    BxFetchProtocol protocol = bx_fetch_prepared_url_protocol(target);
    return protocol == BX_FETCH_PROTOCOL_FTP || protocol == BX_FETCH_PROTOCOL_FTPS;
}

static void advance_redirect_target(BxFetchTransfer* transfer) {
    if (!transfer->pending_redirect_target)
        return;
    bx_fetch_prepared_url_free(transfer->current_target);
    transfer->current_target = transfer->pending_redirect_target;
    transfer->pending_redirect_target = NULL;
}

static void reset_response_state(BxFetchTransfer* transfer) {
    bx_fetch_response_reset_headers(transfer->resp);
    save_headers_reset(transfer);
    transfer->resume_needs_content_range = false;
    transfer->resume_saw_content_range = false;
    transfer->discard_body = false;
    transfer->resume_restart_validation_pending = false;
    transfer->response_body_bytes = 0;
    transfer->transform_source_len = 0;
    transfer->transform_failed = false;
    transfer->progress = (BxFetchProgressSample){.generation = transfer->progress.generation + 1};
    transfer->progress_emitted = false;
    transfer->response_headers_finalized = false;
    transfer->anubis_refresh_detected = false;
}

static bool buffer_transform_source(BxFetchTransfer* transfer, const char* data, size_t length) {
    if (!transfer || (!data && length > 0)) {
        errno = EINVAL;
        return false;
    }
    if (length > BX_FETCH_DOCUMENT_PARSE_MAX_BYTES - transfer->transform_source_len) {
        errno = EFBIG;
        transfer->transform_failed = true;
        bx_fetch_transfer_mark_io_failure(transfer, EFBIG);
        return false;
    }

    size_t needed = transfer->transform_source_len + length;
    if (needed > transfer->transform_source_cap) {
        size_t capacity = transfer->transform_source_cap ? transfer->transform_source_cap : 16384u;
        while (capacity < needed) {
            size_t next = capacity * 2u;
            if (next <= capacity || next > BX_FETCH_DOCUMENT_PARSE_MAX_BYTES) {
                capacity = BX_FETCH_DOCUMENT_PARSE_MAX_BYTES;
                break;
            }
            capacity = next;
        }
        char* grown = realloc(transfer->transform_source, capacity);
        if (!grown) {
            transfer->transform_failed = true;
            bx_fetch_transfer_mark_io_failure(transfer, ENOMEM);
            return false;
        }
        transfer->transform_source = grown;
        transfer->transform_source_cap = capacity;
    }
    if (length > 0)
        memcpy(transfer->transform_source + transfer->transform_source_len, data, length);
    transfer->transform_source_len += length;
    return true;
}

static bool anubis_probe_eligible(const BxFetchTransfer* transfer) {
    if (!transfer || !transfer->engine || !transfer->engine->cfg || !transfer->req ||
        transfer->anubis_phase == BX_FETCH_ANUBIS_PHASE_SUBMIT ||
        transfer->engine->cfg->http.no_cookies || transfer->engine->cfg->download.spider ||
        transfer->resume_requested || bx_fetch_request_has_body_file(transfer->req) ||
        transfer->req->body || !transfer->req->method ||
        strcasecmp(transfer->req->method, "GET") != 0 ||
        transfer->resp->status_code != 200) {
        return false;
    }

    BxFetchProtocol protocol =
        bx_fetch_response_protocol(transfer->resp, bx_fetch_request_target(transfer->req));
    if (protocol != BX_FETCH_PROTOCOL_HTTP && protocol != BX_FETCH_PROTOCOL_HTTPS)
        return false;
    const char* content_type =
        bx_fetch_response_header_value(transfer->resp, "Content-Type");
    return bx_fetch_content_type_equals(content_type, "text/html") ||
           bx_fetch_content_type_equals(content_type, "application/xhtml+xml");
}

static bool finalize_response_headers(BxFetchTransfer* transfer) {
    if (!transfer || transfer->response_headers_finalized)
        return transfer && transfer->response_headers_finalized;
    if (transfer->headers_cb &&
        transfer->headers_cb(transfer->callback_userdata,
                             transfer->req,
                             transfer->resp,
                             transfer->writer) != 0) {
        bx_fetch_transfer_mark_io_failure(transfer, EIO);
        return false;
    }
    transfer->response_headers_finalized = true;
    if (transfer->resume_needs_content_range)
        transfer->progress.accepted_prefix_bytes = (uint64_t)transfer->resume_from;
    return true;
}

static bool write_response_bytes(BxFetchTransfer* transfer,
                                 const char* data,
                                 size_t length) {
    if (!transfer || !transfer->writer || (!data && length > 0))
        return false;
    bool ftp = transfer_uses_ftp(transfer);
    if (!ftp && !finalize_response_headers(transfer))
        return false;
    if (transfer->engine->cfg->http.save_headers &&
        !ftp && !transfer->save_headers_written &&
        transfer->save_headers_len > 0) {
        BxFetchWriterWriteResult result =
            bx_fetch_writer_write(transfer->writer,
                                  transfer->save_headers_buf,
                                  transfer->save_headers_len);
        if (result == BX_FETCH_WRITER_WRITE_DOWNSTREAM_CLOSED) {
            transfer->downstream_closed = true;
            errno = 0;
            return false;
        }
        if (result != BX_FETCH_WRITER_WRITE_OK) {
            bx_fetch_transfer_mark_io_failure(transfer, EIO);
            return false;
        }
        transfer->save_headers_written = true;
    }
    if (length > 0 && transfer->engine->cfg->download.html_to_markdown) {
        if (!buffer_transform_source(transfer, data, length))
            return false;
    }
    else if (length > 0) {
        BxFetchWriterWriteResult result =
            bx_fetch_writer_write(transfer->writer, data, length);
        if (result == BX_FETCH_WRITER_WRITE_DOWNSTREAM_CLOSED) {
            transfer->downstream_closed = true;
            errno = 0;
            return false;
        }
        if (result != BX_FETCH_WRITER_WRITE_OK) {
            bx_fetch_transfer_mark_io_failure(transfer, EIO);
            return false;
        }
    }
    return true;
}

static bool write_markdown_response(BxFetchTransfer* transfer) {
    if (!transfer || !transfer->writer) {
        errno = EINVAL;
        return false;
    }
    const BxFetchPreparedUrl* effective = bx_fetch_response_effective_target(transfer->resp);
    if (!effective)
        effective = bx_fetch_request_target(transfer->req);
    const char* base_url = effective ? bx_fetch_prepared_url_transport(effective) : NULL;
    size_t markdown_length = 0;
    char* markdown = bx_fetch_html_to_markdown(base_url,
                                               transfer->transform_source ? transfer->transform_source : "",
                                               transfer->transform_source_len,
                                               &markdown_length);
    if (!markdown) {
        transfer->transform_failed = true;
        bx_fetch_transfer_mark_io_failure(transfer, errno ? errno : EINVAL);
        return false;
    }

    BxFetchWriterWriteResult result = bx_fetch_writer_write(transfer->writer, markdown, markdown_length);
    int error_number = errno;
    free(markdown);
    if (result == BX_FETCH_WRITER_WRITE_DOWNSTREAM_CLOSED) {
        transfer->downstream_closed = true;
        errno = 0;
        return true;
    }
    if (result != BX_FETCH_WRITER_WRITE_OK) {
        errno = error_number ? error_number : EIO;
        transfer->transform_failed = true;
        bx_fetch_transfer_mark_io_failure(transfer, error_number ? error_number : EIO);
        return false;
    }
    return true;
}

static void set_anubis_error(BxFetchTransfer* transfer,
                             int error_number,
                             const char* detail) {
    if (!transfer || transfer->anubis_error_number != 0)
        return;
    transfer->anubis_error_number = error_number > 0 ? error_number : EPROTO;
    transfer->anubis_error_detail = detail;
    transfer->discard_body = true;
    transfer->anubis_probe_active = false;
}

static BxFetchAnubisProbeResult probe_anubis(BxFetchTransfer* transfer,
                                             bool final) {
    BxFetchAnubisChallenge challenge = {0};
    BxFetchAnubisProbeResult result =
        bx_fetch_anubis_probe(transfer->anubis_probe,
                              transfer->anubis_probe_len,
                              final,
                              &challenge);
    if (result == BX_FETCH_ANUBIS_PROBE_MATCH) {
        int difficulty_limit =
            challenge.algorithm_kind == BX_FETCH_ANUBIS_ALGORITHM_SHA256
                ? BX_FETCH_ANUBIS_SHA256_DEFAULT_MAX_DIFFICULTY
                : BX_FETCH_ANUBIS_DEFAULT_MAX_DIFFICULTY;
        if (challenge.difficulty > difficulty_limit) {
            set_anubis_error(transfer,
                             EFBIG,
                             "Anubis challenge difficulty exceeds the supported limit");
        }
        else if (transfer->anubis_phase == BX_FETCH_ANUBIS_PHASE_RETRY) {
            set_anubis_error(transfer,
                             ELOOP,
                             "Anubis challenge remained after validation");
        }
        else {
            const BxFetchPreparedUrl* effective =
                bx_fetch_response_effective_target(transfer->resp);
            BxFetchPreparedUrl* retry_target =
                bx_fetch_prepared_url_clone(effective ? effective : transfer->current_target);
            if (!retry_target) {
                set_anubis_error(transfer,
                                 ENOMEM,
                                 "could not retain the Anubis challenge target");
                return result;
            }
            bx_fetch_prepared_url_free(transfer->anubis_retry_target);
            transfer->anubis_retry_target = retry_target;
            transfer->anubis_challenge = challenge;
            transfer->anubis_challenge_detected = true;
            transfer->anubis_probe_active = false;
            transfer->discard_body = true;
        }
    }
    else if (result == BX_FETCH_ANUBIS_PROBE_INVALID) {
        set_anubis_error(transfer, EPROTO, "malformed or unsupported Anubis challenge");
    }
    return result;
}

size_t bx_fetch_header_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    BxFetchTransfer* t = userdata;
    if (size != 0 && nmemb > SIZE_MAX / size) {
        return reject_response_header(t, BX_FETCH_RESPONSE_HEADER_POLICY_LINE_TOO_LARGE);
    }
    size_t total = size * nmemb;
    if (!t || total == 0)
        return total;

    /*
     * FTP control replies are bounded observations, not HTTP headers. In
     * particular, server text must not acquire HTTP naming, redirect, or
     * resume authority. FTP staging occurs after the final transfer reply.
     */
    if (transfer_uses_ftp(t)) {
        bool starts_response = t->pending_redirect_target != NULL;
        advance_redirect_target(t);
        if (starts_response)
            reset_response_state(t);
        if (!account_response_header_line(t, total, starts_response))
            return 0;
        if (t->engine && t->engine->observer.on_response_header)
            t->engine->observer.on_response_header(t->engine->observer.userdata, t->req, t->resp, ptr, total);
        return total;
    }

    bool capture_headers = t->engine && t->engine->cfg->http.save_headers && !t->engine->cfg->download.spider;

    if (total > BX_FETCH_RESPONSE_HEADER_LINE_MAX_BYTES) {
        return reject_response_header(t, BX_FETCH_RESPONSE_HEADER_POLICY_LINE_TOO_LARGE);
    }
    char* line = malloc(total + 1);
    if (!line) {
        bx_fetch_transfer_mark_io_failure(t, ENOMEM);
        return 0;
    }
    memcpy(line, ptr, total);
    line[total] = '\0';

    size_t len = total;
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
        line[--len] = '\0';
    }

    int parsed_status = 0;
    bool starts_response = len >= 5 && strncasecmp(line, "HTTP/", 5) == 0 && sscanf(line, "HTTP/%*s %d", &parsed_status) == 1;
    if (!account_response_header_line(t, total, starts_response)) {
        free(line);
        return 0;
    }

    char* colon = len > 0 && !starts_response ? strchr(line, ':') : NULL;
    if (colon && t->resp->header_count >= BX_FETCH_RESPONSE_HEADER_MAX_FIELDS) {
        free(line);
        return reject_response_header(t, BX_FETCH_RESPONSE_HEADER_POLICY_TOO_MANY_FIELDS);
    }

    if (t->engine && t->engine->observer.on_response_header) {
        t->engine->observer.on_response_header(t->engine->observer.userdata, t->req, t->resp, ptr, total);
    }

    if (len == 0) {
        if (capture_headers && t->save_headers_len > 0 && save_headers_append(t, ptr, total) != 0) {
            int append_error = errno;
            free(line);
            errno = append_error;
            return handle_save_headers_append_failure(t);
        }
        if (t->resume_needs_content_range && !t->resume_saw_content_range) {
            t->resume_validation_failed = true;
            free(line);
            return 0;
        }
        if (!t->response_headers_finalized) {
            if (!bx_fetch_transfer_refresh_effective_url(t)) {
                free(line);
                return 0;
            }

            int status = t->resp ? t->resp->status_code : 0;
            bool callback_eligible = status == 304 || ((status == 200 || status == 206) && !t->discard_body);
            if (t->anubis_phase == BX_FETCH_ANUBIS_PHASE_SUBMIT) {
                t->response_headers_finalized = true;
            }
            else if (t->anubis_refresh_detected) {
                int difficulty_limit =
                    t->anubis_challenge.algorithm_kind ==
                            BX_FETCH_ANUBIS_ALGORITHM_SHA256
                        ? BX_FETCH_ANUBIS_SHA256_DEFAULT_MAX_DIFFICULTY
                        : BX_FETCH_ANUBIS_DEFAULT_MAX_DIFFICULTY;
                if (t->anubis_challenge.difficulty > difficulty_limit) {
                    set_anubis_error(
                        t,
                        EFBIG,
                        "Anubis challenge difficulty exceeds the supported limit");
                }
                else if (t->anubis_phase == BX_FETCH_ANUBIS_PHASE_RETRY) {
                    set_anubis_error(
                        t,
                        ELOOP,
                        "Anubis challenge remained after validation");
                }
                else {
                    bx_fetch_prepared_url_free(t->anubis_retry_target);
                    t->anubis_retry_target =
                        bx_fetch_prepared_url_clone(t->resp->effective_target);
                    if (!t->anubis_retry_target) {
                        set_anubis_error(
                            t,
                            ENOMEM,
                            "could not retain the Anubis challenge target");
                    }
                    else {
                        t->anubis_challenge_detected = true;
                        t->discard_body = true;
                    }
                }
                t->response_headers_finalized = true;
            }
            else if (callback_eligible && anubis_probe_eligible(t)) {
                if (!t->anubis_probe) {
                    t->anubis_probe =
                        malloc(BX_FETCH_ANUBIS_PROBE_LIMIT_BYTES + 1u);
                    if (!t->anubis_probe) {
                        bx_fetch_transfer_mark_io_failure(t, ENOMEM);
                        free(line);
                        return 0;
                    }
                }
                t->anubis_probe_len = 0;
                t->anubis_probe_active = true;
            }
            else if (callback_eligible && !finalize_response_headers(t)) {
                free(line);
                return 0;
            }
            else if (!callback_eligible) {
                t->response_headers_finalized = true;
            }
        }
        free(line);
        return total;
    }

    if (starts_response) {
        advance_redirect_target(t);

        int status = parsed_status;
        if (status > 0) {
            t->resp->status_code = status;
            reset_response_state(t);
            if (capture_headers) {
                if (save_headers_append(t, ptr, total) != 0) {
                    int append_error = errno;
                    free(line);
                    errno = append_error;
                    return handle_save_headers_append_failure(t);
                }
            }

            if (t->resume_requested && status >= 200) {
                BxFetchResumeAction action = bx_fetch_resume_action_for_status(status);
                if (action == BX_FETCH_RESUME_ACTION_RESTART) {
                    if (bx_fetch_writer_begin_replace(t->writer) != 0) {
                        bx_fetch_transfer_mark_io_failure(t, EIO);
                        free(line);
                        return 0;
                    }
                    t->resume_restart_validation_pending = true;
                    t->resume_requested = false;
                }
                else if (action == BX_FETCH_RESUME_ACTION_DISCARD) {
                    t->discard_body = true;
                    /* A redirect/auth response does not withdraw Range from
                     * the next request. Keep validation pending for its body. */
                }
                else if (status == 206) {
                    t->resume_needs_content_range = true;
                }
            }
        }

        free(line);
        return total;
    }

    if (capture_headers && t->save_headers_len > 0 && save_headers_append(t, ptr, total) != 0) {
        int append_error = errno;
        free(line);
        errno = append_error;
        return handle_save_headers_append_failure(t);
    }

    if (!colon) {
        free(line);
        return total;
    }

    *colon = '\0';
    char* name = line;
    char* value = colon + 1;
    while (*value == ' ' || *value == '\t')
        value++;

    if (bx_fetch_response_add_header(t->resp, name, value) != 0) {
        if (errno == EFBIG) {
            BxFetchResponseHeaderPolicyFailure failure =
                t->resp->header_count >= BX_FETCH_RESPONSE_HEADER_MAX_FIELDS ? BX_FETCH_RESPONSE_HEADER_POLICY_TOO_MANY_FIELDS : BX_FETCH_RESPONSE_HEADER_POLICY_BLOCK_TOO_LARGE;
            free(line);
            return reject_response_header(t, failure);
        }
        bx_fetch_transfer_mark_io_failure(t, ENOMEM);
        free(line);
        return 0;
    }

    if (t->anubis_phase != BX_FETCH_ANUBIS_PHASE_SUBMIT &&
        !t->engine->cfg->http.no_cookies &&
        t->resp->status_code == 200 &&
        strcasecmp(name, "Refresh") == 0 &&
        strstr(value, BX_FETCH_ANUBIS_PASS_PATH) != NULL) {
        BxFetchAnubisChallenge challenge = {0};
        BxFetchAnubisProbeResult result =
            bx_fetch_anubis_probe_refresh(value, &challenge);
        if (result == BX_FETCH_ANUBIS_PROBE_MATCH) {
            t->anubis_challenge = challenge;
            t->anubis_refresh_detected = true;
        }
        else {
            set_anubis_error(t,
                             EPROTO,
                             "malformed Anubis Refresh challenge");
        }
    }

    if (t->anubis_phase != BX_FETCH_ANUBIS_PHASE_SUBMIT &&
        t->engine && t->engine->cfg->http.max_redirect > 0 &&
        status_is_redirect(t->resp->status_code) &&
        strcasecmp(name, "Location") == 0) {
        BxFetchPreparedUrl* redirect_target = resolve_redirect_target(t, value);
        if (!redirect_target) {
            t->url_canonicalization_failed = true;
            free(line);
            return 0;
        }
        bool redirect_allowed = !t->redirect_cb || t->redirect_cb(t->redirect_userdata, redirect_target);
        /*
         * The core callback is the authoritative policy path and supplies
         * diagnostics. This second check keeps the lower-level net API from
         * following URL credentials when paranoid mode is used without that
         * callback.
         */
        if (redirect_allowed && t->engine->cfg->http.paranoid && bx_fetch_prepared_url_has_userinfo(redirect_target)) {
            redirect_allowed = false;
        }
        if (!redirect_allowed) {
            t->redirect_policy_rejected = true;
            bx_fetch_prepared_url_free(redirect_target);
            free(line);
            return 0;
        }

        bx_fetch_prepared_url_free(t->pending_redirect_target);
        t->pending_redirect_target = redirect_target;
    }

    if (t->resume_needs_content_range && strcasecmp(name, "Content-Range") == 0) {
        BxFetchContentRange range;
        if (bx_fetch_parse_content_range(value, &range) != 0 || range.start != t->resume_from) {
            t->resume_validation_failed = true;
            free(line);
            return 0;
        }
        t->resume_saw_content_range = true;
        t->progress.total_known = range.complete_length_known;
        t->progress.total_bytes = range.complete_length_known ? (uint64_t)range.complete_length : 0;
    }

    free(line);
    return total;
}

size_t bx_fetch_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    BxFetchTransfer* t = userdata;
    if (size != 0 && nmemb > SIZE_MAX / size) {
        errno = EOVERFLOW;
        bx_fetch_transfer_mark_io_failure(t, EOVERFLOW);
        return 0;
    }
    size_t total = size * nmemb;
    if (!t || !t->engine || t->engine->cancelled || !t->writer)
        return 0;
    /* libcurl sends synthetic FTP NOBODY headers to the write callback too. */
    if (t->engine->cfg->download.spider)
        return total;

    if (t->engine && t->engine->rate_limiter.rate_bytes_per_sec > 0) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            return 0;
        }

        double wait_s = bx_fetch_token_bucket_consume(&t->engine->rate_limiter, total, &now);
        if (bx_fetch_sleep_for_seconds(wait_s) != 0) {
            return 0;
        }
    }

    if ((uint64_t)total > (uint64_t)LLONG_MAX - (uint64_t)t->response_body_bytes) {
        errno = EFBIG;
        bx_fetch_transfer_mark_io_failure(t, EFBIG);
        return 0;
    }
    t->response_body_bytes += (curl_off_t)total;

    if (t->anubis_phase == BX_FETCH_ANUBIS_PHASE_SUBMIT || t->discard_body) {
        bx_fetch_record_downloaded_bytes(t, total);
        return total;
    }

    if (t->anubis_probe_active) {
        size_t available = BX_FETCH_ANUBIS_PROBE_LIMIT_BYTES - t->anubis_probe_len;
        size_t buffered = total < available ? total : available;
        if (buffered > 0) {
            memcpy(t->anubis_probe + t->anubis_probe_len, ptr, buffered);
            t->anubis_probe_len += buffered;
            t->anubis_probe[t->anubis_probe_len] = '\0';
        }

        BxFetchAnubisProbeResult probe_result = probe_anubis(t, false);
        if (t->anubis_error_number != 0 || t->anubis_challenge_detected) {
            bx_fetch_record_downloaded_bytes(t, total);
            return total;
        }
        if (probe_result == BX_FETCH_ANUBIS_PROBE_UNDECIDED &&
            t->anubis_probe_len < BX_FETCH_ANUBIS_PROBE_LIMIT_BYTES) {
            bx_fetch_record_downloaded_bytes(t, total);
            return total;
        }

        t->anubis_probe_active = false;
        if (!write_response_bytes(t, t->anubis_probe, t->anubis_probe_len) ||
            (buffered < total &&
             !write_response_bytes(t, ptr + buffered, total - buffered))) {
            return 0;
        }
        t->anubis_probe_len = 0;
        bx_fetch_record_downloaded_bytes(t, total);
        return total;
    }

    if (!write_response_bytes(t, ptr, total))
        return 0;
    bx_fetch_record_downloaded_bytes(t, total);
    return total;
}

static int timer_callback(CURLM* multi, long timeout_ms, void* userdata);
static int socket_callback(CURL* easy, curl_socket_t socket_fd, int action, void* userdata, void* socket_data);

static void engine_cleanup(BxFetchEngine* engine) {
    if (!engine)
        return;
    if (engine->multi)
        curl_multi_cleanup(engine->multi);
    if (engine->epoll_fd >= 0)
        close(engine->epoll_fd);
    if (engine->timer_fd >= 0)
        close(engine->timer_fd);
    free(engine);
}

BxFetchEngine* bx_fetch_engine_new(const struct bx_fetch_config* cfg, const BxFetchTransportObserver* observer) {
    if (!cfg) {
        errno = EINVAL;
        return NULL;
    }

    BxFetchEngine* engine = calloc(1, sizeof(*engine));
    if (!engine)
        return NULL;
    engine->cfg = cfg;
    engine->epoll_fd = -1;
    engine->timer_fd = -1;
    if (observer)
        engine->observer = *observer;

    engine->multi = curl_multi_init();
    if (!engine->multi)
        goto fail;

    engine->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (engine->epoll_fd < 0)
        goto fail;

    engine->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (engine->timer_fd < 0)
        goto fail;

    struct epoll_event timer_event = {
        .events = EPOLLIN,
        .data.fd = engine->timer_fd,
    };
    if (epoll_ctl(engine->epoll_fd, EPOLL_CTL_ADD, engine->timer_fd, &timer_event) != 0) {
        goto fail;
    }

    if (curl_multi_setopt(engine->multi, CURLMOPT_SOCKETFUNCTION, socket_callback) != CURLM_OK || curl_multi_setopt(engine->multi, CURLMOPT_SOCKETDATA, engine) != CURLM_OK ||
        curl_multi_setopt(engine->multi, CURLMOPT_TIMERFUNCTION, timer_callback) != CURLM_OK || curl_multi_setopt(engine->multi, CURLMOPT_TIMERDATA, engine) != CURLM_OK) {
        errno = EIO;
        goto fail;
    }

    engine->quota_limit_bytes = cfg->download.quota;
    engine->quota_exhausted = engine->quota_limit_bytes == 0;
    if (cfg->download.limit_rate_bytes_per_sec > 0) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
            goto fail;
        bx_fetch_token_bucket_init(&engine->rate_limiter, cfg->download.limit_rate_bytes_per_sec, &now);
    }
    return engine;

fail:
    engine_cleanup(engine);
    return NULL;
}

void bx_fetch_engine_cancel(BxFetchEngine* engine) {
    if (!engine || engine->cancelled)
        return;

    engine->cancelled = true;
    while (engine->active_head) {
        BxFetchTransfer* transfer = engine->active_head;
        if (transfer->resp) {
            transfer->resp->error_code = (int)CURLE_ABORTED_BY_CALLBACK;
            transfer->resp->transport_error_kind = BX_FETCH_TRANSPORT_ERROR_NETWORK;
        }
        bx_fetch_engine_dispose_transfer(engine, transfer, BX_FETCH_ERROR_CANCELLED);
    }
}

void bx_fetch_engine_free(BxFetchEngine* engine) {
    if (!engine)
        return;
    bx_fetch_engine_cancel(engine);
    engine_cleanup(engine);
}

static int timer_callback(CURLM* multi, long timeout_ms, void* userdata) {
    BxFetchEngine* engine = userdata;
    (void)multi;
    if (!engine)
        return -1;

    struct itimerspec timer = {0};
    if (timeout_ms > 0) {
        if (!bx_time_milliseconds_to_timespec(timeout_ms, &timer.it_value)) {
            engine->invariant_failed = true;
            errno = EOVERFLOW;
            return -1;
        }
    }
    else if (timeout_ms == 0) {
        /* A zero it_value disarms timerfd, so use its smallest delay. */
        timer.it_value.tv_nsec = 1;
    }

    if (timerfd_settime(engine->timer_fd, 0, &timer, NULL) != 0) {
        engine->invariant_failed = true;
        return -1;
    }
    return 0;
}

static int socket_callback(CURL* easy, curl_socket_t socket_fd, int action, void* userdata, void* socket_data) {
    BxFetchEngine* engine = userdata;
    (void)easy;
    (void)socket_data;
    if (!engine)
        return -1;

    if (action == CURL_POLL_REMOVE) {
        if (epoll_ctl(engine->epoll_fd, EPOLL_CTL_DEL, socket_fd, NULL) != 0 && errno != ENOENT && errno != EBADF) {
            engine->invariant_failed = true;
            return -1;
        }
        return 0;
    }

    uint32_t events = EPOLLERR | EPOLLHUP;
    if (action == CURL_POLL_IN || action == CURL_POLL_INOUT)
        events |= EPOLLIN;
    if (action == CURL_POLL_OUT || action == CURL_POLL_INOUT)
        events |= EPOLLOUT;
    if (action != CURL_POLL_IN && action != CURL_POLL_OUT && action != CURL_POLL_INOUT) {
        engine->invariant_failed = true;
        errno = EPROTO;
        return -1;
    }

    struct epoll_event event = {
        .events = events,
        .data.fd = socket_fd,
    };
    if (epoll_ctl(engine->epoll_fd, EPOLL_CTL_ADD, socket_fd, &event) == 0)
        return 0;
    if (errno == EEXIST && epoll_ctl(engine->epoll_fd, EPOLL_CTL_MOD, socket_fd, &event) == 0) {
        return 0;
    }

    engine->invariant_failed = true;
    return -1;
}

static bool perform_socket_action(BxFetchEngine* engine, curl_socket_t socket_fd, int action) {
    int running = 0;
    CURLMcode result = curl_multi_socket_action(engine->multi, socket_fd, action, &running);
    if (result == CURLM_OK)
        return true;
    engine->invariant_failed = true;
    errno = EIO;
    return false;
}

static void fail_active_transfers(BxFetchEngine* engine, BxFetchError result) {
    if (!engine)
        return;
    engine->cancelled = true;
    while (engine->active_head)
        bx_fetch_engine_dispose_transfer(engine, engine->active_head, result);
}

static void reset_response_for_anubis_request(BxFetchTransfer* transfer,
                                              BxFetchAnubisPhase phase) {
    BxFetchResponse* response = transfer->resp;
    reset_response_state(transfer);
    bx_fetch_prepared_url_free(response->effective_target);
    response->effective_target = NULL;
    free(response->content_type);
    response->content_type = NULL;
    free(response->transport_error_detail);
    response->transport_error_detail = NULL;
    response->status_code = 0;
    response->content_length = 0;
    response->error_code = 0;
    response->error_number = -1;
    response->transport_error_kind = BX_FETCH_TRANSPORT_ERROR_NONE;
    response->request_body_io_failed = false;
    response->header_policy_failure = BX_FETCH_RESPONSE_HEADER_POLICY_OK;

    bx_fetch_prepared_url_free(transfer->pending_redirect_target);
    transfer->pending_redirect_target = NULL;
    transfer->redirect_policy_rejected = false;
    transfer->url_canonicalization_failed = false;
    transfer->redirect_protocol_unsupported = false;
    transfer->redirect_target_policy = BX_FETCH_NET_TARGET_ALLOWED;
    transfer->resume_validation_failed = false;
    transfer->io_failed = false;
    transfer->request_body_io_failed = false;
    transfer->io_error_number = 0;
    transfer->anubis_probe_len = 0;
    transfer->anubis_probe_active = false;
    transfer->anubis_challenge_detected = false;
    transfer->anubis_phase = phase;
    transfer->discard_body = phase == BX_FETCH_ANUBIS_PHASE_SUBMIT;
}

static bool requeue_anubis_request(BxFetchEngine* engine,
                                   BxFetchTransfer* transfer,
                                   BxFetchPreparedUrl* target,
                                   BxFetchAnubisPhase phase) {
    if (!engine || !transfer || !target || !transfer->multi_attached) {
        bx_fetch_prepared_url_free(target);
        set_anubis_error(transfer, EPROTO, "invalid Anubis transfer state");
        return false;
    }
    if (bx_fetch_prepared_url_policy(target,
                                     bx_fetch_config_requires_https(engine->cfg)) !=
            BX_FETCH_PROTOCOL_DECISION_ALLOW ||
        bx_fetch_net_target_policy(engine->cfg, target) !=
            BX_FETCH_NET_TARGET_ALLOWED) {
        bx_fetch_prepared_url_free(target);
        set_anubis_error(transfer,
                         ENOTSUP,
                         "Anubis endpoint violates transport policy");
        return false;
    }

    CURLMcode remove_result =
        curl_multi_remove_handle(engine->multi, transfer->easy);
    if (remove_result != CURLM_OK) {
        engine->invariant_failed = true;
        bx_fetch_prepared_url_free(target);
        set_anubis_error(transfer, EPROTO, "could not suspend Anubis transfer");
        return false;
    }
    transfer->multi_attached = false;

    CURLcode url_result =
        curl_easy_setopt(transfer->easy,
                         CURLOPT_URL,
                         bx_fetch_prepared_url_transport(target));
    CURLcode follow_result =
        curl_easy_setopt(transfer->easy,
                         CURLOPT_FOLLOWLOCATION,
                         phase == BX_FETCH_ANUBIS_PHASE_SUBMIT
                             ? 0L
                             : (engine->cfg->http.max_redirect > 0 ? 1L : 0L));
    if (url_result != CURLE_OK || follow_result != CURLE_OK) {
        bx_fetch_prepared_url_free(target);
        set_anubis_error(transfer,
                         EIO,
                         "could not configure Anubis request");
        return false;
    }

    bx_fetch_prepared_url_free(transfer->current_target);
    transfer->current_target = target;
    reset_response_for_anubis_request(transfer, phase);

    CURLMcode add_result =
        curl_multi_add_handle(engine->multi, transfer->easy);
    if (add_result != CURLM_OK) {
        set_anubis_error(transfer, EIO, "could not dispatch Anubis request");
        return false;
    }
    transfer->multi_attached = true;
    return true;
}

static bool begin_anubis_submission(BxFetchEngine* engine,
                                    BxFetchTransfer* transfer) {
    double started = bx_fetch_monotonic_seconds();
    BxFetchAnubisSolution solution;
    if (bx_fetch_anubis_solve(&transfer->anubis_challenge, &solution) != 0) {
        int error_number = errno ? errno : EPROTO;
        set_anubis_error(transfer,
                         error_number,
                         "could not solve Anubis challenge");
        return false;
    }

    double solved = bx_fetch_monotonic_seconds();
    double elapsed_seconds =
        solved >= started && started > 0.0 ? solved - started : 0.0;
    double minimum_seconds =
        (double)solution.minimum_wait_milliseconds / 1000.0;
    if (elapsed_seconds < minimum_seconds &&
        bx_fetch_sleep_for_seconds(minimum_seconds - elapsed_seconds) != 0) {
        set_anubis_error(transfer, errno, "Anubis challenge wait failed");
        return false;
    }
    double finished = bx_fetch_monotonic_seconds();
    elapsed_seconds =
        finished >= started && started > 0.0 ? finished - started : minimum_seconds;
    uint64_t elapsed_milliseconds =
        elapsed_seconds >= (double)UINT64_MAX / 1000.0
            ? UINT64_MAX
            : (uint64_t)(elapsed_seconds * 1000.0);
    char elapsed_text[32];
    int elapsed_length =
        snprintf(elapsed_text,
                 sizeof(elapsed_text),
                 "%" PRIu64,
                 elapsed_milliseconds);
    if (elapsed_length <= 0 || (size_t)elapsed_length >= sizeof(elapsed_text)) {
        set_anubis_error(transfer, EOVERFLOW, "invalid Anubis elapsed time");
        return false;
    }

    char* pass_url =
        bx_fetch_anubis_pass_url(transfer->anubis_retry_target,
                                 &transfer->anubis_challenge,
                                 &solution,
                                 elapsed_text);
    if (!pass_url) {
        set_anubis_error(transfer,
                         errno,
                         "could not construct Anubis validation URL");
        return false;
    }
    BxFetchPreparedUrl* pass_target = bx_fetch_url_prepare(pass_url);
    free(pass_url);
    if (!pass_target ||
        !bx_fetch_prepared_url_same_origin(transfer->anubis_retry_target,
                                           pass_target)) {
        bx_fetch_prepared_url_free(pass_target);
        set_anubis_error(transfer,
                         EPROTO,
                         "Anubis validation URL changed origin");
        return false;
    }
    return requeue_anubis_request(engine,
                                  transfer,
                                  pass_target,
                                  BX_FETCH_ANUBIS_PHASE_SUBMIT);
}

static bool begin_anubis_retry(BxFetchEngine* engine,
                               BxFetchTransfer* transfer,
                               int status) {
    if (status < 300 || status >= 400) {
        set_anubis_error(transfer,
                         EACCES,
                         "Anubis validation request was rejected");
        return false;
    }
    BxFetchPreparedUrl* retry_target =
        bx_fetch_prepared_url_clone(transfer->anubis_retry_target);
    if (!retry_target) {
        set_anubis_error(transfer,
                         ENOMEM,
                         "could not retain Anubis retry target");
        return false;
    }
    return requeue_anubis_request(engine,
                                  transfer,
                                  retry_target,
                                  BX_FETCH_ANUBIS_PHASE_RETRY);
}

static void populate_terminal_response(BxFetchTransfer* transfer, CURLcode curl_result) {
    if (!transfer || !transfer->easy || !transfer->resp)
        return;

    char* content_type = NULL;
    if (curl_easy_getinfo(transfer->easy, CURLINFO_CONTENT_TYPE, &content_type) == CURLE_OK && content_type) {
        char* copy = strdup(content_type);
        if (copy) {
            free(transfer->resp->content_type);
            transfer->resp->content_type = copy;
        }
    }

    curl_off_t content_length = -1;
    if (curl_easy_getinfo(transfer->easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &content_length) == CURLE_OK) {
        transfer->resp->content_length = (int64_t)content_length;
    }

    transfer->resp->error_code = (int)curl_result;
    int os_error_number = -1;
#ifdef CURLINFO_OS_ERRNO
    long os_errno = 0;
    if (curl_easy_getinfo(transfer->easy, CURLINFO_OS_ERRNO, &os_errno) == CURLE_OK && os_errno > 0 && os_errno <= INT_MAX) {
        os_error_number = (int)os_errno;
    }
#endif
    transfer->resp->error_number = transfer->resp->header_policy_failure != BX_FETCH_RESPONSE_HEADER_POLICY_OK ? EFBIG
                                   : transfer->anubis_error_number > 0                                           ? transfer->anubis_error_number
                                   : (transfer->io_failed && transfer->io_error_number > 0)                       ? transfer->io_error_number
                                                                                                                  : os_error_number;
    transfer->resp->request_body_io_failed = transfer->request_body_io_failed;
    transfer->resp->transport_error_kind = bx_fetch_classify_curl_transport_error(curl_result);
    if (transfer_uses_ftp(transfer) && transfer->resp->status_code >= 400 && transfer->resp->status_code < 600 &&
        (transfer->resp->transport_error_kind == BX_FETCH_TRANSPORT_ERROR_NONE || transfer->resp->transport_error_kind == BX_FETCH_TRANSPORT_ERROR_NETWORK)) {
        transfer->resp->transport_error_kind = transfer->resp->status_code == 530 ? BX_FETCH_TRANSPORT_ERROR_AUTH : BX_FETCH_TRANSPORT_ERROR_SERVER;
    }

    free(transfer->resp->transport_error_detail);
    transfer->resp->transport_error_detail = NULL;
    const char* detail = NULL;
    if (transfer->anubis_error_detail) {
        detail = transfer->anubis_error_detail;
    }
    else if (transfer->redirect_target_policy != BX_FETCH_NET_TARGET_ALLOWED) {
        detail = bx_fetch_net_target_policy_reason(transfer->redirect_target_policy);
    }
    else if (transfer->url_canonicalization_failed) {
        detail = transfer->redirect_protocol_unsupported ? "redirect URL uses an unsupported protocol" : "effective or redirect URL failed canonicalization";
    }
    else if (transfer->transform_failed) {
        detail = "failed to convert the HTML response to Markdown";
    }
    else if (curl_result != CURLE_OK) {
        detail = curl_easy_strerror(curl_result);
    }
    if (detail && detail[0] != '\0')
        transfer->resp->transport_error_detail = strdup(detail);
}

static BxFetchError classify_terminal_result(BxFetchTransfer* transfer, CURLcode curl_result, int status, bool invariant_ok) {
    if (!invariant_ok)
        return BX_FETCH_ERROR_INTERNAL;
    if (transfer->resp->header_policy_failure != BX_FETCH_RESPONSE_HEADER_POLICY_OK) {
        return BX_FETCH_ERROR_RESOURCE_LIMIT;
    }
    if (transfer->anubis_error_number == EFBIG)
        return BX_FETCH_ERROR_RESOURCE_LIMIT;
    if (transfer->anubis_error_number != 0)
        return BX_FETCH_ERROR_UNSUPPORTED;
    if (transfer->url_canonicalization_failed)
        return BX_FETCH_ERROR_UNSUPPORTED;
    if (transfer->redirect_policy_rejected)
        return BX_FETCH_ERROR_CANCELLED;
    if (transfer->transform_failed) {
        if (transfer->io_error_number == EFBIG)
            return BX_FETCH_ERROR_RESOURCE_LIMIT;
        if (transfer->io_error_number == ENOMEM)
            return BX_FETCH_ERROR_MEMORY;
        return BX_FETCH_ERROR_UNSUPPORTED;
    }
    if (transfer->io_failed)
        return BX_FETCH_ERROR_IO;
    if (transfer->resume_validation_failed)
        return BX_FETCH_ERROR_HTTP;

    BxFetchError result = bx_fetch_map_curl_result(curl_result);
    if (result == BX_FETCH_OK && transfer_uses_ftp(transfer) && !transfer->engine->cfg->download.spider &&
        bx_fetch_response_payload(transfer->resp, bx_fetch_request_target(transfer->req)) != BX_FETCH_RESPONSE_PAYLOAD_BODY)
        return BX_FETCH_ERROR_NETWORK;
    if (result == BX_FETCH_OK && status >= 400)
        return BX_FETCH_ERROR_HTTP;
    return result;
}

static bool finish_writer(BxFetchEngine* engine, BxFetchTransfer* transfer, CURLcode curl_result) {
    bool transport_succeeded = curl_result == CURLE_OK && !transfer->url_canonicalization_failed;
    BxFetchResponsePayload payload = bx_fetch_response_payload(transfer->resp, bx_fetch_request_target(transfer->req));
    bool ftp = transfer_uses_ftp(transfer);
    bool commit = false;

    if (transport_succeeded && transfer->anubis_error_number == 0 &&
        !transfer->anubis_challenge_detected &&
        transfer->anubis_phase != BX_FETCH_ANUBIS_PHASE_SUBMIT &&
        payload == BX_FETCH_RESPONSE_PAYLOAD_BODY &&
        !(ftp && engine->cfg->download.spider)) {
        if (transfer->resume_restart_validation_pending && !bx_fetch_resume_restart_preserves_verified_prefix(transfer->resume_from, (long long)transfer->response_body_bytes)) {
            transfer->resume_validation_failed = true;
        }
        else {
            commit = true;
        }
    }

    if (commit && ftp) {
        /*
         * Unlike HTTP, FTP has no pre-body header boundary. The writer is
         * still private here; run the same staging callback before close.
         * HTTP Range/conditional handling is not an FTP resume contract.
         */
        if (transfer->resume_requested || !bx_fetch_config_ftp_output_supported(engine->cfg)) {
            errno = ENOTSUP;
            bx_fetch_transfer_mark_io_failure(transfer, ENOTSUP);
            commit = false;
        }
        else if (transfer->headers_cb && transfer->headers_cb(transfer->callback_userdata, transfer->req, transfer->resp, transfer->writer) != 0) {
            bx_fetch_transfer_mark_io_failure(transfer, EIO);
            commit = false;
        }
        else {
            transfer->response_headers_finalized = true;
        }
    }

    if (commit && !transfer->discard_body && !transfer->response_headers_finalized) {
        engine->invariant_failed = true;
        commit = false;
    }
    if (commit && engine->cfg->download.html_to_markdown && !write_markdown_response(transfer))
        commit = false;

    if (transport_succeeded && payload == BX_FETCH_RESPONSE_PAYLOAD_NOT_MODIFIED) {
        if (bx_fetch_transfer_close_writer_metadata_only(transfer) != 0)
            bx_fetch_transfer_mark_io_failure(transfer, EIO);
    }
    else if (commit) {
        if (bx_fetch_transfer_close_writer(transfer) != 0)
            bx_fetch_transfer_mark_io_failure(transfer, EIO);
    }
    else if (!bx_fetch_transfer_abort_writer(transfer)) {
        engine->invariant_failed = true;
    }

    bool finalized = transfer->writer == NULL && (transfer->writer_closed || transfer->writer_aborted);
    if (!finalized)
        engine->invariant_failed = true;
    return finalized;
}

static bool finish_completed_message(BxFetchEngine* engine, const struct CURLMsg* message) {
    BxFetchTransfer* transfer = NULL;
    if (curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &transfer) != CURLE_OK || !transfer) {
        engine->invariant_failed = true;
        errno = EPROTO;
        return false;
    }

    bool invariant_ok = bx_fetch_net_require(engine, transfer->state == BX_FETCH_TRANSFER_STATE_ONGOING) && bx_fetch_net_require(engine, !transfer->terminal_callback_invoked);

    long response_code = 0;
    if (curl_easy_getinfo(message->easy_handle, CURLINFO_RESPONSE_CODE, &response_code) != CURLE_OK) {
        invariant_ok = false;
        engine->invariant_failed = true;
    }
    int status = response_code >= 0 && response_code <= INT_MAX ? (int)response_code : 0;
    transfer->resp->status_code = status;
    if (!transfer->resp->effective_target && !bx_fetch_transfer_refresh_effective_url(transfer)) {
        transfer->url_canonicalization_failed = true;
    }

    if (transfer->progress_cb) {
        curl_off_t total = -1;
        (void)curl_easy_getinfo(message->easy_handle, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &total);
        bx_fetch_progress_emit(transfer, total, true);
    }

    if (message->data.result == CURLE_OK) {
        if (transfer->anubis_probe_active) {
            BxFetchAnubisProbeResult probe_result =
                probe_anubis(transfer, true);
            if (probe_result == BX_FETCH_ANUBIS_PROBE_NO_MATCH) {
                transfer->anubis_probe_active = false;
                if (!write_response_bytes(transfer,
                                          transfer->anubis_probe,
                                          transfer->anubis_probe_len)) {
                    invariant_ok = false;
                }
                transfer->anubis_probe_len = 0;
            }
        }

        if (transfer->anubis_challenge_detected &&
            transfer->anubis_error_number == 0) {
            if (begin_anubis_submission(engine, transfer))
                return true;
        }
        else if (transfer->anubis_phase == BX_FETCH_ANUBIS_PHASE_SUBMIT &&
                 transfer->anubis_error_number == 0) {
            if (begin_anubis_retry(engine, transfer, status))
                return true;
        }
    }

    CURLcode terminal_result =
        transfer->downstream_closed ? CURLE_OK : message->data.result;
    if (!finish_writer(engine, transfer, terminal_result))
        invariant_ok = false;
    populate_terminal_response(transfer, terminal_result);

    BxFetchError result = classify_terminal_result(
        transfer,
        terminal_result,
        status,
        invariant_ok && !engine->invariant_failed);
    bx_fetch_engine_dispose_transfer(engine, transfer, result);
    return !engine->invariant_failed;
}

int bx_fetch_engine_run(BxFetchEngine* engine) {
    if (!engine) {
        errno = EINVAL;
        return -1;
    }
    if (engine->active_transfers == 0)
        return engine->invariant_failed ? -1 : 0;
    if (engine->cancelled) {
        fail_active_transfers(engine, BX_FETCH_ERROR_CANCELLED);
        return 0;
    }

    if (!perform_socket_action(engine, CURL_SOCKET_TIMEOUT, 0))
        goto engine_failure;

    struct epoll_event events[BX_FETCH_MAX_EPOLL_EVENTS];
    int event_count = epoll_wait(engine->epoll_fd, events, BX_FETCH_MAX_EPOLL_EVENTS, 100);
    if (event_count < 0) {
        if (errno == EINTR)
            return 0;
        engine->invariant_failed = true;
        goto engine_failure;
    }

    for (int i = 0; i < event_count; i++) {
        if (events[i].data.fd == engine->timer_fd) {
            uint64_t expirations = 0;
            ssize_t read_count = read(engine->timer_fd, &expirations, sizeof(expirations));
            if (read_count < 0 && errno != EAGAIN) {
                engine->invariant_failed = true;
                goto engine_failure;
            }
            if (read_count >= 0 && read_count != (ssize_t)sizeof(expirations)) {
                engine->invariant_failed = true;
                errno = EIO;
                goto engine_failure;
            }
            if (!perform_socket_action(engine, CURL_SOCKET_TIMEOUT, 0))
                goto engine_failure;
            continue;
        }

        int action = 0;
        if (events[i].events & EPOLLIN)
            action |= CURL_CSELECT_IN;
        if (events[i].events & EPOLLOUT)
            action |= CURL_CSELECT_OUT;
        if (events[i].events & (EPOLLERR | EPOLLHUP))
            action |= CURL_CSELECT_ERR;
        if (!perform_socket_action(engine, events[i].data.fd, action))
            goto engine_failure;
    }

    int messages_left = 0;
    struct CURLMsg* message = NULL;
    while ((message = curl_multi_info_read(engine->multi, &messages_left))) {
        if (message->msg != CURLMSG_DONE)
            continue;
        if (!finish_completed_message(engine, message))
            goto engine_failure;
    }

    return 0;

engine_failure:
    fail_active_transfers(engine, BX_FETCH_ERROR_INTERNAL);
    return -1;
}

bool bx_fetch_engine_is_active(const BxFetchEngine* engine) {
    return engine && engine->active_transfers > 0;
}

bool bx_fetch_engine_quota_exhausted(const BxFetchEngine* engine) {
    return engine && engine->quota_limit_bytes >= 0 && engine->quota_exhausted;
}
