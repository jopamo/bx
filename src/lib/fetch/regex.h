#ifndef MIRA_REGEX_H
#define MIRA_REGEX_H

/* MIRA_HEADER_OWNER: util */
/* MIRA_HEADER_CONSUMERS: util, cli, policy */

/*
 * Layering contract:
 * - Regex flavor mapping is centralized so CLI and policy compile equivalent
 *   patterns with one flag mapping implementation.
 *
 * Ownership and lifetime:
 * - Inputs are borrowed.
 * - `flags_out` is caller-owned output storage.
 */

int mira_regex_compile_flags_for_type(const char *regex_type, int *flags_out);

#endif // MIRA_REGEX_H
