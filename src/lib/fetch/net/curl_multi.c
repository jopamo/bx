#define _GNU_SOURCE
#include "engine_internal.h"
#include "lib/fetch/resume_validation.h"
#include "lib/fetch/url.h"
#include <curl/curl.h>
#include <errno.h>
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

    if (bx_fetch_prepared_url_policy(target, t->engine && t->engine->cfg ? t->engine->cfg->https.https_only : false) != BX_FETCH_PROTOCOL_DECISION_ALLOW) {
        t->redirect_protocol_unsupported = true;
        bx_fetch_prepared_url_free(target);
        errno = EPROTONOSUPPORT;
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

size_t bx_fetch_header_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    BxFetchTransfer* t = userdata;
    if (size != 0 && nmemb > SIZE_MAX / size) {
        return reject_response_header(t, BX_FETCH_RESPONSE_HEADER_POLICY_LINE_TOO_LARGE);
    }
    size_t total = size * nmemb;
    if (!t || total == 0)
        return total;

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
            if (t->headers_cb && (status == 200 || status == 206) && !t->discard_body && t->headers_cb(t->callback_userdata, t->req, t->resp, t->writer) != 0) {
                bx_fetch_transfer_mark_io_failure(t, EIO);
                free(line);
                return 0;
            }

            t->response_headers_finalized = true;
        }
        free(line);
        return total;
    }

    if (starts_response) {
        if (t->pending_redirect_target) {
            bx_fetch_prepared_url_free(t->current_target);
            t->current_target = t->pending_redirect_target;
            t->pending_redirect_target = NULL;
        }

        int status = parsed_status;
        if (status > 0) {
            t->resp->status_code = status;
            bx_fetch_response_reset_headers(t->resp);
            if (capture_headers) {
                save_headers_reset(t);
                if (save_headers_append(t, ptr, total) != 0) {
                    int append_error = errno;
                    free(line);
                    errno = append_error;
                    return handle_save_headers_append_failure(t);
                }
            }

            t->resume_needs_content_range = false;
            t->resume_saw_content_range = false;
            t->discard_body = false;
            t->resume_restart_validation_pending = false;
            t->response_body_bytes = 0;
            t->progress_resume_offset = 0;
            t->response_headers_finalized = false;

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
                    t->resume_requested = false;
                }
                else if (status == 206) {
                    t->resume_needs_content_range = true;
                    t->progress_resume_offset = (curl_off_t)t->resume_from;
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

    if (t->engine && t->engine->cfg->http.max_redirect > 0 && status_is_redirect(t->resp->status_code) && strcasecmp(name, "Location") == 0) {
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
        if (!bx_fetch_resume_content_range_matches(value, t->resume_from)) {
            t->resume_validation_failed = true;
            free(line);
            return 0;
        }
        t->resume_saw_content_range = true;
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

    if (t->discard_body) {
        bx_fetch_record_downloaded_bytes(t, total);
        return total;
    }

    if (t->engine && t->engine->cfg->http.save_headers && !t->save_headers_written && t->save_headers_len > 0) {
        if (bx_fetch_writer_write(t->writer, t->save_headers_buf, t->save_headers_len) != 0) {
            bx_fetch_transfer_mark_io_failure(t, EIO);
            return 0;
        }
        t->save_headers_written = true;
    }

    if (bx_fetch_writer_write(t->writer, ptr, total) != 0) {
        bx_fetch_transfer_mark_io_failure(t, EIO);
        return 0;  // signal error
    }

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
        timer.it_value.tv_sec = timeout_ms / 1000;
        timer.it_value.tv_nsec = (timeout_ms % 1000) * 1000000;
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
                                   : (transfer->io_failed && transfer->io_error_number > 0)                    ? transfer->io_error_number
                                                                                                               : os_error_number;
    transfer->resp->request_body_io_failed = transfer->request_body_io_failed;
    transfer->resp->transport_error_kind = bx_fetch_classify_curl_transport_error(curl_result);

    free(transfer->resp->transport_error_detail);
    transfer->resp->transport_error_detail = NULL;
    const char* detail = NULL;
    if (transfer->url_canonicalization_failed) {
        detail = transfer->redirect_protocol_unsupported ? "redirect URL uses an unsupported protocol" : "effective or redirect URL failed canonicalization";
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
    if (transfer->url_canonicalization_failed)
        return BX_FETCH_ERROR_UNSUPPORTED;
    if (transfer->redirect_policy_rejected)
        return BX_FETCH_ERROR_CANCELLED;
    if (transfer->io_failed)
        return BX_FETCH_ERROR_IO;
    if (transfer->resume_validation_failed)
        return BX_FETCH_ERROR_HTTP;

    BxFetchError result = bx_fetch_map_curl_result(curl_result);
    if (result == BX_FETCH_OK && status >= 400)
        return BX_FETCH_ERROR_HTTP;
    return result;
}

static bool finish_writer(BxFetchEngine* engine, BxFetchTransfer* transfer, CURLcode curl_result, int status) {
    bool transport_succeeded = curl_result == CURLE_OK && !transfer->url_canonicalization_failed;
    bool commit = false;

    if (transport_succeeded && status != 304) {
        if (transfer->resume_restart_validation_pending && !bx_fetch_resume_restart_preserves_verified_prefix(transfer->resume_from, (long long)transfer->response_body_bytes)) {
            transfer->resume_validation_failed = true;
        }
        else {
            commit = true;
        }
    }

    if (commit && (status == 200 || status == 206) && !transfer->discard_body && !transfer->response_headers_finalized) {
        engine->invariant_failed = true;
        commit = false;
    }

    if (commit) {
        if (bx_fetch_transfer_close_writer(transfer) != 0)
            bx_fetch_transfer_mark_io_failure(transfer, EIO);
    }
    else if (!bx_fetch_transfer_abort_writer(transfer)) {
        engine->invariant_failed = true;
    }
    else if (status == 304 && transport_succeeded) {
        transfer->resp->output_state = BX_FETCH_OUTPUT_STATE_UNCHANGED;
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

    if (engine->observer.on_progress) {
        curl_off_t total = -1;
        curl_off_t downloaded = 0;
        (void)curl_easy_getinfo(message->easy_handle, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &total);
        (void)curl_easy_getinfo(message->easy_handle, CURLINFO_SIZE_DOWNLOAD_T, &downloaded);
        bx_fetch_progress_emit(transfer, total, downloaded);
    }

    populate_terminal_response(transfer, message->data.result);
    if (!finish_writer(engine, transfer, message->data.result, status))
        invariant_ok = false;

    BxFetchError result = classify_terminal_result(transfer, message->data.result, status, invariant_ok && !engine->invariant_failed);
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
