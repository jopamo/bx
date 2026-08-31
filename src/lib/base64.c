#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lib/base64.h"
#include "lib/base64_internal.h"

const uint8_t bx_base64_alphabet[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t bx_base64_encoded_size(size_t input_len) {
    if (input_len > SIZE_MAX - 2u) {
        return SIZE_MAX;
    }
    size_t groups = (input_len + 2u) / 3u;
    if (groups > SIZE_MAX / 4u) {
        return SIZE_MAX;
    }
    return groups * 4u;
}

size_t bx_base64_encode_complete(const uint8_t* input, size_t input_len, char* output) {
    size_t complete_len = input_len - (input_len % 3u);
    size_t consumed = bx_base64_arm64_encode_blocks(input, complete_len, output);
    size_t out_pos = (consumed / 3u) * 4u;

    for (size_t i = consumed; i < complete_len; i += 3u) {
        uint8_t a = input[i];
        uint8_t b = input[i + 1u];
        uint8_t c = input[i + 2u];
        output[out_pos++] = (char)bx_base64_alphabet[a >> 2u];
        output[out_pos++] = (char)bx_base64_alphabet[((a & 0x03u) << 4u) | (b >> 4u)];
        output[out_pos++] = (char)bx_base64_alphabet[((b & 0x0fu) << 2u) | (c >> 6u)];
        output[out_pos++] = (char)bx_base64_alphabet[c & 0x3fu];
    }
    return out_pos;
}

size_t bx_base64_encode(const uint8_t* input, size_t input_len, char* output) {
    size_t complete_len = input_len - (input_len % 3u);
    size_t out_pos = bx_base64_encode_complete(input, complete_len, output);
    size_t tail_len = input_len - complete_len;

    if (tail_len == 1u) {
        uint8_t a = input[complete_len];
        output[out_pos++] = (char)bx_base64_alphabet[a >> 2u];
        output[out_pos++] = (char)bx_base64_alphabet[(a & 0x03u) << 4u];
        output[out_pos++] = '=';
        output[out_pos++] = '=';
    }
    else if (tail_len == 2u) {
        uint8_t a = input[complete_len];
        uint8_t b = input[complete_len + 1u];
        output[out_pos++] = (char)bx_base64_alphabet[a >> 2u];
        output[out_pos++] = (char)bx_base64_alphabet[((a & 0x03u) << 4u) | (b >> 4u)];
        output[out_pos++] = (char)bx_base64_alphabet[(b & 0x0fu) << 2u];
        output[out_pos++] = '=';
    }
    return out_pos;
}

int bx_base64_decode_value(unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return (int)(ch - 'A');
    }
    if (ch >= 'a' && ch <= 'z') {
        return (int)(ch - 'a') + 26;
    }
    if (ch >= '0' && ch <= '9') {
        return (int)(ch - '0') + 52;
    }
    if (ch == '+') {
        return 62;
    }
    if (ch == '/') {
        return 63;
    }
    return -1;
}

static bool bx_base64_decode_quad(const unsigned char input[4], uint8_t output[3]) {
    int a = bx_base64_decode_value(input[0]);
    int b = bx_base64_decode_value(input[1]);
    int c = bx_base64_decode_value(input[2]);
    int d = bx_base64_decode_value(input[3]);

    if (a < 0 || b < 0 || c < 0 || d < 0) {
        return false;
    }
    output[0] = (uint8_t)((a << 2) | (b >> 4));
    output[1] = (uint8_t)((b << 4) | (c >> 2));
    output[2] = (uint8_t)((c << 6) | d);
    return true;
}

size_t bx_base64_decode_blocks(const unsigned char* input, size_t input_len, uint8_t* output) {
    size_t in_pos = 0u;
    size_t out_pos = 0u;

    while (input_len - in_pos >= 64u) {
        if (!bx_base64_arm64_decode_64(input + in_pos, output + out_pos)) {
            break;
        }
        in_pos += 64u;
        out_pos += 48u;
    }
    while (input_len - in_pos >= 4u) {
        if (!bx_base64_decode_quad(input + in_pos, output + out_pos)) {
            break;
        }
        in_pos += 4u;
        out_pos += 3u;
    }
    return in_pos;
}

bool bx_base64_decode_exact(const char* input, size_t input_len, uint8_t* output, size_t* output_len) {
    if (input_len == 0u) {
        *output_len = 0u;
        return true;
    }
    if ((input_len % 4u) != 0u) {
        return false;
    }

    size_t padding = 0u;
    if (input[input_len - 1u] == '=') {
        padding++;
    }
    if (input_len >= 2u && input[input_len - 2u] == '=') {
        padding++;
    }
    size_t prefix_len = input_len - 4u;
    size_t consumed = bx_base64_decode_blocks((const unsigned char*)input, prefix_len, output);
    if (consumed != prefix_len) {
        return false;
    }

    const unsigned char* q = (const unsigned char*)input + prefix_len;
    int a = bx_base64_decode_value(q[0]);
    int b = bx_base64_decode_value(q[1]);
    int c = q[2] == '=' ? 0 : bx_base64_decode_value(q[2]);
    int d = q[3] == '=' ? 0 : bx_base64_decode_value(q[3]);
    if (a < 0 || b < 0 || c < 0 || d < 0 || (q[2] == '=' && q[3] != '=') || (q[2] == '=' && padding != 2u) || (q[3] == '=' && padding == 0u)) {
        return false;
    }

    size_t out_pos = (prefix_len / 4u) * 3u;
    output[out_pos++] = (uint8_t)((a << 2) | (b >> 4));
    if (padding < 2u) {
        output[out_pos++] = (uint8_t)((b << 4) | (c >> 2));
    }
    if (padding == 0u) {
        output[out_pos++] = (uint8_t)((c << 6) | d);
    }
    *output_len = out_pos;
    return true;
}
