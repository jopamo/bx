# bx native ZIP mechanics

This directory contains the policy-light ZIP format, compression, context, and
I/O mechanics integrated from `zip-utils` commit
`3f37f68aee3e5f7dca2cab7c6b012f881621bc4e` (2026-08-14).

The code is compiled directly into the bx multicall binary. It does not build,
spawn, or dispatch through standalone `zip-utils` executables. User-facing
argument grammar and diagnostics remain in:

- `src/applets/archive/zip/zip.c`
- `src/applets/archive/unzip/unzip.c` (`zipinfo` shares this frontend by argv0)

bx modifications are an altered implementation and are not an official
Info-ZIP release. See `LICENSE.InfoZIP`.
