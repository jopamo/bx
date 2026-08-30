#include "lib/jq/pcre2_backend.h"

#include <stddef.h>
#include <string.h>

#include <pcre2.h>

#include "jv_unicode.h"

static long long bx_jq_utf8_codepoint_offset(const char *input,
                                             PCRE2_SIZE byte_offset) {
    long long idx = 0;
    const char *cursor = input;
    const char *limit = input + byte_offset;

    while (cursor < limit) {
        int advance = jvp_utf8_decode_length(*cursor);
        if (advance <= 0)
            advance = 1;
        cursor += advance;
        idx++;
    }

    return idx;
}

static PCRE2_SIZE bx_jq_next_start_offset(const char *input,
                                          PCRE2_SIZE length,
                                          PCRE2_SIZE current) {
    if (current >= length)
        return length + 1;

    int advance = jvp_utf8_decode_length(input[current]);
    if (advance <= 0)
        advance = 1;
    return current + (PCRE2_SIZE)advance;
}

static jv bx_jq_regex_failure_from_code(int errcode) {
    PCRE2_UCHAR errbuf[256];
    int msg_rc = pcre2_get_error_message(errcode, errbuf,
                                         sizeof(errbuf) / sizeof(errbuf[0]));

    if (msg_rc >= 0) {
        return jv_invalid_with_msg(
            jv_string_concat(jv_string("Regex failure: "),
                             jv_string((const char *)errbuf)));
    }

    return jv_invalid_with_msg(
        jv_string("Regex failure: unknown PCRE2 error"));
}

static jv bx_jq_capture_object(const char *input_string,
                               PCRE2_SIZE begin,
                               PCRE2_SIZE end) {
    if (begin == PCRE2_UNSET || end == PCRE2_UNSET) {
        jv cap = jv_object_set(jv_object(), jv_string("offset"), jv_number(-1));
        cap = jv_object_set(cap, jv_string("length"), jv_number(0));
        cap = jv_object_set(cap, jv_string("string"), jv_null());
        cap = jv_object_set(cap, jv_string("name"), jv_null());
        return cap;
    }

    long long offset = bx_jq_utf8_codepoint_offset(input_string, begin);
    long long length = bx_jq_utf8_codepoint_offset(input_string, end) - offset;
    jv cap = jv_object_set(jv_object(), jv_string("offset"), jv_number(offset));
    cap = jv_object_set(cap, jv_string("length"), jv_number(length));
    cap = jv_object_set(cap, jv_string("string"),
                        jv_string_sized(input_string + begin, (int)(end - begin)));
    cap = jv_object_set(cap, jv_string("name"), jv_null());
    return cap;
}

static jv bx_jq_apply_capture_names(jv captures, pcre2_code *code) {
    uint32_t name_count = 0;
    uint32_t entry_size = 0;
    PCRE2_SPTR name_table = NULL;

    if (pcre2_pattern_info(code, PCRE2_INFO_NAMECOUNT, &name_count) != 0 ||
        name_count == 0)
        return captures;

    if (pcre2_pattern_info(code, PCRE2_INFO_NAMEENTRYSIZE, &entry_size) != 0)
        return captures;
    if (pcre2_pattern_info(code, PCRE2_INFO_NAMETABLE, &name_table) != 0)
        return captures;

    for (uint32_t i = 0; i < name_count; i++) {
        PCRE2_SPTR entry = name_table + (i * entry_size);
        uint16_t group = (uint16_t)(((uint16_t)entry[0] << 8) | entry[1]);
        if (group == 0)
            continue;

        size_t name_len = strnlen((const char *)(entry + 2), entry_size - 2);
        jv cap = jv_array_get(jv_copy(captures), group - 1);
        if (jv_get_kind(cap) == JV_KIND_OBJECT) {
            cap = jv_object_set(cap, jv_string("name"),
                                jv_string_sized((const char *)(entry + 2),
                                                (int)name_len));
            captures = jv_array_set(captures, group - 1, cap);
        } else {
            jv_free(cap);
        }
    }

    return captures;
}

jv bx_jq_pcre2_match(jv input,
                     jv regex,
                     int test,
                     uint32_t compile_options,
                     uint32_t match_options,
                     bool global,
                     bool longest) {
    const char *input_string = jv_string_value(input);
    PCRE2_SIZE input_length = (PCRE2_SIZE)jv_string_length_bytes(jv_copy(input));
    int errcode = 0;
    PCRE2_SIZE erroffset = 0;
    pcre2_compile_context *compile_ctx = pcre2_compile_context_create(NULL);
    pcre2_code *code = NULL;
    pcre2_match_data *match_data = NULL;
    jv result = jv_invalid();
    uint32_t capture_count = 0;
    PCRE2_SIZE start = 0;

    (void)longest;

    if (compile_ctx != NULL)
        (void)pcre2_set_parens_nest_limit(compile_ctx, 1024);

    code = pcre2_compile((PCRE2_SPTR)jv_string_value(regex),
                         PCRE2_ZERO_TERMINATED,
                         compile_options | PCRE2_UTF | PCRE2_UCP,
                         &errcode,
                         &erroffset,
                         compile_ctx);
    if (compile_ctx != NULL)
        pcre2_compile_context_free(compile_ctx);
    compile_ctx = NULL;

    if (code == NULL) {
        result = bx_jq_regex_failure_from_code(errcode);
        goto out;
    }

    (void)pcre2_pattern_info(code, PCRE2_INFO_CAPTURECOUNT, &capture_count);
    (void)pcre2_jit_compile(code, PCRE2_JIT_COMPLETE);

    match_data = pcre2_match_data_create_from_pattern(code, NULL);
    if (match_data == NULL) {
        result = jv_invalid_with_msg(
            jv_string("Regex failure: unable to allocate match data"));
        goto out;
    }

    result = test ? jv_false() : jv_array();
    do {
        int rc = pcre2_match(code,
                             (PCRE2_SPTR)input_string,
                             input_length,
                             start,
                             match_options | PCRE2_NO_UTF_CHECK,
                             match_data,
                             NULL);
        if (rc == PCRE2_ERROR_NOMATCH) {
            break;
        }
        if (rc < 0) {
            jv_free(result);
            result = bx_jq_regex_failure_from_code(rc);
            break;
        }

        if (test) {
            jv_free(result);
            result = jv_true();
            break;
        }

        PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
        PCRE2_SIZE match_begin = ovector[0];
        PCRE2_SIZE match_end = ovector[1];
        long long offset = bx_jq_utf8_codepoint_offset(input_string, match_begin);
        long long length = bx_jq_utf8_codepoint_offset(input_string, match_end) - offset;

        jv match = jv_object_set(jv_object(), jv_string("offset"), jv_number(offset));
        match = jv_object_set(match, jv_string("length"), jv_number(length));
        match = jv_object_set(
            match,
            jv_string("string"),
            jv_string_sized(input_string + match_begin,
                            (int)(match_end - match_begin)));

        jv captures = jv_array();
        for (uint32_t group = 1; group <= capture_count; group++) {
            PCRE2_SIZE cap_begin = ovector[group * 2];
            PCRE2_SIZE cap_end = ovector[(group * 2) + 1];
            captures = jv_array_append(
                captures,
                bx_jq_capture_object(input_string, cap_begin, cap_end));
        }
        captures = bx_jq_apply_capture_names(captures, code);
        match = jv_object_set(match, jv_string("captures"), captures);
        result = jv_array_append(result, match);

        if (match_end == match_begin) {
            start = bx_jq_next_start_offset(input_string, input_length, match_end);
        } else {
            start = match_end;
        }
    } while (global && start <= input_length);

out:
    if (match_data != NULL)
        pcre2_match_data_free(match_data);
    if (code != NULL)
        pcre2_code_free(code);
    jv_free(input);
    jv_free(regex);
    return result;
}
