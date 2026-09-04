#define _GNU_SOURCE
#include "lib/fetch/pathmap.h"
#include "lib/fetch/url.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

typedef struct {
    bool unix_mode;
    bool windows_mode;
} RestrictFileNameModes;

static const char* pathmap_empty_segment_token(void) {
    return "@mira@empty";
}

static const char* pathmap_reserved_segment_prefix(void) {
    return "@mira@";
}

static const char* pathmap_escaped_segment_prefix(void) {
    return "@mira@literal@";
}

static const char* pathmap_encoded_segment_prefix(void) {
    return "@mira@encoded@";
}

static bool restrict_modes_enabled(RestrictFileNameModes modes) {
    return modes.unix_mode || modes.windows_mode;
}

static size_t count_leading_dots(const char* text) {
    size_t count = 0;
    if (!text)
        return 0;

    while (text[count] == '.') {
        count++;
    }

    return count;
}

static bool is_windows_device_name(const char* text) {
    if (!text || text[0] == '\0')
        return false;

    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '.' || text[len - 1] == ' ')) {
        len--;
    }
    if (len == 0)
        return false;

    size_t base_len = 0;
    while (base_len < len && text[base_len] != '.') {
        base_len++;
    }

    if (base_len == 3) {
        char name[4];
        for (size_t i = 0; i < 3; i++) {
            name[i] = (char)toupper((unsigned char)text[i]);
        }
        name[3] = '\0';
        return strcmp(name, "CON") == 0 || strcmp(name, "PRN") == 0 || strcmp(name, "AUX") == 0 || strcmp(name, "NUL") == 0;
    }

    if (base_len == 4) {
        char prefix[4];
        for (size_t i = 0; i < 3; i++) {
            prefix[i] = (char)toupper((unsigned char)text[i]);
        }
        prefix[3] = '\0';
        return ((strcmp(prefix, "COM") == 0) || (strcmp(prefix, "LPT") == 0)) && text[3] >= '1' && text[3] <= '9';
    }

    return false;
}

static const char* default_page_name(const EffectiveConfig* cfg) {
    if (!cfg || !cfg->http.default_page || cfg->http.default_page[0] == '\0') {
        return "index.html";
    }
    return cfg->http.default_page;
}

static bool parse_restrict_modes(const EffectiveConfig* cfg, RestrictFileNameModes* modes_out) {
    if (!modes_out) {
        errno = EINVAL;
        return false;
    }
    *modes_out = (RestrictFileNameModes){0};
    if (!cfg || !cfg->download.restrict_file_names || cfg->download.restrict_file_names[0] == '\0') {
        return true;
    }

    char* copy = strdup(cfg->download.restrict_file_names);
    if (!copy)
        return false;

    char* cursor = copy;
    char* tok = NULL;
    while ((tok = strsep(&cursor, ",")) != NULL) {
        while (*tok && isspace((unsigned char)*tok))
            tok++;

        size_t len = strlen(tok);
        while (len > 0 && isspace((unsigned char)tok[len - 1])) {
            tok[--len] = '\0';
        }
        if (len == 0)
            continue;

        if (strcasecmp(tok, "unix") == 0) {
            modes_out->unix_mode = true;
        }
        else if (strcasecmp(tok, "windows") == 0) {
            modes_out->windows_mode = true;
        }
        else {
            free(copy);
            errno = EINVAL;
            return false;
        }
    }

    free(copy);
    return true;
}

static bool is_windows_reserved_char(unsigned char c) {
    switch (c) {
        case '<':
        case '>':
        case ':':
        case '"':
        case '\\':
        case '|':
        case '?':
        case '*':
            return true;
        default:
            return false;
    }
}

static bool component_needs_encoding(const char* component, RestrictFileNameModes modes) {
    if (!component || component[0] == '\0' || strcmp(component, ".") == 0 || strcmp(component, "..") == 0) {
        return true;
    }

    size_t len = strlen(component);
    if (modes.windows_mode && (component[len - 1] == '.' || component[len - 1] == ' ' || is_windows_device_name(component))) {
        return true;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)component[i];
        if (c < 0x20 || c == 0x7f || c == '/' || (modes.windows_mode && is_windows_reserved_char(c))) {
            return true;
        }
    }
    return false;
}

static char hex_digit(unsigned int value) {
    return value < 10u ? (char)('0' + value) : (char)('a' + (value - 10u));
}

static char* encode_component(const char* component) {
    const char* prefix = pathmap_encoded_segment_prefix();
    size_t prefix_len = strlen(prefix);
    size_t component_len = strlen(component);
    if (component_len > (SIZE_MAX - prefix_len - 1u) / 2u) {
        errno = EOVERFLOW;
        return NULL;
    }

    char* encoded = malloc(prefix_len + (component_len * 2u) + 1u);
    if (!encoded)
        return NULL;
    memcpy(encoded, prefix, prefix_len);

    for (size_t i = 0; i < component_len; i++) {
        unsigned char byte = (unsigned char)component[i];
        encoded[prefix_len + (i * 2u)] = hex_digit(byte >> 4u);
        encoded[prefix_len + (i * 2u) + 1u] = hex_digit(byte & 0x0fu);
    }
    encoded[prefix_len + (component_len * 2u)] = '\0';
    return encoded;
}

char* pathmap_sanitize_component(const char* component, const EffectiveConfig* cfg) {
    if (!component)
        return NULL;
    if (component[0] == '\0') {
        return strdup(pathmap_empty_segment_token());
    }

    RestrictFileNameModes modes = {0};
    if (!parse_restrict_modes(cfg, &modes))
        return NULL;
    if (component_needs_encoding(component, modes)) {
        return encode_component(component);
    }

    bool restrict_mode = restrict_modes_enabled(modes);
    const char* reserved_prefix = pathmap_reserved_segment_prefix();
    size_t reserved_prefix_len = strlen(reserved_prefix);

    char* clean = strdup(component);
    if (!clean)
        return NULL;

    size_t leading_dots = restrict_mode ? count_leading_dots(clean) : 0;

    const char* payload_source = clean + leading_dots;
    char* payload = strdup(payload_source);
    if (!payload) {
        free(clean);
        return NULL;
    }

    char* sanitized = NULL;
    if (leading_dots > 0) {
        if (payload[0] == '\0') {
            if (asprintf(&sanitized, "@mira@dot%zu", leading_dots) == -1) {
                sanitized = NULL;
            }
        }
        else if (asprintf(&sanitized, "@mira@dot%zu@%s", leading_dots, payload) == -1) {
            sanitized = NULL;
        }
    }
    else if (strncmp(payload, reserved_prefix, reserved_prefix_len) == 0) {
        if (asprintf(&sanitized, "%s%s", pathmap_escaped_segment_prefix(), payload) == -1) {
            sanitized = NULL;
        }
    }
    else {
        sanitized = strdup(payload);
    }

    free(payload);
    free(clean);
    return sanitized;
}

static char* sanitize_directory_segment(const char* segment, const EffectiveConfig* cfg) {
    char* sanitized = NULL;

    if (!segment || segment[0] == '\0') {
        sanitized = strdup(pathmap_empty_segment_token());
    }
    else {
        sanitized = pathmap_sanitize_component(segment, cfg);
    }

    return sanitized;
}

static void free_segments(char** segments, size_t count) {
    if (!segments)
        return;
    for (size_t i = 0; i < count; i++) {
        free(segments[i]);
    }
    free(segments);
}

static char* build_directory_part(const char* path, int cut_dirs, const EffectiveConfig* cfg) {
    if (!path)
        return NULL;

    const char* trimmed = path;
    if (trimmed[0] == '/') {
        trimmed++;
    }

    char* copy = strdup(trimmed);
    if (!copy)
        return NULL;

    char** segments = NULL;
    size_t count = 0;
    size_t capacity = 0;
    size_t total_len = 1;
    size_t skipped = 0;

    char* cursor = copy;
    char* token = NULL;
    while ((token = strsep(&cursor, "/")) != NULL) {
        bool is_last_token = (cursor == NULL);
        if (is_last_token) {
            break;
        }

        if (cut_dirs > 0 && skipped < (size_t)cut_dirs) {
            skipped++;
            continue;
        }

        char* sanitized = sanitize_directory_segment(token, cfg);
        if (!sanitized) {
            free(copy);
            free_segments(segments, count);
            return NULL;
        }

        if (count == capacity) {
            size_t next_capacity = capacity == 0 ? 4 : capacity * 2;
            char** tmp = realloc(segments, next_capacity * sizeof(*segments));
            if (!tmp) {
                free(sanitized);
                free(copy);
                free_segments(segments, count);
                return NULL;
            }
            segments = tmp;
            capacity = next_capacity;
        }

        segments[count++] = sanitized;
        total_len += strlen(sanitized) + 1;
    }

    free(copy);

    if (count == 0) {
        free(segments);
        return NULL;
    }

    char* joined = malloc(total_len);
    if (!joined) {
        free_segments(segments, count);
        return NULL;
    }

    joined[0] = '\0';
    for (size_t i = 0; i < count; i++) {
        if (i > 0)
            strcat(joined, "/");
        strcat(joined, segments[i]);
    }

    free_segments(segments, count);
    return joined;
}

static char* extract_filename_from_url(const char* url, const EffectiveConfig* cfg) {
    MiraURL* mu = mira_url_parse(url);
    if (!mu)
        return NULL;

    char* filename = NULL;
    const char* default_page = default_page_name(cfg);
    if (mu->path) {
        // Strip trailing slash if present
        size_t len = strlen(mu->path);
        if (len > 0 && mu->path[len - 1] == '/') {
            filename = strdup(default_page);
        }
        else {
            char* path_copy = strdup(mu->path);
            if (!path_copy) {
                mira_url_free(mu);
                return NULL;
            }
            const char* p = strrchr(path_copy, '/');
            if (p && *(p + 1) != '\0') {
                filename = strdup(p + 1);
            }
            else if (p == NULL && path_copy[0] != '\0') {
                // No slash, but path not empty
                filename = strdup(path_copy);
            }
            free(path_copy);
        }
    }

    if (!filename) {
        filename = strdup(default_page);
    }
    if (!filename) {
        mira_url_free(mu);
        return NULL;
    }

    char* sanitized = pathmap_sanitize_component(filename, cfg);
    free(filename);
    if (!sanitized) {
        mira_url_free(mu);
        return NULL;
    }
    if (sanitized[0] == '\0') {
        free(sanitized);
        sanitized = pathmap_sanitize_component(default_page, cfg);
    }

    mira_url_free(mu);
    return sanitized;
}

char* pathmap_url_to_local(const char* url, const EffectiveConfig* cfg) {
    if (!url || !cfg)
        return NULL;

    char* canonical = mira_url_canonicalize(url);
    if (!canonical)
        return NULL;
    char* local_path = pathmap_canonical_url_to_local(canonical, cfg);
    free(canonical);
    return local_path;
}

char* pathmap_canonical_url_to_local(const char* url, const EffectiveConfig* cfg) {
    if (!url || !cfg)
        return NULL;

    MiraURL* mu = mira_url_parse(url);
    if (!mu)
        return NULL;

    char* local_path = NULL;
    bool flatten_layout = cfg->dirs.no_directories || (!cfg->recursive.recursive && !cfg->dirs.force_directories);

    if (flatten_layout) {
        local_path = extract_filename_from_url(url, cfg);
    }
    else {
        // Build the directory structure
        char* host_part = NULL;
        char* proto_part = NULL;
        char* dir_part = NULL;
        char* file_part = extract_filename_from_url(url, cfg);
        if (!file_part) {
            mira_url_free(mu);
            return NULL;
        }

        if (cfg->dirs.protocol_directories && mu->scheme) {
            proto_part = pathmap_sanitize_component(mu->scheme, cfg);
        }

        if (!cfg->dirs.no_host_directories && mu->host) {
            host_part = pathmap_sanitize_component(mu->host, cfg);
        }

        if (mu->path) {
            dir_part = build_directory_part(mu->path, cfg->dirs.cut_dirs, cfg);
        }

        // Join components: [proto/][host/][dir/]file
        size_t total_len = (proto_part ? strlen(proto_part) + 1 : 0) + (host_part ? strlen(host_part) + 1 : 0) + (dir_part ? strlen(dir_part) + 1 : 0) + strlen(file_part) + 1;

        local_path = malloc(total_len);
        if (!local_path) {
            free(proto_part);
            free(host_part);
            free(dir_part);
            free(file_part);
            mira_url_free(mu);
            return NULL;
        }
        local_path[0] = '\0';

        if (proto_part) {
            strcat(local_path, proto_part);
            strcat(local_path, "/");
        }
        if (host_part) {
            strcat(local_path, host_part);
            strcat(local_path, "/");
        }
        if (dir_part && dir_part[0]) {
            strcat(local_path, dir_part);
            strcat(local_path, "/");
        }
        strcat(local_path, file_part);

        free(proto_part);
        free(host_part);
        free(dir_part);
        free(file_part);
    }

    mira_url_free(mu);

    if (!local_path) {
        return NULL;
    }

    // Prefix with directory_prefix if set
    if (cfg->dirs.directory_prefix && cfg->dirs.directory_prefix[0] != '\0') {
        char* full_path = NULL;
        if (asprintf(&full_path, "%s/%s", cfg->dirs.directory_prefix, local_path) == -1) {
            full_path = NULL;
        }
        free(local_path);
        return full_path;
    }

    return local_path;
}
