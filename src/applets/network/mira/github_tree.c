#define _GNU_SOURCE
#include "github_internal.h"
#include "repository_json.h"
#include "lib/fetch/exit_code.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static jv field(jv object, const char* name) {
    return jv_object_get(jv_copy(object), jv_string(name));
}

static bool text(jv value, size_t limit) {
    if (jv_get_kind(value) != JV_KIND_STRING)
        return false;
    int length = jv_string_length_bytes(jv_copy(value));
    return length > 0 && (size_t)length <= limit &&
        strlen(jv_string_value(value)) == (size_t)length;
}

static bool sha_valid(jv value) {
    if (!text(value, 64))
        return false;
    size_t length = strlen(jv_string_value(value));
    return (length == 40 || length == 64) &&
        strspn(jv_string_value(value), "0123456789abcdefABCDEF") == length;
}

static bool tree_valid(jv value, const char* expected_sha, bool shallow) {
    if (jv_get_kind(value) != JV_KIND_OBJECT)
        return false;
    jv sha = field(value, "sha");
    jv truncated = field(value, "truncated");
    jv entries = field(value, "tree");
    bool valid = sha_valid(sha) &&
        (!expected_sha || strcmp(jv_string_value(sha), expected_sha) == 0) &&
        (jv_get_kind(truncated) == JV_KIND_FALSE || jv_get_kind(truncated) == JV_KIND_TRUE) &&
        jv_get_kind(entries) == JV_KIND_ARRAY;
    int count = valid ? jv_array_length(jv_copy(entries)) : 0;
    for (int i = 0; valid && i < count; i++) {
        jv entry = jv_array_get(jv_copy(entries), i);
        if (jv_get_kind(entry) != JV_KIND_OBJECT) {
            jv_free(entry);
            valid = false;
            break;
        }
        jv path = field(entry, "path"), type = field(entry, "type"), child = field(entry, "sha");
        valid = text(path, 4096) && text(type, 16) && sha_valid(child);
        if (valid) {
            const char* name = jv_string_value(path);
            const char* kind = jv_string_value(type);
            valid = (!shallow || !strchr(name, '/')) && strcmp(name, ".") != 0 && strcmp(name, "..") != 0 &&
                (strcmp(kind, "tree") == 0 || strcmp(kind, "blob") == 0 || strcmp(kind, "commit") == 0);
        }
        jv_free(path); jv_free(type); jv_free(child); jv_free(entry);
    }
    jv_free(sha); jv_free(truncated); jv_free(entries);
    return valid;
}

static bool is_truncated(jv value) {
    jv flag = field(value, "truncated");
    bool truncated = jv_get_kind(flag) == JV_KIND_TRUE;
    jv_free(flag);
    return truncated;
}

static int fetch_tree(const MiraGithubArguments* arguments, struct bx_fetch_config* config,
                      BxFetchBudget* budget, const char* ref, bool recursive, jv* value) {
    char* encoded = bx_fetch_url_encode_component(ref, 4096);
    char* url = NULL;
    const char* root = arguments->repository.api_root ? arguments->repository.api_root : MIRA_GITHUB_API_ROOT;
    if (encoded && asprintf(&url, "/repos/%s/git/trees/%s%s",
            arguments->operands[0], encoded, recursive ? "?recursive=1" : "") < 0)
        url = NULL;
    free(encoded);
    if (!url)
        return BX_FETCH_EXIT_PARSE_OR_CONFIG;
    char* path = url;
    url = bx_fetch_url_join_https_root(root, path);
    free(path);
    if (!url)
        return BX_FETCH_EXIT_PARSE_OR_CONFIG;
    int result = bx_mira_repository_json(config, budget, url, value);
    free(url);
    if (result == 0 && !tree_valid(*value, NULL, !recursive)) {
        fputs("mira: github tree: invalid tree response\n", stderr);
        return BX_FETCH_EXIT_PROTOCOL;
    }
    return result;
}

static jv task(const char* sha, const char* prefix, int depth) {
    return jv_object_set(jv_object_set(jv_object_set(jv_object(),
        jv_string("sha"), jv_string(sha)), jv_string("prefix"), jv_string(prefix)),
        jv_string("depth"), jv_number(depth));
}

static bool account_value(jv value, size_t* bytes) {
    jv encoded = jv_dump_string(jv_copy(value), 0);
    if (jv_get_kind(encoded) != JV_KIND_STRING) {
        jv_free(encoded);
        return false;
    }
    size_t length = (size_t)jv_string_length_bytes(jv_copy(encoded));
    jv_free(encoded);
    if (length > MIRA_REPOSITORY_JSON_LIMIT - *bytes)
        return false;
    *bytes += length;
    return true;
}

static int expand_tree(const MiraGithubArguments* arguments, struct bx_fetch_config* config,
                       BxFetchBudget* budget, jv* document) {
    jv root_sha = field(*document, "sha");
    jv queue = jv_array_append(jv_array(), task(jv_string_value(root_sha), "", 0));
    jv cache = jv_object(), result = jv_array();
    size_t retained_bytes = 0;
    int code = 0;
    for (int head = 0; head < jv_array_length(jv_copy(queue)); head++) {
        jv current = jv_array_get(jv_copy(queue), head);
        jv sha = field(current, "sha"), prefix = field(current, "prefix"), depth_value = field(current, "depth");
        int depth = (int)jv_number_value(depth_value);
        jv_free(depth_value);
        jv subtree = jv_object_get(jv_copy(cache), jv_copy(sha));
        if (!jv_is_valid(subtree)) {
            jv_free(subtree);
            subtree = jv_invalid();
            if (budget->max_requests > 0 && budget->requests_started >= (uint64_t)budget->max_requests) {
                code = BX_FETCH_EXIT_POLICY;
            } else {
                code = fetch_tree(arguments, config, budget, jv_string_value(sha), false, &subtree);
            }
            if (code == 0 && !tree_valid(subtree, jv_string_value(sha), true))
                code = BX_FETCH_EXIT_PROTOCOL;
            if (code == 0 && !account_value(subtree, &retained_bytes))
                code = BX_FETCH_EXIT_POLICY;
            if (code == 0)
                cache = jv_object_set(cache, jv_copy(sha), jv_copy(subtree));
        }
        if (code == 0 && is_truncated(subtree))
            code = BX_FETCH_EXIT_POLICY;
        jv entries = code == 0 ? field(subtree, "tree") : jv_array();
        int count = jv_array_length(jv_copy(entries));
        for (int i = 0; code == 0 && i < count; i++) {
            jv entry = jv_array_get(jv_copy(entries), i);
            jv path = field(entry, "path");
            char* full_path = NULL;
            const char* base = jv_string_value(prefix);
            if (asprintf(&full_path, "%s%s%s", base, *base ? "/" : "", jv_string_value(path)) < 0)
                full_path = NULL;
            jv_free(path);
            if (!full_path || strlen(full_path) > 4096 || jv_array_length(jv_copy(result)) >= 100000) {
                free(full_path); jv_free(entry);
                code = BX_FETCH_EXIT_POLICY;
                break;
            }
            entry = jv_object_set(entry, jv_string("path"), jv_string(full_path));
            jv kind = field(entry, "type"), child = field(entry, "sha");
            if (!account_value(entry, &retained_bytes)) {
                code = BX_FETCH_EXIT_POLICY;
            } else if (strcmp(jv_string_value(kind), "tree") == 0) {
                if (depth >= 128 || jv_array_length(jv_copy(queue)) >= 10000) {
                    code = BX_FETCH_EXIT_POLICY;
                } else {
                    queue = jv_array_append(queue, task(jv_string_value(child), full_path, depth + 1));
                }
            }
            if (code == 0)
                result = jv_array_append(result, jv_copy(entry));
            free(full_path); jv_free(kind); jv_free(child); jv_free(entry);
        }
        jv_free(entries); jv_free(subtree); jv_free(sha); jv_free(prefix); jv_free(current);
        if (code != 0)
            break;
    }
    if (code != 0 && jv_array_length(jv_copy(result)) == 0) {
        jv_free(result);
        result = field(*document, "tree");
    }
    *document = jv_object_set(*document, jv_string("tree"), result);
    *document = jv_object_set(*document, jv_string("truncated"), code == 0 ? jv_false() : jv_true());
    jv_free(root_sha); jv_free(queue); jv_free(cache);
    return code;
}

int bx_mira_github_tree(const MiraGithubArguments* arguments) {
    if (arguments->operand_count != 2 || !bx_mira_github_repo_is_valid(arguments->operands[0]) ||
        !arguments->operands[1][0] || arguments->jq_program || arguments->json) {
        fputs("mira: github tree requires OWNER/REPO REF [--recursive]; filtering is not supported\n", stderr);
        return BX_FETCH_EXIT_PARSE_OR_CONFIG;
    }
    const char* root = arguments->repository.api_root ? arguments->repository.api_root : MIRA_GITHUB_API_ROOT;
    struct bx_fetch_config* config = bx_mira_github_config(arguments, root, "-");
    if (!config)
        return BX_FETCH_EXIT_PARSE_OR_CONFIG;
    BxFetchBudget budget;
    if (bx_fetch_budget_init(&budget, config->download.max_requests, config->download.max_retry_time) != 0) {
        bx_fetch_config_free(config);
        return BX_FETCH_EXIT_FILE_IO;
    }
    jv document = jv_invalid();
    int result = fetch_tree(arguments, config, &budget, arguments->operands[1], arguments->recursive, &document);
    if (result == 0 && is_truncated(document)) {
        fputs("mira: github tree response is truncated\n", stderr);
        result = arguments->recursive ? expand_tree(arguments, config, &budget, &document) : BX_FETCH_EXIT_POLICY;
        if (result != 0)
            fputs("mira: github tree remains incomplete; truncated=true\n", stderr);
    }
    if (jv_is_valid(document) && (result == 0 || result == BX_FETCH_EXIT_POLICY)) {
        int output = bx_mira_repository_print_json(document, NULL);
        if (output != 0)
            result = output;
    } else {
        jv_free(document);
    }
    bx_fetch_config_free(config);
    return result;
}
