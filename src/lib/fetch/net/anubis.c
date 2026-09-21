#define _GNU_SOURCE
#include "lib/fetch/anubis.h"
#include "crypto/sha256.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ANUBIS_JSON_MAX_DEPTH 16u

typedef struct {
    const char* current;
    const char* end;
} JsonCursor;

typedef struct {
    const char* data;
    size_t length;
    bool present;
    bool incomplete;
} ScriptContent;

static const char* bounded_find(const char* data, size_t length, const char* needle) {
    size_t needle_length = strlen(needle);
    if (!data || needle_length == 0 || needle_length > length)
        return NULL;
    return memmem(data, length, needle, needle_length);
}

static ScriptContent find_json_script(const char* html, size_t length, const char* id) {
    ScriptContent result = {0};
    char double_marker[96];
    char single_marker[96];
    int double_length = snprintf(double_marker, sizeof(double_marker), "id=\"%s\"", id);
    int single_length = snprintf(single_marker, sizeof(single_marker), "id='%s'", id);
    if (double_length <= 0 || (size_t)double_length >= sizeof(double_marker) ||
        single_length <= 0 || (size_t)single_length >= sizeof(single_marker)) {
        return result;
    }

    const char* marker = bounded_find(html, length, double_marker);
    if (!marker)
        marker = bounded_find(html, length, single_marker);
    if (!marker)
        return result;
    result.present = true;

    const char* opening = NULL;
    const char* scan = marker;
    size_t inspected = 0;
    while (scan > html && inspected < 512u) {
        scan--;
        inspected++;
        if (*scan == '>') {
            result.incomplete = false;
            return result;
        }
        if (*scan == '<') {
            if ((size_t)(marker - scan) >= 7u && memcmp(scan, "<script", 7u) == 0)
                opening = scan;
            break;
        }
    }
    if (!opening)
        return result;

    const char* html_end = html + length;
    const char* content = memchr(marker, '>', (size_t)(html_end - marker));
    if (!content) {
        result.incomplete = true;
        return result;
    }
    content++;

    const char* closing = bounded_find(content, (size_t)(html_end - content), "</script>");
    if (!closing) {
        result.incomplete = true;
        return result;
    }

    result.data = content;
    result.length = (size_t)(closing - content);
    return result;
}

static void json_skip_space(JsonCursor* cursor) {
    while (cursor->current < cursor->end && isspace((unsigned char)*cursor->current))
        cursor->current++;
}

static bool json_take(JsonCursor* cursor, char expected) {
    json_skip_space(cursor);
    if (cursor->current >= cursor->end || *cursor->current != expected)
        return false;
    cursor->current++;
    return true;
}

static int hex_value(unsigned char value) {
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return 10 + value - 'a';
    if (value >= 'A' && value <= 'F')
        return 10 + value - 'A';
    return -1;
}

static bool json_string(JsonCursor* cursor, char* output, size_t capacity) {
    json_skip_space(cursor);
    if (cursor->current >= cursor->end || *cursor->current++ != '"' || !output || capacity == 0)
        return false;

    size_t used = 0;
    while (cursor->current < cursor->end) {
        unsigned char value = (unsigned char)*cursor->current++;
        if (value == '"') {
            output[used] = '\0';
            return true;
        }
        if (value < 0x20u)
            return false;
        if (value == '\\') {
            if (cursor->current >= cursor->end)
                return false;
            value = (unsigned char)*cursor->current++;
            switch (value) {
                case '"':
                case '\\':
                case '/':
                    break;
                case 'b':
                    value = '\b';
                    break;
                case 'f':
                    value = '\f';
                    break;
                case 'n':
                    value = '\n';
                    break;
                case 'r':
                    value = '\r';
                    break;
                case 't':
                    value = '\t';
                    break;
                case 'u': {
                    if ((size_t)(cursor->end - cursor->current) < 4u)
                        return false;
                    unsigned codepoint = 0;
                    for (size_t i = 0; i < 4u; i++) {
                        int digit = hex_value((unsigned char)cursor->current[i]);
                        if (digit < 0)
                            return false;
                        codepoint = (codepoint << 4u) | (unsigned)digit;
                    }
                    cursor->current += 4;
                    if (codepoint == 0 || codepoint > 0x7fu)
                        return false;
                    value = (unsigned char)codepoint;
                    break;
                }
                default:
                    return false;
            }
        }
        if (used + 1u >= capacity)
            return false;
        output[used++] = (char)value;
    }
    return false;
}

static bool json_skip_string(JsonCursor* cursor) {
    json_skip_space(cursor);
    if (cursor->current >= cursor->end || *cursor->current++ != '"')
        return false;
    while (cursor->current < cursor->end) {
        unsigned char value = (unsigned char)*cursor->current++;
        if (value == '"')
            return true;
        if (value < 0x20u)
            return false;
        if (value != '\\')
            continue;
        if (cursor->current >= cursor->end)
            return false;
        value = (unsigned char)*cursor->current++;
        if (value == 'u') {
            if ((size_t)(cursor->end - cursor->current) < 4u)
                return false;
            for (size_t i = 0; i < 4u; i++) {
                if (hex_value((unsigned char)cursor->current[i]) < 0)
                    return false;
            }
            cursor->current += 4;
        }
        else if (!strchr("\"\\/bfnrt", (int)value)) {
            return false;
        }
    }
    return false;
}

static bool json_skip_value(JsonCursor* cursor, unsigned depth) {
    if (depth > ANUBIS_JSON_MAX_DEPTH)
        return false;
    json_skip_space(cursor);
    if (cursor->current >= cursor->end)
        return false;

    if (*cursor->current == '"')
        return json_skip_string(cursor);
    if (*cursor->current == '{') {
        cursor->current++;
        json_skip_space(cursor);
        if (cursor->current < cursor->end && *cursor->current == '}') {
            cursor->current++;
            return true;
        }
        for (;;) {
            if (!json_skip_string(cursor) || !json_take(cursor, ':') ||
                !json_skip_value(cursor, depth + 1u)) {
                return false;
            }
            json_skip_space(cursor);
            if (cursor->current < cursor->end && *cursor->current == '}') {
                cursor->current++;
                return true;
            }
            if (!json_take(cursor, ','))
                return false;
        }
    }
    if (*cursor->current == '[') {
        cursor->current++;
        json_skip_space(cursor);
        if (cursor->current < cursor->end && *cursor->current == ']') {
            cursor->current++;
            return true;
        }
        for (;;) {
            if (!json_skip_value(cursor, depth + 1u))
                return false;
            json_skip_space(cursor);
            if (cursor->current < cursor->end && *cursor->current == ']') {
                cursor->current++;
                return true;
            }
            if (!json_take(cursor, ','))
                return false;
        }
    }

    const char* start = cursor->current;
    while (cursor->current < cursor->end &&
           !isspace((unsigned char)*cursor->current) &&
           !strchr(",]}", *cursor->current)) {
        cursor->current++;
    }
    if (cursor->current == start)
        return false;
    size_t length = (size_t)(cursor->current - start);
    if ((length == 4u && (memcmp(start, "true", 4u) == 0 || memcmp(start, "null", 4u) == 0)) ||
        (length == 5u && memcmp(start, "false", 5u) == 0)) {
        return true;
    }

    JsonCursor number = {.current = start, .end = cursor->current};
    if (*number.current == '-')
        number.current++;
    if (number.current >= number.end)
        return false;
    if (*number.current == '0') {
        number.current++;
    }
    else {
        if (!isdigit((unsigned char)*number.current))
            return false;
        while (number.current < number.end && isdigit((unsigned char)*number.current))
            number.current++;
    }
    if (number.current < number.end && *number.current == '.') {
        number.current++;
        const char* digits = number.current;
        while (number.current < number.end && isdigit((unsigned char)*number.current))
            number.current++;
        if (number.current == digits)
            return false;
    }
    if (number.current < number.end && (*number.current == 'e' || *number.current == 'E')) {
        number.current++;
        if (number.current < number.end && (*number.current == '+' || *number.current == '-'))
            number.current++;
        const char* digits = number.current;
        while (number.current < number.end && isdigit((unsigned char)*number.current))
            number.current++;
        if (number.current == digits)
            return false;
    }
    return number.current == number.end;
}

static bool json_nonnegative_int(JsonCursor* cursor, int* output) {
    json_skip_space(cursor);
    if (cursor->current >= cursor->end || !isdigit((unsigned char)*cursor->current))
        return false;
    unsigned value = 0;
    do {
        unsigned digit = (unsigned)(*cursor->current - '0');
        if (value > ((unsigned)INT_MAX - digit) / 10u)
            return false;
        value = value * 10u + digit;
        cursor->current++;
    } while (cursor->current < cursor->end && isdigit((unsigned char)*cursor->current));
    if (cursor->current < cursor->end &&
        !isspace((unsigned char)*cursor->current) &&
        !strchr(",}", *cursor->current)) {
        return false;
    }
    *output = (int)value;
    return true;
}

static bool parse_challenge_object(JsonCursor* cursor, BxFetchAnubisChallenge* challenge) {
    if (!json_take(cursor, '{'))
        return false;
    bool saw_random_data = false;
    bool saw_id = false;
    json_skip_space(cursor);
    if (cursor->current < cursor->end && *cursor->current == '}')
        return false;

    for (;;) {
        char key[64];
        if (!json_string(cursor, key, sizeof(key)) || !json_take(cursor, ':'))
            return false;
        if (strcmp(key, "randomData") == 0) {
            if (saw_random_data ||
                !json_string(cursor, challenge->random_data, sizeof(challenge->random_data))) {
                return false;
            }
            saw_random_data = true;
        }
        else if (strcmp(key, "id") == 0) {
            if (saw_id || !json_string(cursor, challenge->id, sizeof(challenge->id)))
                return false;
            saw_id = true;
        }
        else if (!json_skip_value(cursor, 1u)) {
            return false;
        }
        json_skip_space(cursor);
        if (cursor->current < cursor->end && *cursor->current == '}') {
            cursor->current++;
            return saw_random_data;
        }
        if (!json_take(cursor, ','))
            return false;
    }
}

static bool parse_rules_object(JsonCursor* cursor, BxFetchAnubisChallenge* challenge) {
    if (!json_take(cursor, '{'))
        return false;
    bool saw_difficulty = false;
    bool saw_algorithm = false;
    json_skip_space(cursor);
    if (cursor->current < cursor->end && *cursor->current == '}')
        return false;

    for (;;) {
        char key[64];
        if (!json_string(cursor, key, sizeof(key)) || !json_take(cursor, ':'))
            return false;
        if (strcmp(key, "difficulty") == 0) {
            if (saw_difficulty || !json_nonnegative_int(cursor, &challenge->difficulty))
                return false;
            saw_difficulty = true;
        }
        else if (strcmp(key, "algorithm") == 0) {
            if (saw_algorithm ||
                !json_string(cursor, challenge->algorithm, sizeof(challenge->algorithm))) {
                return false;
            }
            saw_algorithm = true;
        }
        else if (!json_skip_value(cursor, 1u)) {
            return false;
        }
        json_skip_space(cursor);
        if (cursor->current < cursor->end && *cursor->current == '}') {
            cursor->current++;
            return saw_difficulty;
        }
        if (!json_take(cursor, ','))
            return false;
    }
}

static bool parse_challenge_json(const char* data, size_t length, BxFetchAnubisChallenge* challenge) {
    JsonCursor cursor = {.current = data, .end = data + length};
    if (!json_take(&cursor, '{'))
        return false;
    bool saw_challenge = false;
    bool saw_rules = false;

    for (;;) {
        char key[64];
        json_skip_space(&cursor);
        if (cursor.current < cursor.end && *cursor.current == '}') {
            cursor.current++;
            break;
        }
        if (!json_string(&cursor, key, sizeof(key)) || !json_take(&cursor, ':'))
            return false;
        if (strcmp(key, "challenge") == 0) {
            if (saw_challenge)
                return false;
            saw_challenge = true;
            json_skip_space(&cursor);
            if (cursor.current < cursor.end && *cursor.current == '"') {
                if (!json_string(&cursor, challenge->random_data, sizeof(challenge->random_data)))
                    return false;
            }
            else if (!parse_challenge_object(&cursor, challenge)) {
                return false;
            }
        }
        else if (strcmp(key, "rules") == 0) {
            if (saw_rules || !parse_rules_object(&cursor, challenge))
                return false;
            saw_rules = true;
        }
        else if (!json_skip_value(&cursor, 1u)) {
            return false;
        }
        json_skip_space(&cursor);
        if (cursor.current < cursor.end && *cursor.current == '}') {
            cursor.current++;
            break;
        }
        if (!json_take(&cursor, ','))
            return false;
    }
    json_skip_space(&cursor);
    return cursor.current == cursor.end && saw_challenge && saw_rules &&
           challenge->random_data[0] != '\0';
}

static bool parse_algorithm(BxFetchAnubisChallenge* challenge) {
    if (challenge->algorithm[0] == '\0')
        memcpy(challenge->algorithm, "fast", sizeof("fast"));
    if (strcmp(challenge->algorithm, "fast") == 0)
        challenge->algorithm_kind = BX_FETCH_ANUBIS_ALGORITHM_FAST;
    else if (strcmp(challenge->algorithm, "slow") == 0)
        challenge->algorithm_kind = BX_FETCH_ANUBIS_ALGORITHM_SLOW;
    else if (strcmp(challenge->algorithm, "preact") == 0)
        challenge->algorithm_kind = BX_FETCH_ANUBIS_ALGORITHM_PREACT;
    else if (strcmp(challenge->algorithm, "metarefresh") == 0)
        challenge->algorithm_kind = BX_FETCH_ANUBIS_ALGORITHM_METAREFRESH;
    else if (strcmp(challenge->algorithm, "sha256") == 0)
        challenge->algorithm_kind = BX_FETCH_ANUBIS_ALGORITHM_SHA256;
    else
        return false;
    return true;
}

static bool base_prefix_valid(const char* prefix) {
    if (!prefix)
        return false;
    if (prefix[0] == '\0')
        return true;
    if (prefix[0] != '/' || prefix[1] == '/' || strchr(prefix, '?') ||
        strchr(prefix, '#') || strchr(prefix, '\\')) {
        return false;
    }
    for (const unsigned char* value = (const unsigned char*)prefix; *value; value++) {
        if (*value < 0x20u || *value >= 0x7fu)
            return false;
    }
    return true;
}

static bool percent_decode_field(const char* value,
                                 size_t length,
                                 char* output,
                                 size_t capacity) {
    if (!value || !output || capacity == 0)
        return false;
    size_t used = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char decoded = (unsigned char)value[i];
        if (decoded == '%') {
            if (i + 2u >= length)
                return false;
            int high = hex_value((unsigned char)value[i + 1u]);
            int low = hex_value((unsigned char)value[i + 2u]);
            if (high < 0 || low < 0)
                return false;
            decoded = (unsigned char)((unsigned)high << 4u | (unsigned)low);
            i += 2u;
        }
        else if (decoded == '+') {
            decoded = ' ';
        }
        if (decoded == 0 || decoded < 0x20u || used + 1u >= capacity)
            return false;
        output[used++] = (char)decoded;
    }
    output[used] = '\0';
    return true;
}

static bool query_field(const char* query,
                        const char* name,
                        char* output,
                        size_t capacity,
                        bool required) {
    if (!query || !name || !output || capacity == 0)
        return !required;
    size_t name_length = strlen(name);
    bool found = false;
    const char* field = query;
    while (*field) {
        const char* end = strchr(field, '&');
        if (!end)
            end = field + strlen(field);
        const char* equals = memchr(field, '=', (size_t)(end - field));
        if (equals && (size_t)(equals - field) == name_length &&
            memcmp(field, name, name_length) == 0) {
            if (found ||
                !percent_decode_field(equals + 1,
                                      (size_t)(end - equals - 1),
                                      output,
                                      capacity)) {
                return false;
            }
            found = true;
        }
        field = *end ? end + 1 : end;
    }
    return found || !required;
}

static bool parse_submission_url(const char* value,
                                 BxFetchAnubisChallenge* challenge,
                                 bool take_challenge) {
    if (!value || !challenge)
        return false;
    const char* path = value;
    const char* scheme = strstr(value, "://");
    if (scheme) {
        path = strchr(scheme + 3, '/');
        if (!path)
            return false;
    }
    if (path[0] != '/' || path[1] == '/')
        return false;
    const char* query = strchr(path, '?');
    size_t path_length = query ? (size_t)(query - path) : strlen(path);
    size_t suffix_length = strlen(BX_FETCH_ANUBIS_PASS_PATH);
    if (path_length < suffix_length ||
        memcmp(path + path_length - suffix_length,
               BX_FETCH_ANUBIS_PASS_PATH,
               suffix_length) != 0) {
        return false;
    }
    size_t prefix_length = path_length - suffix_length;
    if (prefix_length > BX_FETCH_ANUBIS_BASE_PREFIX_MAX_BYTES)
        return false;
    memcpy(challenge->base_prefix, path, prefix_length);
    challenge->base_prefix[prefix_length] = '\0';
    if (!base_prefix_valid(challenge->base_prefix))
        return false;

    const char* query_text = query ? query + 1 : "";
    if (!query_field(query_text,
                     "id",
                     challenge->id,
                     sizeof(challenge->id),
                     false)) {
        return false;
    }
    if (take_challenge &&
        !query_field(query_text,
                     "challenge",
                     challenge->random_data,
                     sizeof(challenge->random_data),
                     true)) {
        return false;
    }
    return true;
}

static bool parse_preact_json(const char* data,
                              size_t length,
                              BxFetchAnubisChallenge* challenge) {
    JsonCursor cursor = {.current = data, .end = data + length};
    if (!json_take(&cursor, '{'))
        return false;
    bool saw_redir = false;
    bool saw_challenge = false;
    bool saw_difficulty = false;
    char* redir = malloc(BX_FETCH_ANUBIS_PROBE_LIMIT_BYTES + 1u);
    if (!redir)
        return false;

    bool valid = false;
    for (;;) {
        char key[64];
        json_skip_space(&cursor);
        if (cursor.current < cursor.end && *cursor.current == '}') {
            cursor.current++;
            break;
        }
        if (!json_string(&cursor, key, sizeof(key)) || !json_take(&cursor, ':'))
            goto done;
        if (strcmp(key, "redir") == 0) {
            if (saw_redir ||
                !json_string(&cursor,
                             redir,
                             BX_FETCH_ANUBIS_PROBE_LIMIT_BYTES + 1u)) {
                goto done;
            }
            saw_redir = true;
        }
        else if (strcmp(key, "challenge") == 0) {
            if (saw_challenge ||
                !json_string(&cursor,
                             challenge->random_data,
                             sizeof(challenge->random_data))) {
                goto done;
            }
            saw_challenge = true;
        }
        else if (strcmp(key, "difficulty") == 0) {
            if (saw_difficulty ||
                !json_nonnegative_int(&cursor, &challenge->difficulty)) {
                goto done;
            }
            saw_difficulty = true;
        }
        else if (!json_skip_value(&cursor, 1u)) {
            goto done;
        }
        json_skip_space(&cursor);
        if (cursor.current < cursor.end && *cursor.current == '}') {
            cursor.current++;
            break;
        }
        if (!json_take(&cursor, ','))
            goto done;
    }
    json_skip_space(&cursor);
    valid = cursor.current == cursor.end && saw_redir && saw_challenge &&
            saw_difficulty && challenge->random_data[0] != '\0' &&
            challenge->difficulty <= BX_FETCH_ANUBIS_DIFFICULTY_MAX &&
            parse_submission_url(redir, challenge, false);
    if (valid) {
        memcpy(challenge->algorithm, "preact", sizeof("preact"));
        challenge->algorithm_kind = BX_FETCH_ANUBIS_ALGORITHM_PREACT;
    }
done:
    free(redir);
    return valid;
}

BxFetchAnubisProbeResult bx_fetch_anubis_probe_refresh(const char* value,
                                                       BxFetchAnubisChallenge* out) {
    if (!value || !out)
        return BX_FETCH_ANUBIS_PROBE_INVALID;
    while (isspace((unsigned char)*value))
        value++;
    char* end = NULL;
    errno = 0;
    long delay = strtol(value, &end, 10);
    if (errno || end == value || delay <= 0 ||
        delay - 1 > BX_FETCH_ANUBIS_DIFFICULTY_MAX) {
        return BX_FETCH_ANUBIS_PROBE_INVALID;
    }
    while (isspace((unsigned char)*end))
        end++;
    if (*end++ != ';')
        return BX_FETCH_ANUBIS_PROBE_INVALID;
    while (isspace((unsigned char)*end))
        end++;
    if (strncasecmp(end, "url=", 4u) != 0)
        return BX_FETCH_ANUBIS_PROBE_INVALID;
    end += 4;
    while (isspace((unsigned char)*end))
        end++;
    if (*end == '\0')
        return BX_FETCH_ANUBIS_PROBE_INVALID;

    BxFetchAnubisChallenge parsed = {
        .algorithm_kind = BX_FETCH_ANUBIS_ALGORITHM_METAREFRESH,
        .difficulty = (int)(delay - 1),
    };
    memcpy(parsed.algorithm, "metarefresh", sizeof("metarefresh"));
    if (!parse_submission_url(end, &parsed, true))
        return BX_FETCH_ANUBIS_PROBE_INVALID;
    *out = parsed;
    return BX_FETCH_ANUBIS_PROBE_MATCH;
}

BxFetchAnubisProbeResult bx_fetch_anubis_probe(const char* prefix, size_t length, bool final, BxFetchAnubisChallenge* out) {
    if (!prefix || !out || length > BX_FETCH_ANUBIS_PROBE_LIMIT_BYTES)
        return BX_FETCH_ANUBIS_PROBE_INVALID;

    ScriptContent challenge_script = find_json_script(prefix, length, "anubis_challenge");
    if (!challenge_script.present) {
        ScriptContent preact_script =
            find_json_script(prefix, length, "preact_info");
        if (preact_script.present) {
            if (preact_script.incomplete)
                return final ? BX_FETCH_ANUBIS_PROBE_INVALID
                             : BX_FETCH_ANUBIS_PROBE_UNDECIDED;
            BxFetchAnubisChallenge parsed = {0};
            if (!preact_script.data ||
                !parse_preact_json(preact_script.data,
                                   preact_script.length,
                                   &parsed)) {
                return BX_FETCH_ANUBIS_PROBE_INVALID;
            }
            *out = parsed;
            return BX_FETCH_ANUBIS_PROBE_MATCH;
        }

        const char* meta = bounded_find(prefix, length, "http-equiv=\"refresh\"");
        if (meta) {
            const char* html_end = prefix + length;
            const char* content =
                bounded_find(meta,
                             (size_t)(html_end - meta),
                             "content=\"");
            if (!content)
                return final ? BX_FETCH_ANUBIS_PROBE_INVALID
                             : BX_FETCH_ANUBIS_PROBE_UNDECIDED;
            content += strlen("content=\"");
            const char* closing =
                memchr(content, '"', (size_t)(html_end - content));
            if (!closing)
                return final ? BX_FETCH_ANUBIS_PROBE_INVALID
                             : BX_FETCH_ANUBIS_PROBE_UNDECIDED;
            size_t encoded_length = (size_t)(closing - content);
            char* decoded = malloc(encoded_length + 1u);
            if (!decoded)
                return BX_FETCH_ANUBIS_PROBE_INVALID;
            size_t used = 0;
            for (size_t i = 0; i < encoded_length;) {
                if (i + 5u <= encoded_length &&
                    memcmp(content + i, "&amp;", 5u) == 0) {
                    decoded[used++] = '&';
                    i += 5u;
                }
                else {
                    decoded[used++] = content[i++];
                }
            }
            decoded[used] = '\0';
            BxFetchAnubisProbeResult result =
                bx_fetch_anubis_probe_refresh(decoded, out);
            free(decoded);
            return result;
        }
        return final ? BX_FETCH_ANUBIS_PROBE_NO_MATCH
                     : BX_FETCH_ANUBIS_PROBE_UNDECIDED;
    }
    if (challenge_script.incomplete)
        return final ? BX_FETCH_ANUBIS_PROBE_INVALID : BX_FETCH_ANUBIS_PROBE_UNDECIDED;
    if (!challenge_script.data)
        return BX_FETCH_ANUBIS_PROBE_INVALID;

    BxFetchAnubisChallenge parsed = {0};
    if (!parse_challenge_json(challenge_script.data, challenge_script.length, &parsed) ||
        parsed.difficulty < 0 || parsed.difficulty > BX_FETCH_ANUBIS_DIFFICULTY_MAX ||
        !parse_algorithm(&parsed)) {
        return BX_FETCH_ANUBIS_PROBE_INVALID;
    }

    ScriptContent base_script = find_json_script(prefix, length, "anubis_base_prefix");
    if (base_script.present && base_script.incomplete)
        return final ? BX_FETCH_ANUBIS_PROBE_INVALID : BX_FETCH_ANUBIS_PROBE_UNDECIDED;
    if (base_script.present) {
        JsonCursor cursor = {
            .current = base_script.data,
            .end = base_script.data + base_script.length,
        };
        if (!base_script.data ||
            !json_string(&cursor, parsed.base_prefix, sizeof(parsed.base_prefix))) {
            return BX_FETCH_ANUBIS_PROBE_INVALID;
        }
        json_skip_space(&cursor);
        if (cursor.current != cursor.end)
            return BX_FETCH_ANUBIS_PROBE_INVALID;
    }
    else if (!final) {
        return BX_FETCH_ANUBIS_PROBE_UNDECIDED;
    }

    if (!base_prefix_valid(parsed.base_prefix))
        return BX_FETCH_ANUBIS_PROBE_INVALID;
    *out = parsed;
    return BX_FETCH_ANUBIS_PROBE_MATCH;
}

static void digest_to_hex(const uint8_t digest[BX_SHA256_DIGEST_SIZE], char output[65]) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < BX_SHA256_DIGEST_SIZE; i++) {
        output[i * 2u] = digits[digest[i] >> 4u];
        output[i * 2u + 1u] = digits[digest[i] & 0x0fu];
    }
    output[64] = '\0';
}

static bool digest_matches(const uint8_t digest[BX_SHA256_DIGEST_SIZE], int difficulty) {
    size_t full_bytes = (size_t)difficulty / 2u;
    for (size_t i = 0; i < full_bytes; i++) {
        if (digest[i] != 0)
            return false;
    }
    return (difficulty & 1) == 0 || (digest[full_bytes] & 0xf0u) == 0;
}

static bool digest_matches_bits(const uint8_t digest[BX_SHA256_DIGEST_SIZE],
                                int difficulty) {
    size_t full_bytes = (size_t)difficulty / 8u;
    for (size_t i = 0; i < full_bytes; i++) {
        if (digest[i] != 0)
            return false;
    }
    unsigned remaining = (unsigned)difficulty & 7u;
    return remaining == 0 ||
           (digest[full_bytes] & (uint8_t)(0xffu << (8u - remaining))) == 0;
}

static size_t decimal_u64(uint64_t value, char output[BX_FETCH_ANUBIS_NONCE_MAX_BYTES]) {
    char reversed[BX_FETCH_ANUBIS_NONCE_MAX_BYTES];
    size_t length = 0;
    do {
        reversed[length++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0);
    for (size_t i = 0; i < length; i++)
        output[i] = reversed[length - i - 1u];
    output[length] = '\0';
    return length;
}

int bx_fetch_anubis_solve(const BxFetchAnubisChallenge* challenge, BxFetchAnubisSolution* solution) {
    if (!challenge || !solution || challenge->random_data[0] == '\0' ||
        challenge->difficulty < 0 || challenge->difficulty > BX_FETCH_ANUBIS_DIFFICULTY_MAX) {
        errno = EINVAL;
        return -1;
    }

    BxFetchAnubisSolution solved = {0};
    if (challenge->algorithm_kind == BX_FETCH_ANUBIS_ALGORITHM_METAREFRESH) {
        size_t length = strlen(challenge->random_data);
        if (length >= sizeof(solved.response)) {
            errno = EOVERFLOW;
            return -1;
        }
        memcpy(solved.response, challenge->random_data, length + 1u);
        solved.attempts = 1u;
        solved.minimum_wait_milliseconds = (uint64_t)challenge->difficulty * 800u;
        *solution = solved;
        return 0;
    }

    struct bx_sha256_ctx base;
    bx_sha256_init(&base);
    bx_sha256_update(&base, challenge->random_data, strlen(challenge->random_data));
    if (challenge->algorithm_kind == BX_FETCH_ANUBIS_ALGORITHM_PREACT) {
        uint8_t digest[BX_SHA256_DIGEST_SIZE];
        bx_sha256_final(&base, digest);
        digest_to_hex(digest, solved.response);
        solved.attempts = 1u;
        solved.minimum_wait_milliseconds = (uint64_t)challenge->difficulty * 80u;
        *solution = solved;
        return 0;
    }
    if (challenge->algorithm_kind == BX_FETCH_ANUBIS_ALGORITHM_SHA256) {
        size_t encoded_length = strlen(challenge->random_data);
        if (encoded_length == 0 || (encoded_length & 1u) != 0 ||
            encoded_length / 2u > BX_FETCH_ANUBIS_RANDOM_DATA_MAX_BYTES) {
            errno = EINVAL;
            return -1;
        }
        uint8_t decoded[BX_FETCH_ANUBIS_RANDOM_DATA_MAX_BYTES];
        size_t decoded_length = encoded_length / 2u;
        for (size_t i = 0; i < decoded_length; i++) {
            int high =
                hex_value((unsigned char)challenge->random_data[i * 2u]);
            int low =
                hex_value((unsigned char)challenge->random_data[i * 2u + 1u]);
            if (high < 0 || low < 0) {
                errno = EINVAL;
                return -1;
            }
            decoded[i] = (uint8_t)((unsigned)high << 4u | (unsigned)low);
        }

        struct bx_sha256_ctx binary_base;
        bx_sha256_init(&binary_base);
        bx_sha256_update(&binary_base, decoded, decoded_length);
        bool little_endian = decoded[decoded_length - 1u] >= 128u;
        for (uint32_t nonce = 0;; nonce++) {
            uint8_t nonce_bytes[4];
            for (size_t i = 0; i < sizeof(nonce_bytes); i++) {
                size_t shift_index = little_endian ? i : 3u - i;
                nonce_bytes[i] = (uint8_t)(nonce >> (shift_index * 8u));
            }
            struct bx_sha256_ctx attempt = binary_base;
            uint8_t digest[BX_SHA256_DIGEST_SIZE];
            bx_sha256_update(&attempt, nonce_bytes, sizeof(nonce_bytes));
            bx_sha256_final(&attempt, digest);
            if (digest_matches_bits(digest, challenge->difficulty)) {
                decimal_u64(nonce, solved.nonce);
                digest_to_hex(digest, solved.response);
                solved.attempts = (uint64_t)nonce + 1u;
                *solution = solved;
                return 0;
            }
            if (nonce == UINT32_MAX) {
                errno = EOVERFLOW;
                return -1;
            }
        }
    }
    if (challenge->algorithm_kind != BX_FETCH_ANUBIS_ALGORITHM_FAST &&
        challenge->algorithm_kind != BX_FETCH_ANUBIS_ALGORITHM_SLOW) {
        errno = EINVAL;
        return -1;
    }

    for (uint64_t nonce = 0;; nonce++) {
        char nonce_text[BX_FETCH_ANUBIS_NONCE_MAX_BYTES];
        size_t nonce_length = decimal_u64(nonce, nonce_text);
        struct bx_sha256_ctx attempt = base;
        uint8_t digest[BX_SHA256_DIGEST_SIZE];
        bx_sha256_update(&attempt, nonce_text, nonce_length);
        bx_sha256_final(&attempt, digest);
        if (digest_matches(digest, challenge->difficulty)) {
            memcpy(solved.nonce, nonce_text, nonce_length + 1u);
            digest_to_hex(digest, solved.response);
            solved.attempts = nonce == UINT64_MAX ? UINT64_MAX : nonce + 1u;
            *solution = solved;
            return 0;
        }
        if (nonce == UINT64_MAX) {
            errno = EOVERFLOW;
            return -1;
        }
    }
}

static char* percent_encode(const char* input) {
    static const char digits[] = "0123456789ABCDEF";
    if (!input)
        return NULL;
    size_t length = strlen(input);
    if (length > (SIZE_MAX - 1u) / 3u) {
        errno = EOVERFLOW;
        return NULL;
    }
    char* output = malloc(length * 3u + 1u);
    if (!output)
        return NULL;
    size_t used = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char value = (unsigned char)input[i];
        if (isalnum(value) || value == '-' || value == '.' || value == '_' || value == '~') {
            output[used++] = (char)value;
        }
        else {
            output[used++] = '%';
            output[used++] = digits[value >> 4u];
            output[used++] = digits[value & 0x0fu];
        }
    }
    output[used] = '\0';
    return output;
}

static bool decimal_text_valid(const char* value) {
    if (!value || value[0] == '\0')
        return false;
    for (; *value; value++) {
        if (!isdigit((unsigned char)*value))
            return false;
    }
    return true;
}

char* bx_fetch_anubis_pass_url(const BxFetchPreparedUrl* target,
                               const BxFetchAnubisChallenge* challenge,
                               const BxFetchAnubisSolution* solution,
                               const char* elapsed_milliseconds) {
    if (!target || !challenge || !solution || !base_prefix_valid(challenge->base_prefix) ||
        solution->response[0] == '\0') {
        errno = EINVAL;
        return NULL;
    }
    BxFetchProtocol protocol = bx_fetch_prepared_url_protocol(target);
    if (protocol != BX_FETCH_PROTOCOL_HTTP && protocol != BX_FETCH_PROTOCOL_HTTPS) {
        errno = EPROTONOSUPPORT;
        return NULL;
    }
    bool proof_of_work = challenge->algorithm_kind == BX_FETCH_ANUBIS_ALGORITHM_FAST ||
                         challenge->algorithm_kind == BX_FETCH_ANUBIS_ALGORITHM_SLOW ||
                         challenge->algorithm_kind == BX_FETCH_ANUBIS_ALGORITHM_SHA256;
    if (proof_of_work &&
        (!decimal_text_valid(solution->nonce) || !decimal_text_valid(elapsed_milliseconds))) {
        errno = EINVAL;
        return NULL;
    }

    size_t prefix_length = strlen(challenge->base_prefix);
    while (prefix_length > 0 && challenge->base_prefix[prefix_length - 1u] == '/')
        prefix_length--;
    size_t suffix_length = strlen(BX_FETCH_ANUBIS_PASS_PATH);
    if (prefix_length > SIZE_MAX - suffix_length - 1u) {
        errno = EOVERFLOW;
        return NULL;
    }
    char* path = malloc(prefix_length + suffix_length + 1u);
    if (!path)
        return NULL;
    memcpy(path, challenge->base_prefix, prefix_length);
    memcpy(path + prefix_length, BX_FETCH_ANUBIS_PASS_PATH, suffix_length + 1u);

    BxFetchPreparedUrl* safe_target = bx_fetch_url_prepare(bx_fetch_prepared_url_display(target));
    BxFetchPreparedUrl* endpoint = safe_target ? bx_fetch_prepared_url_resolve(safe_target, path) : NULL;
    free(path);
    if (!safe_target || !endpoint || !bx_fetch_prepared_url_same_origin(safe_target, endpoint) ||
        bx_fetch_prepared_url_has_userinfo(endpoint)) {
        bx_fetch_prepared_url_free(endpoint);
        bx_fetch_prepared_url_free(safe_target);
        errno = EINVAL;
        return NULL;
    }

    char* encoded_response = percent_encode(solution->response);
    char* encoded_redir = percent_encode(bx_fetch_prepared_url_display(safe_target));
    char* encoded_id = challenge->id[0] ? percent_encode(challenge->id) : NULL;
    if (!encoded_response || !encoded_redir || (challenge->id[0] && !encoded_id)) {
        free(encoded_response);
        free(encoded_redir);
        free(encoded_id);
        bx_fetch_prepared_url_free(endpoint);
        bx_fetch_prepared_url_free(safe_target);
        return NULL;
    }

    const char* endpoint_text = bx_fetch_prepared_url_display(endpoint);
    const char* response_key = challenge->algorithm_kind == BX_FETCH_ANUBIS_ALGORITHM_PREACT
                                   ? "result"
                                   : challenge->algorithm_kind == BX_FETCH_ANUBIS_ALGORITHM_METAREFRESH
                                         ? "challenge"
                                         : "response";
    const char* nonce_part = proof_of_work ? "&nonce=" : "";
    const char* nonce_value = proof_of_work ? solution->nonce : "";
    const char* elapsed_part = proof_of_work ? "&elapsedTime=" : "";
    const char* elapsed_value = proof_of_work ? elapsed_milliseconds : "";
    const char* id_part = encoded_id ? "&id=" : "";
    const char* id_value = encoded_id ? encoded_id : "";

    size_t needed = strlen(endpoint_text) + 1u + strlen(response_key) + 1u +
                    strlen(encoded_response) + strlen(nonce_part) + strlen(nonce_value) +
                    strlen("&redir=") + strlen(encoded_redir) + strlen(elapsed_part) +
                    strlen(elapsed_value) + strlen(id_part) + strlen(id_value) + 1u;
    char* result = malloc(needed);
    if (result) {
        int written = snprintf(result,
                               needed,
                               "%s?%s=%s%s%s&redir=%s%s%s%s%s",
                               endpoint_text,
                               response_key,
                               encoded_response,
                               nonce_part,
                               nonce_value,
                               encoded_redir,
                               elapsed_part,
                               elapsed_value,
                               id_part,
                               id_value);
        if (written < 0 || (size_t)written >= needed) {
            free(result);
            result = NULL;
            errno = EOVERFLOW;
        }
    }

    free(encoded_response);
    free(encoded_redir);
    free(encoded_id);
    bx_fetch_prepared_url_free(endpoint);
    bx_fetch_prepared_url_free(safe_target);
    return result;
}
