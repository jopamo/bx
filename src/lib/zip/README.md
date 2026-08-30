# bx native ZIP mechanics

This directory contains the policy-light ZIP format, compression, context, and
I/O mechanics integrated from `zip-utils` commit
`3f37f68aee3e5f7dca2cab7c6b012f881621bc4e` (2026-08-14).

The code is compiled directly into the bx multicall binary. It does not build,
spawn, or dispatch through standalone `zip-utils` executables. User-facing
argument grammar and diagnostics remain in applet policy modules:

- `src/applets/archive/zip/zip.c`: thin lifecycle entrypoint
- `src/applets/archive/zip/zip_parse.c`: zip/zipnote option grammar and help
- `src/applets/archive/zip/zipnote.c`: zipnote edit-stream policy
- `src/applets/archive/unzip/unzip.c`: thin lifecycle entrypoint
- `src/applets/archive/unzip/unzip_parse.c`: unzip/zipinfo grammar and help

Shared boundaries:

- `comments.c` owns archive comment mutation and publication.
- `extract_path.c` owns ZIP extraction-path construction and uses bx
  `path_ops`/`fd_ops` for generic path and syscall mechanics.
- `input_walk.c` owns ZIP operand traversal and filtering semantics.
- `ops.c` uses bx `argv_packer` and `child_runner`; no shell is involved.
- `publish.c` uses bx `copy_data`, `fd_ops`, and `path_ops` for
  cross-filesystem publication mechanics; the writer only selects when to
  publish.
- `cli_common.c` shares status mapping and delegates color/path mechanics to
  bx-wide libraries.

bx modifications are an altered implementation and are not an official
Info-ZIP release. See `LICENSE.InfoZIP`.
