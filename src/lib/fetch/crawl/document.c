#define _GNU_SOURCE
#include "lib/fetch/document.h"
#include "lib/fetch/response.h"
#include "lib/fetch/writer.h"
#include "lib/path_ops.h"
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    BxFetchDocumentLinkFn callback;
    void* userdata;
    bool failed;
    int error_number;
} LinkAdapter;

static int document_fail(BxFetchDocumentOutcome* outcome, BxFetchDocumentKind kind, BxFetchDocumentFailure failure, int error_number) {
    if (outcome) {
        *outcome = (BxFetchDocumentOutcome){
            .kind = kind,
            .failure = failure,
            .error_number = error_number,
        };
    }
    errno = error_number;
    return -1;
}

static bool content_type_equals(const char* content_type, const char* expected) {
    if (!content_type || !expected)
        return false;

    size_t bounded_length = strnlen(content_type, BX_FETCH_RESPONSE_HEADER_LINE_MAX_BYTES + 1u);
    if (bounded_length > BX_FETCH_RESPONSE_HEADER_LINE_MAX_BYTES)
        return false;

    while (bounded_length > 0 && isspace((unsigned char)*content_type)) {
        content_type++;
        bounded_length--;
    }
    const char* end = memchr(content_type, ';', bounded_length);
    size_t length = end ? (size_t)(end - content_type) : bounded_length;
    while (length > 0 && isspace((unsigned char)content_type[length - 1])) {
        length--;
    }
    return strlen(expected) == length && strncasecmp(content_type, expected, length) == 0;
}

static bool path_has_extension(const char* path, const char* extension) {
    const char* actual = bx_path_extension_ptr(path);
    return actual && extension && strcasecmp(actual, extension) == 0;
}

static bool html_tag_name_allowed(const char* name, size_t length) {
    static const char* const tags[] = {
        "a",  "article", "aside", "body", "div", "footer", "form", "h1",     "h2",      "h3",   "h4",    "h5",    "h6",    "head", "header", "html",  "img", "input",
        "li", "link",    "main",  "meta", "nav", "ol",     "p",    "script", "section", "span", "style", "table", "tbody", "td",   "th",     "title", "tr",  "ul",
    };

    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
        if (strlen(tags[i]) == length && strncasecmp(name, tags[i], length) == 0) {
            return true;
        }
    }
    return false;
}

static size_t skip_whitespace(const unsigned char* data, size_t length, size_t position) {
    while (position < length && isspace(data[position]))
        position++;
    return position;
}

static size_t skip_prefix(const unsigned char* data, size_t length, size_t position, const char* prefix, const char* terminator) {
    size_t prefix_length = strlen(prefix);
    if (position > length || prefix_length > length - position || strncasecmp((const char*)data + position, prefix, prefix_length) != 0) {
        return position;
    }

    position += prefix_length;
    size_t terminator_length = strlen(terminator);
    while (position <= length && terminator_length <= length - position) {
        if (memcmp(data + position, terminator, terminator_length) == 0)
            return position + terminator_length;
        position++;
    }
    return length;
}

static bool html_sniff_matches_doctype(const unsigned char* data, size_t length, size_t position) {
    static const char prefix[] = "<!doctype";
    size_t prefix_length = sizeof(prefix) - 1u;
    if (position > length || prefix_length > length - position || strncasecmp((const char*)data + position, prefix, prefix_length) != 0) {
        return false;
    }
    position = skip_whitespace(data, length, position + prefix_length);
    if (position > length || 4u > length - position || strncasecmp((const char*)data + position, "html", 4u) != 0) {
        return false;
    }
    return position + 4u == length || isspace(data[position + 4u]) || data[position + 4u] == '>';
}

static bool html_sniff_matches_tag(const unsigned char* data, size_t length, size_t position) {
    if (position >= length || data[position] != '<')
        return false;
    position++;
    if (position < length && data[position] == '/')
        position++;

    size_t start = position;
    while (position < length && (isalnum(data[position]) || data[position] == '-' || data[position] == ':')) {
        position++;
    }
    return position > start && html_tag_name_allowed((const char*)data + start, position - start);
}

static bool payload_sniffs_as_html(const unsigned char* data, size_t length) {
    size_t sniff_length = length < 1024u ? length : 1024u;
    size_t position = 0;
    if (sniff_length >= 3u && data[0] == 0xefu && data[1] == 0xbbu && data[2] == 0xbfu) {
        position = 3u;
    }

    while (position < sniff_length) {
        size_t next = skip_whitespace(data, sniff_length, position);
        if (next != position) {
            position = next;
            continue;
        }
        next = skip_prefix(data, sniff_length, position, "<!--", "-->");
        if (next != position) {
            position = next;
            continue;
        }
        next = skip_prefix(data, sniff_length, position, "<?xml", "?>");
        if (next != position) {
            position = next;
            continue;
        }
        break;
    }

    return position < sniff_length && (html_sniff_matches_doctype(data, sniff_length, position) || html_sniff_matches_tag(data, sniff_length, position));
}

static BxFetchDocumentKind classify_document(const char* path, const char* content_type, const unsigned char* data, size_t length) {
    bool explicit_html = content_type_equals(content_type, "text/html") || content_type_equals(content_type, "application/xhtml+xml");
    bool explicit_css = content_type_equals(content_type, "text/css");
    bool ambiguous = !content_type || content_type[0] == '\0' || content_type_equals(content_type, "text/plain") || content_type_equals(content_type, "application/octet-stream") ||
                     content_type_equals(content_type, "binary/octet-stream");
    bool sniffed_html = payload_sniffs_as_html(data, length);

    if (explicit_css)
        return BX_FETCH_DOCUMENT_CSS;
    if (explicit_html)
        return sniffed_html ? BX_FETCH_DOCUMENT_HTML : BX_FETCH_DOCUMENT_NONE;
    if (ambiguous && sniffed_html)
        return BX_FETCH_DOCUMENT_HTML;
    if ((path_has_extension(path, ".html") || path_has_extension(path, ".htm") || path_has_extension(path, ".xhtml") || path_has_extension(path, ".xht")) && sniffed_html) {
        return BX_FETCH_DOCUMENT_HTML;
    }
    if (path_has_extension(path, ".css") && ambiguous)
        return BX_FETCH_DOCUMENT_CSS;
    return BX_FETCH_DOCUMENT_NONE;
}

static int read_document(const char* path, unsigned char** data_out, size_t* length_out, BxFetchDocumentOutcome* outcome) {
    int fd = bx_fetch_writer_open_existing_file(path);
    if (fd == -1) {
        return document_fail(outcome, BX_FETCH_DOCUMENT_NONE, BX_FETCH_DOCUMENT_FAILURE_OPEN, errno ? errno : EIO);
    }

    struct stat file_status;
    if (fstat(fd, &file_status) != 0) {
        int error_number = errno ? errno : EIO;
        close(fd);
        return document_fail(outcome, BX_FETCH_DOCUMENT_NONE, BX_FETCH_DOCUMENT_FAILURE_READ, error_number);
    }
    if (file_status.st_size < 0 || (uintmax_t)file_status.st_size > (uintmax_t)BX_FETCH_DOCUMENT_MAX_BYTES) {
        close(fd);
        return document_fail(outcome, BX_FETCH_DOCUMENT_NONE, BX_FETCH_DOCUMENT_FAILURE_TOO_LARGE, EFBIG);
    }

    size_t capacity = (size_t)file_status.st_size;
    if (capacity < 16384u)
        capacity = 16384u;
    if (capacity > BX_FETCH_DOCUMENT_MAX_BYTES)
        capacity = BX_FETCH_DOCUMENT_MAX_BYTES;
    unsigned char* data = malloc(capacity + 1u);
    if (!data) {
        close(fd);
        return document_fail(outcome, BX_FETCH_DOCUMENT_NONE, BX_FETCH_DOCUMENT_FAILURE_ALLOCATION, ENOMEM);
    }

    size_t length = 0;
    for (;;) {
        if (length == capacity) {
            if (capacity == BX_FETCH_DOCUMENT_MAX_BYTES) {
                unsigned char extra;
                ssize_t extra_size = read(fd, &extra, 1u);
                if (extra_size == 0)
                    break;
                int error_number = extra_size < 0 ? (errno ? errno : EIO) : EFBIG;
                free(data);
                close(fd);
                return document_fail(outcome, BX_FETCH_DOCUMENT_NONE, extra_size < 0 ? BX_FETCH_DOCUMENT_FAILURE_READ : BX_FETCH_DOCUMENT_FAILURE_TOO_LARGE, error_number);
            }
            size_t next_capacity = capacity > SIZE_MAX / 2u ? BX_FETCH_DOCUMENT_MAX_BYTES : capacity * 2u;
            if (next_capacity > BX_FETCH_DOCUMENT_MAX_BYTES)
                next_capacity = BX_FETCH_DOCUMENT_MAX_BYTES;
            unsigned char* grown = realloc(data, next_capacity + 1u);
            if (!grown) {
                free(data);
                close(fd);
                return document_fail(outcome, BX_FETCH_DOCUMENT_NONE, BX_FETCH_DOCUMENT_FAILURE_ALLOCATION, ENOMEM);
            }
            data = grown;
            capacity = next_capacity;
        }

        ssize_t read_size = read(fd, data + length, capacity - length);
        if (read_size == 0)
            break;
        if (read_size < 0) {
            if (errno == EINTR)
                continue;
            int error_number = errno ? errno : EIO;
            free(data);
            close(fd);
            return document_fail(outcome, BX_FETCH_DOCUMENT_NONE, BX_FETCH_DOCUMENT_FAILURE_READ, error_number);
        }
        length += (size_t)read_size;
    }

    if (close(fd) != 0) {
        int error_number = errno ? errno : EIO;
        free(data);
        return document_fail(outcome, BX_FETCH_DOCUMENT_NONE, BX_FETCH_DOCUMENT_FAILURE_READ, error_number);
    }
    data[length] = '\0';
    *data_out = data;
    *length_out = length;
    return 0;
}

static void adapt_html_link(void* userdata, const char* reference, BxFetchHtmlLinkKind kind) {
    LinkAdapter* adapter = userdata;
    if (!adapter || adapter->failed)
        return;
    if (adapter->callback(adapter->userdata, reference, kind) != 0) {
        adapter->failed = true;
        adapter->error_number = errno ? errno : EIO;
    }
}

static void adapt_css_link(void* userdata, const char* reference) {
    adapt_html_link(userdata, reference, BX_FETCH_HTML_LINK_REQUISITE);
}

int bx_fetch_document_extract_links(const char* path, const char* content_type, const BxFetchPreparedUrl* base, BxFetchDocumentLinkFn callback, void* userdata, BxFetchDocumentOutcome* outcome) {
    if (outcome)
        *outcome = (BxFetchDocumentOutcome){0};
    if (!path || !base || !callback) {
        return document_fail(outcome, BX_FETCH_DOCUMENT_NONE, BX_FETCH_DOCUMENT_FAILURE_INVALID_ARGUMENT, EINVAL);
    }

    unsigned char* data = NULL;
    size_t length = 0;
    if (read_document(path, &data, &length, outcome) != 0)
        return -1;

    BxFetchDocumentKind kind = classify_document(path, content_type, data, length);
    if (outcome)
        outcome->kind = kind;
    if (kind == BX_FETCH_DOCUMENT_NONE) {
        free(data);
        return 0;
    }

    LinkAdapter adapter = {
        .callback = callback,
        .userdata = userdata,
    };
    const char* base_url = bx_fetch_prepared_url_transport(base);
    int parse_result = kind == BX_FETCH_DOCUMENT_HTML ? bx_fetch_html_extract_links_typed(base_url, (const char*)data, length, adapt_html_link, &adapter)
                                                      : bx_fetch_css_extract_links(base_url, (const char*)data, length, adapt_css_link, &adapter);
    int error_number = errno;
    free(data);

    if (adapter.failed) {
        return document_fail(outcome, kind, BX_FETCH_DOCUMENT_FAILURE_CALLBACK, adapter.error_number);
    }
    if (parse_result != 0) {
        return document_fail(outcome, kind, BX_FETCH_DOCUMENT_FAILURE_PARSE, error_number ? error_number : EINVAL);
    }
    return 0;
}
