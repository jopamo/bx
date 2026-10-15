# sed candidate source inventory

`upstream.json` pins the GNU sed candidate and its gnulib gitlink. No engine
source is imported or built here yet. The execution contract and remaining
work live in the workspace's `sed.md`, outside this repository.

The archive URLs are commit-specific. Verify their SHA-256 hashes before
extracting them. The GitHub tree API for the pinned sed commit independently
identifies `gnulib` as a mode-160000 gitlink with the recorded commit.
The coreutils gnulib archive is a mirror of that object, not a replacement
dependency selected from its current branch.

## Dependency closure

The inventory records the 52 modules requested by upstream `bootstrap.conf`,
the `lock` exclusion, all 564 resolved modules including tests, and the 1116
file entries in the generated `gl_FILE_LIST`. These are a conservative source
closure, not a list of translation units to link. Configure selects platform
replacements; test and release tooling must not become applet dependencies.
`tests=lib/` entries refer to library files copied into the test area.

To reproduce the import, extract both verified archives into separate
directories. In the sed source directory run the pinned gnulib tree's
`gnulib-tool --import`, followed by the `import.options` and `import.modules`
arrays in order. Use `GNULIB_TOOL_IMPL=py`. The relative `--dir=.` is deliberate:
absolute paths appear in generated macro comments. Do not run an unpinned
bootstrap or let a normal bx build fetch dependencies.

The four `generated_import_files` hashes cover gnulib's generated import
metadata and makefiles. They reproduced byte-for-byte in two separate source
directories on 2026-10-15. The post-import copies from upstream's hook are
listed separately; they are not generated source. The hook also creates an
empty `ChangeLog`.

Autoconf/Automake/gettext outputs, configured wrapper headers and sed version
files are not yet generated. Their inputs include `configure.ac`,
`Makefile.am`, imported m4 macros and header templates, and `sed/local.mk`.
`config.h` and configured wrappers are target-dependent and must not be copied
from a development host as a portable bx configuration.

## Notices and integration boundary

The six sed engine translation units and their headers carry GPL-3.0-or-later
notices. The gnulib module license labels are preserved verbatim in the JSON;
they are not SPDX identifiers or a substitute for individual file notices.
All 228 modules without a License field are test modules. Examples checked
directly include GPL-3.0-or-later `lib/dfa.c` and LGPL-2.1-or-later
`lib/regex.c`. Build-tool exceptions, permissive helper notices and test-file
notices must remain with their files when importing. No upstream source or
license text has been copied into this directory.

No local source patches exist at this inventory stage. Compile/link,
namespacing, excluded command handlers, target configuration and Issue 8
qualification are still open. In particular, the module name `regex` does
not establish support for Issue 8 shortest-match ERE repetition modifiers.
