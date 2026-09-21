#ifndef BX_FETCH_ANUBIS_H
#define BX_FETCH_ANUBIS_H

/* BX_FETCH_HEADER_OWNER: runtime */
/* BX_FETCH_HEADER_CONSUMERS: runtime, net */

/*
 * Layering contract:
 * - Implements the client side of the bounded Anubis challenge protocol:
 *   challenge-page probing, JSON extraction, native challenge solving, and
 *   pass-challenge URL construction.
 * - No transport, policy, diagnostics, or filesystem state lives here.
 *
 * Ownership and lifetime:
 * - All functions borrow their inputs. The challenge struct is a value type.
 * - bx_fetch_anubis_pass_url() returns an owned string.
 */

#include "url.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bounded challenge-page window. Real pages keep the challenge script in the
 * document head, far inside this limit. */
#define BX_FETCH_ANUBIS_PROBE_LIMIT_BYTES ((size_t)32 * 1024u)
#define BX_FETCH_ANUBIS_PASS_PATH "/.within.website/x/cmd/anubis/api/pass-challenge"
#define BX_FETCH_ANUBIS_RANDOM_DATA_MAX_BYTES ((size_t)256)
#define BX_FETCH_ANUBIS_ID_MAX_BYTES ((size_t)64)
#define BX_FETCH_ANUBIS_ALGORITHM_MAX_BYTES ((size_t)32)
#define BX_FETCH_ANUBIS_BASE_PREFIX_MAX_BYTES ((size_t)256)
#define BX_FETCH_ANUBIS_DIFFICULTY_MAX 64
#define BX_FETCH_ANUBIS_DEFAULT_MAX_DIFFICULTY 6
#define BX_FETCH_ANUBIS_SHA256_DEFAULT_MAX_DIFFICULTY 24
#define BX_FETCH_ANUBIS_NONCE_MAX_BYTES 21u
#define BX_FETCH_ANUBIS_RESPONSE_MAX_BYTES (BX_FETCH_ANUBIS_RANDOM_DATA_MAX_BYTES + 1u)

typedef enum {
    BX_FETCH_ANUBIS_ALGORITHM_FAST = 0,
    BX_FETCH_ANUBIS_ALGORITHM_SLOW,
    BX_FETCH_ANUBIS_ALGORITHM_PREACT,
    BX_FETCH_ANUBIS_ALGORITHM_METAREFRESH,
    BX_FETCH_ANUBIS_ALGORITHM_SHA256,
} BxFetchAnubisAlgorithm;

typedef struct {
    char random_data[BX_FETCH_ANUBIS_RANDOM_DATA_MAX_BYTES + 1u];
    char id[BX_FETCH_ANUBIS_ID_MAX_BYTES + 1u];
    char algorithm[BX_FETCH_ANUBIS_ALGORITHM_MAX_BYTES + 1u];
    char base_prefix[BX_FETCH_ANUBIS_BASE_PREFIX_MAX_BYTES + 1u];
    BxFetchAnubisAlgorithm algorithm_kind;
    int difficulty;
} BxFetchAnubisChallenge;

typedef struct {
    char nonce[BX_FETCH_ANUBIS_NONCE_MAX_BYTES];
    char response[BX_FETCH_ANUBIS_RESPONSE_MAX_BYTES];
    uint64_t attempts;
    uint64_t minimum_wait_milliseconds;
} BxFetchAnubisSolution;

typedef enum {
    /* Not enough bytes to decide; ask for more of the body. */
    BX_FETCH_ANUBIS_PROBE_UNDECIDED = 0,
    /* No complete challenge script in the window. */
    BX_FETCH_ANUBIS_PROBE_NO_MATCH,
    /* A complete, well-formed challenge was extracted. */
    BX_FETCH_ANUBIS_PROBE_MATCH,
    /* A challenge script is present but malformed, incomplete at end of body,
     * or outside the supported protocol bounds. */
    BX_FETCH_ANUBIS_PROBE_INVALID,
} BxFetchAnubisProbeResult;

/*
 * Scans a growing body prefix for the embedded Anubis challenge. `final` means
 * no more bytes can arrive, so an incomplete window is decided. The caller
 * owns the probe window bound and must treat a full window as NO_MATCH.
 */
BxFetchAnubisProbeResult bx_fetch_anubis_probe(const char* prefix, size_t length, bool final, BxFetchAnubisChallenge* out);
/* Parses the HTTP Refresh form used by metarefresh challenges. */
BxFetchAnubisProbeResult bx_fetch_anubis_probe_refresh(const char* value, BxFetchAnubisChallenge* out);

/*
 * Solves one parsed challenge. fast/slow use leading zero nibbles over text.
 * sha256 uses leading zero bits over decoded challenge bytes and a binary
 * 32-bit nonce. preact hashes once and metarefresh echoes the challenge; both
 * report the protocol's minimum wait. The caller owns narrower limits.
 * Returns 0 on success or -1 with errno (EINVAL/EOVERFLOW).
 */
int bx_fetch_anubis_solve(const BxFetchAnubisChallenge* challenge, BxFetchAnubisSolution* solution);

/*
 * Builds the absolute same-origin pass-challenge URL for the challenge served
 * at `target`. `redir` is the userinfo-free canonical challenge-page URL.
 * Returns an owned string or NULL with errno set.
 */
char* bx_fetch_anubis_pass_url(const BxFetchPreparedUrl* target,
                               const BxFetchAnubisChallenge* challenge,
                               const BxFetchAnubisSolution* solution,
                               const char* elapsed_milliseconds);

#endif /* BX_FETCH_ANUBIS_H */
