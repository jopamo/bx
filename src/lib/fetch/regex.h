#ifndef BX_FETCH_REGEX_H
#define BX_FETCH_REGEX_H

/* BX_FETCH_HEADER_OWNER: util */
/* BX_FETCH_HEADER_CONSUMERS: util, cli, policy */

/*
 * Layering contract:
 * - Regex flavor mapping is centralized so CLI and policy compile equivalent
 *   patterns with one flag mapping implementation.
 *
 * Ownership and lifetime:
 * - Inputs are borrowed.
 * - `flags_out` is caller-owned output storage.
 */

int bx_fetch_regex_compile_flags_for_type(const char* regex_type, int* flags_out);

#endif  // BX_FETCH_REGEX_H
