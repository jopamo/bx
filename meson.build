project('bx', 'c',
  meson_version : '>=1.6.0',
  default_options : ['warning_level=3', 'c_std=c11', 'werror=true']
)

python3 = find_program('python3', required : true)
bx_version_source = vcs_tag(
  input : 'src/bx/version.c.in',
  output : 'bx-version.c',
  fallback : 'unknown-00000000',
  command : [
    python3,
    join_paths(meson.project_source_root(), 'tools', 'version', 'generate.py'),
    meson.project_source_root(),
    get_option('build_version'),
  ],
)
add_project_arguments(
  '-D_POSIX_C_SOURCE=200809L',
  '-include',
  'bx/version.h',
  language : 'c',
)
init_kill_removed = get_option('init_kill_removed')
init_kill_delay = get_option('init_kill_delay')
if not init_kill_removed and init_kill_delay != 0
  error('init_kill_delay requires init_kill_removed=true')
endif
add_project_arguments(
  '-DBX_INIT_FEATURE_KILL_REMOVED=' + (init_kill_removed ? '1' : '0'),
  '-DBX_INIT_FEATURE_KILL_DELAY=' + init_kill_delay.to_string(),
  language : 'c',
)

lscolors_install_subdir = '/etc'
lscolors_install_path = join_paths(lscolors_install_subdir, 'LS_COLORS')
add_project_arguments('-DBX_LSCOLORS_FILE="' + lscolors_install_path + '"', language : 'c')

screen_utf8_install_subdir = join_paths(get_option('datadir'), 'bx', 'screen', 'utf8encodings')
screen_utf8_install_path = join_paths(get_option('prefix'), screen_utf8_install_subdir)
screen_system_rc_path = join_paths(get_option('prefix'), get_option('sysconfdir'), 'bx', 'screenrc')
edit_vim_runtime_install_subdir = join_paths(get_option('datadir'), 'bx', 'vim')
edit_vim_runtime_install_path = join_paths(get_option('prefix'), edit_vim_runtime_install_subdir)
edit_vim_runtime_source_path = join_paths(meson.project_source_root(), 'src', 'applets', 'text', 'edit', 'runtime')
stdbuf_install_subdir = join_paths(get_option('libexecdir'), 'bx')
stdbuf_installed_path = join_paths(get_option('prefix'), stdbuf_install_subdir, 'libstdbuf.so')
add_project_arguments('-DBX_STDBUF_INSTALLED_PATH="' + stdbuf_installed_path + '"', language : 'c')
add_project_arguments('-DBX_STDBUF_INSTALL_SUBDIR="' + stdbuf_install_subdir + '"', language : 'c')

cc = meson.get_compiler('c')
can_run = meson.can_run_host_binaries()
have_updwtmpx = cc.has_function(
  'updwtmpx',
  prefix : '#define _GNU_SOURCE\n#include <utmpx.h>',
)
if have_updwtmpx
  add_project_arguments('-DBX_HAVE_UPDWTMPX=1', language : 'c')
else
  add_project_arguments('-DBX_HAVE_UPDWTMPX=0', language : 'c')
endif
arm64_sve_intrinsics = cc.compiles(
  '''
  #if !defined(__aarch64__) || !defined(__ARM_FEATURE_SVE)
  #error "SVE intrinsics not enabled for this target"
  #endif
  #include <arm_sve.h>
  int main(void) {
    svbool_t pg = svptrue_b8();
    return svptest_any(pg, pg) ? 0 : 0;
  }
  ''',
  name : 'arm64 sve intrinsics',
)
if arm64_sve_intrinsics
  add_project_arguments('-DBX_LITERAL_HAVE_ARM64_SVE_INTRINSICS=1', language : 'c')
else
  add_project_arguments('-DBX_LITERAL_HAVE_ARM64_SVE_INTRINSICS=0', language : 'c')
endif
warn_flags = [
  '-Wshadow',
  '-Wstrict-prototypes',
  '-Wmissing-prototypes',
  '-Wpointer-arith',
  '-Wcast-qual',
]
add_project_arguments(cc.get_supported_arguments(warn_flags), language : 'c')

static_binary_opt = get_option('static_binary')
compiler_targets_glibc = cc.has_header_symbol('features.h', '__GLIBC__')
musl_gcc = find_program('musl-gcc', required : false)
static_link_probe_src = 'int main(void) { return 0; }'
static_link_probe_args = []

# Probe the exact static-link argument combination we will pass later. Some
# toolchains only produce a working fully static executable with a particular
# mode: plain -static, -static combined with -no-pie, or static PIE via
# -static-pie. Checking only one form can reject a usable clang/musl setup.
if cc.links(
  static_link_probe_src,
  args : ['-static', '-no-pie'],
  name : 'static link with -no-pie',
)
  static_link_probe_args = ['-static', '-no-pie']
elif cc.links(
  static_link_probe_src,
  args : ['-static'],
  name : 'static link',
)
  static_link_probe_args = ['-static']
elif cc.links(
  static_link_probe_src,
  args : ['-static-pie'],
  name : 'static pie link',
)
  static_link_probe_args = ['-static-pie']
endif
static_link_supported = static_link_probe_args.length() != 0

use_static_binary = false
if static_binary_opt == 'enabled'
  if not static_link_supported
    error('static_binary=enabled requires a toolchain that supports a fully static link via -static or -static-pie')
  endif
  use_static_binary = true
elif static_binary_opt == 'auto'
  if compiler_targets_glibc
    if musl_gcc.found()
      warning('static_binary=auto: musl-gcc is available but this configure did not select it; using shared linkage. Re-run setup with CC=musl-gcc if you want musl static linkage.')
    endif
  elif static_link_supported
    use_static_binary = true
  else
    warning('static_binary=auto: toolchain lacks -static; using shared linkage')
  endif
endif

bx_static_link_args = []
if use_static_binary
  bx_static_link_args = static_link_probe_args
endif
bx_build_id_link_args = []
if host_machine.system() == 'linux'
  bx_build_id_link_args = cc.get_supported_link_arguments([
    '-Wl,--build-id=sha1',
  ])
  if bx_build_id_link_args.length() == 0
    error('Linux bx requires linker build-id support for exact self-exec identity')
  endif
endif
bx_link_args = bx_static_link_args + bx_build_id_link_args

mira_embed_opt = get_option('mira_embed')
mira_embed_dep = declare_dependency()
mira_embed_enabled = false
if mira_embed_opt != 'disabled'
  # mira-embed ships as a static library, so consumers always need the
  # full transitive link set from pkg-config --static, even when bx itself is
  # built as a dynamically linked executable.
  pkgconf = find_program('pkg-config', required : true)
  mira_embed_exists = run_command(pkgconf, '--exists', 'mira-embed', check : false).returncode() == 0
  if mira_embed_exists
    mira_embed_cflags_str = run_command(pkgconf, '--cflags', 'mira-embed', check : true).stdout().strip()
    mira_embed_libs_str = run_command(pkgconf, '--libs', '--static', 'mira-embed', check : true).stdout().strip()

    mira_embed_cflags = []
    if mira_embed_cflags_str != ''
      mira_embed_cflags = mira_embed_cflags_str.split()
    endif

    mira_embed_libs = []
    if mira_embed_libs_str != ''
      mira_embed_libs = mira_embed_libs_str.split()
    endif

    mira_embed_dep = declare_dependency(
      compile_args : mira_embed_cflags,
      link_args : mira_embed_libs,
    )
    mira_embed_enabled = true
  elif mira_embed_opt == 'enabled'
    error('mira-embed was not found via pkg-config')
  endif
endif

if mira_embed_enabled
  mira_embed_has_run_argv = cc.has_function(
    'mira_embed_run_argv',
    prefix : '#include <mira/embed.h>',
    dependencies : [mira_embed_dep],
  )
  if not mira_embed_has_run_argv
    if mira_embed_opt == 'enabled'
      error('mira-embed found but missing mira_embed_run_argv; reinstall a newer mira-embed static library')
    endif
    warning('mira-embed found but missing mira_embed_run_argv; disabling wget mira-embed backend')
    mira_embed_enabled = false
    mira_embed_dep = declare_dependency()
  endif
endif

if mira_embed_enabled and use_static_binary
  mira_embed_static_link_ok = cc.links(
    '''
    #include <mira/embed.h>
    int main(void) {
      char *argv[] = {"bx", 0};
      return mira_embed_run_argv(1, argv);
    }
    ''',
    dependencies : [mira_embed_dep],
    args : bx_static_link_args,
    name : 'mira-embed static linkability probe',
  )
  if not mira_embed_static_link_ok
    if mira_embed_opt == 'enabled'
      error('mira-embed found but is not fully static-linkable with this toolchain/dependency set; provide static libcurl transitive dependencies or reconfigure with -Dstatic_binary=disabled')
    endif
    warning('mira-embed found but is not fully static-linkable with the current libcurl dependency set; disabling wget mira-embed backend (reconfigure with -Dstatic_binary=disabled or provide static libcurl transitive dependencies)')
    mira_embed_enabled = false
    mira_embed_dep = declare_dependency()
  endif
endif

if mira_embed_enabled
  add_project_arguments('-DBX_HAVE_MIRA_EMBED=1', language : 'c')
else
  add_project_arguments('-DBX_HAVE_MIRA_EMBED=0', language : 'c')
endif

threads_dep = dependency('threads')
pcre2_dep = dependency('libpcre2-8', static: use_static_binary)
zlib_dep = dependency('zlib', required : false, static: use_static_binary)
if not zlib_dep.found()
  zlib_dep = cc.find_library('z', required : true)
endif

libbz2_dep = cc.find_library('bz2', required : false, static: use_static_binary)
if not libbz2_dep.found() and not use_static_binary
  libbz2_dep = dependency('bzip2', required : false, static: false)
endif
if not libbz2_dep.found() and not use_static_binary
  libbz2_dep = cc.find_library('bz2', required : false)
endif
if not libbz2_dep.found()
  if use_static_binary
    error('libbz2 is required; bx man/mandoc must not silently build without bzip2-compressed manual page support. Provide a static libbz2 or reconfigure with -Dstatic_binary=disabled.')
  endif
  error('libbz2 is required; bx man/mandoc must not silently build without bzip2-compressed manual page support.')
endif
add_project_arguments('-DBX_HAVE_LIBBZ2=1', language : 'c')

liblzma_dep = dependency('liblzma', required : false, static: use_static_binary)
if not liblzma_dep.found()
  liblzma_dep = cc.find_library('lzma', required : false)
endif
if not liblzma_dep.found()
  if use_static_binary
    error('liblzma is required; bx man/mandoc must not silently build without xz-compressed manual page support. Provide a static liblzma or reconfigure with -Dstatic_binary=disabled.')
  endif
  error('liblzma is required; bx man/mandoc must not silently build without xz-compressed manual page support.')
endif
add_project_arguments('-DBX_HAVE_LIBLZMA=1', language : 'c')

libzstd_dep = dependency('libzstd', required : false, static: use_static_binary)
if not libzstd_dep.found()
  libzstd_dep = cc.find_library('zstd', required : false)
endif
if not libzstd_dep.found()
  if use_static_binary
    error('libzstd is required; bx man/mandoc must not silently build without zstd-compressed manual page support. Provide a static libzstd or reconfigure with -Dstatic_binary=disabled.')
  endif
  error('libzstd is required; bx man/mandoc must not silently build without zstd-compressed manual page support.')
endif
add_project_arguments('-DBX_HAVE_LIBZSTD=1', language : 'c')

bx_deps = [cc.find_library('m', required : false), threads_dep, pcre2_dep, zlib_dep]
if libbz2_dep.found()
  bx_deps += [libbz2_dep]
endif
if liblzma_dep.found()
  bx_deps += [liblzma_dep]
endif
if libzstd_dep.found()
  bx_deps += [libzstd_dep]
endif
if mira_embed_enabled
  bx_deps += [mira_embed_dep]
endif

# shared bx runtime and helpers
bx_core_sources = [
  bx_version_source,
  'src/bx/main.c',
  'src/bx/applet_profile.c',
  'src/bx/runtime_snapshot.c',
  'src/bx/self_exec.c',
  'src/bx/libbx.c',
]

dispatch_tables_src = custom_target(
  'bx-dispatch-tables-c',
  input : 'src/dispatch/applets.json',
  output : 'bx_dispatch_tables.c',
  command : [
    python3,
    files('tools/dispatch/generate_dispatch_tables.py'),
    '@INPUT@',
    '@OUTPUT@',
  ],
  depend_files : files(
    'tools/dispatch/generate_dispatch_tables.py',
  ),
)

bx_core_sources += [dispatch_tables_src]

bx_lib_sources = [
  'src/lib/arm64_features.c',
  'src/lib/base64.c',
  'src/lib/base64_arm64.c',
  'src/lib/cli_common.c',
  'src/lib/args_common.c',
  'src/lib/argv_packer.c',
  'src/lib/backpressure_limit.c',
  'src/lib/backup_ops.c',
  'src/lib/byte_count.c',
  'src/lib/byte_count_arm64.c',
  'src/lib/byte_transform.c',
  'src/lib/byte_transform_arm64.c',
  'src/lib/bx_seq.c',
  'src/lib/cancel_state.c',
  'src/lib/color.c',
  'src/lib/child_runner.c',
  'src/lib/copy_data.c',
  'src/lib/copy_metadata.c',
  'src/lib/copy_tree.c',
  'src/lib/dir_cycle.c',
  'src/lib/daemon_ops.c',
  'src/lib/fd_ops.c',
  'src/lib/fd_transaction.c',
  'src/lib/fault_injection.c',
  'src/lib/file_info_fmt.c',
  'src/lib/fopen_dash.c',
  'src/lib/fmt_engine.c',
  'src/lib/id_parse.c',
  'src/lib/line_writer.c',
  'src/lib/line_writer_file.c',
  'src/lib/mode_parse.c',
  'src/lib/mount_table.c',
  'src/lib/manual_runtime.c',
  'src/lib/output_alloc_counter.c',
  'src/lib/output_adapter.c',
  'src/lib/output_policy.c',
  'src/lib/output_profile_counter.c',
  'src/lib/output_sink.c',
  'src/lib/output_publication.c',
  'src/lib/ordered_publication.c',
  'src/lib/output.c',
  'src/lib/output_quote.c',
  'src/lib/overwrite_ops.c',
  'src/lib/path_ops.c',
  'src/lib/path_quote.c',
  'src/lib/pidfile_ops.c',
  'src/lib/preload_ops.c',
  'src/lib/prompt_ops.c',
  'src/lib/random_bytes.c',
  'src/lib/remove_ops.c',
  'src/lib/same_file.c',
  'src/lib/sandbox_explain.c',
  'src/lib/signal_names.c',
  'src/lib/size_parse.c',
  'src/lib/text_buffer.c',
  'src/lib/stat_ops.c',
  'src/lib/statx_compat.c',
  'src/lib/system/syslog_config.c',
  'src/lib/system/syslog_core.c',
  'src/lib/syscall_counter.c',
  'src/lib/thread_count.c',
  'src/lib/time_parse.c',
  'src/lib/trace.c',
  'src/lib/tty_speed.c',
  'src/lib/update_policy.c',
  'src/lib/utmp_ops.c',
  'src/lib/workqueue_contract.c',
  'src/lib/workqueue_profile.c',
  'src/lib/work_pool.c',
  'src/lib/xreadwrite.c',
]

bx_crypto_sources = [
  'src/crypto/crc32b.c',
  'src/crypto/crc32b_arm64.c',
  'src/crypto/digest_util.c',
  'src/crypto/digestsum.c',
  'src/crypto/md5.c',
  'src/crypto/sha1.c',
  'src/crypto/sha1_arm64.c',
  'src/crypto/sha256.c',
  'src/crypto/sha256_arm64.c',
  'src/crypto/sha256_x86.c',
  'src/crypto/sha512.c',
]

bx_archive_applet_sources = [
  'src/applets/archive/archive_bzip2.c',
  'src/applets/archive/archive_codec.c',
  'src/applets/archive/archive_common.c',
  'src/applets/archive/archive_gzip.c',
  'src/applets/archive/archive_xz.c',
  'src/applets/archive/archive_zstd.c',
  'src/applets/archive/archive_temp.c',
  'src/applets/archive/archive_fs.c',
  'src/applets/archive/cpio/cpio.c',
  'src/applets/archive/cpio/cpio_backend.c',
  'src/applets/archive/tar/tar.c',
  'src/applets/archive/tar/tar_backend.c',
  'src/applets/archive/tar/tar_create.c',
  'src/applets/archive/tar/tar_dumpdir.c',
  'src/applets/archive/tar/tar_files_from.c',
  'src/applets/archive/tar/tar_id_map.c',
  'src/applets/archive/tar/tar_incremental.c',
  'src/applets/archive/tar/tar_names.c',
  'src/applets/archive/tar/tar_patterns.c',
  'src/applets/archive/tar/tar_report.c',
  'src/applets/archive/tar/tar_reader.c',
  'src/applets/archive/tar/tar_select.c',
  'src/applets/archive/tar/tar_stream.c',
]

bx_zip_include_dirs = include_directories(
  'src',
  'src/lib/zip',
  'src/lib/zip/include',
  'src/lib/zip/common',
  'src/lib/zip/compression',
  'src/lib/zip/format',
)

bx_zip_sources = files(
  'src/lib/zip/common/status.c',
  'src/lib/zip/common/strlist.c',
  'src/lib/zip/common/ctx.c',
  'src/lib/zip/common/fileio.c',
  'src/lib/zip/compression/buffer_utils.c',
  'src/lib/zip/compression/zlib_shim.c',
  'src/lib/zip/compression/bzip2_shim.c',
  'src/lib/zip/compression/crc32.c',
  'src/lib/zip/compression/zipcrypto.c',
  'src/lib/zip/format/reader.c',
  'src/lib/zip/format/writer.c',
  'src/lib/zip/format/recovery.c',
  'src/lib/zip/cli_common.c',
  'src/lib/zip/comments.c',
  'src/lib/zip/extract_path.c',
  'src/lib/zip/input_walk.c',
  'src/lib/zip/ops.c',
  'src/lib/zip/publish.c',
  'src/applets/archive/zip/zip.c',
  'src/applets/archive/zip/zip_parse.c',
  'src/applets/archive/zip/zipnote.c',
  'src/applets/archive/unzip/unzip.c',
  'src/applets/archive/unzip/unzip_parse.c',
)

bx_zip_lib = static_library(
  'bx_zip',
  bx_zip_sources,
  include_directories : bx_zip_include_dirs,
  dependencies : [zlib_dep, libbz2_dep],
  # zlib and libbz2 expose historically non-const input pointer types.
  c_args : cc.get_supported_arguments(['-Wno-cast-qual']),
)

# applet groups
bx_base_applet_sources = [
  'src/applets/base/base64/base64.c',
  'src/applets/base/basename/basename.c',
  'src/applets/base/bc/args.c',
  'src/applets/base/bc/bc.c',
  'src/applets/base/bc/bc_lex.c',
  'src/applets/base/bc/bc_parse.c',
  'src/applets/base/bc/data.c',
  'src/applets/base/bc/file.c',
  'src/applets/base/bc/lang.c',
  'src/applets/base/bc/lex.c',
  'src/applets/base/bc/main.c',
  'src/applets/base/bc/num.c',
  'src/applets/base/bc/opt.c',
  'src/applets/base/bc/parse.c',
  'src/applets/base/bc/program.c',
  'src/applets/base/bc/rand.c',
  'src/applets/base/bc/read.c',
  'src/applets/base/bc/vector.c',
  'src/applets/base/bc/vm.c',
  'src/applets/base/cat/cat.c',
  'src/applets/base/cksum/cksum.c',
  'src/applets/base/comm/comm.c',
  'src/applets/base/cp/cp.c',
  'src/applets/base/cut/cut.c',
  'src/applets/base/date/date.c',
  'src/applets/base/df/df.c',
  'src/applets/base/dircolors/dircolors.c',
  'src/applets/base/dirname/dirname.c',
  'src/applets/base/du/du.c',
  'src/applets/base/echo/echo.c',
  'src/applets/base/env/env.c',
  'src/applets/base/expand/expand.c',
  'src/applets/base/expr/expr.c',
  'src/applets/base/find/find.c',
  'src/applets/base/find/find_frontend.c',
  'src/applets/base/find/find_eval.c',
  'src/applets/base/find/find_exec.c',
  'src/applets/base/find/find_expr.c',
  'src/applets/base/find/find_output.c',
  'src/applets/base/find/find_parse.c',
  'src/applets/base/find/find_parse_helpers.c',
  'src/applets/base/find/find_regex.c',
  'src/applets/base/find/find_run.c',
  'src/applets/base/fold/fold.c',
  'src/applets/base/fmt/fmt.c',
  'src/applets/base/head/head.c',
  'src/applets/base/hostid/hostid.c',
  'src/applets/base/hostname/hostname.c',
  'src/applets/base/id/id.c',
  'src/applets/base/join/join.c',
  'src/applets/base/link/link.c',
  'src/applets/base/ln/ln.c',
  'src/applets/base/logname/logname.c',
  'src/applets/base/ls/ls.c',
  'src/applets/base/md5sum/md5sum.c',
  'src/applets/base/mkdir/mkdir.c',
  'src/applets/base/mkfifo/mkfifo.c',
  'src/applets/base/mknod/mknod.c',
  'src/applets/base/mktemp/mktemp.c',
  'src/applets/base/mv/mv.c',
  'src/applets/base/nl/nl.c',
  'src/applets/base/nohup/nohup.c',
  'src/applets/base/nproc/nproc.c',
  'src/applets/base/numfmt/numfmt.c',
  'src/applets/base/od/od.c',
  'src/applets/base/paste/paste.c',
  'src/applets/base/pathchk/pathchk.c',
  'src/applets/base/printenv/printenv.c',
  'src/applets/base/printf/printf.c',
  'src/applets/base/pwd/pwd.c',
  'src/applets/base/readlink/readlink.c',
  'src/applets/base/realpath/realpath.c',
  'src/applets/base/rm/rm.c',
  'src/applets/base/rmdir/rmdir.c',
  'src/applets/base/seq/seq.c',
  'src/applets/base/sha1sum/sha1sum.c',
  'src/applets/base/sha256sum/sha256sum.c',
  'src/applets/base/shred/shred.c',
  'src/applets/base/shuf/shuf.c',
  'src/applets/base/sleep/sleep.c',
  'src/applets/base/sort/sort.c',
  'src/applets/base/split/split.c',
  'src/applets/base/stat/stat.c',
  'src/applets/base/sum/sum.c',
  'src/applets/base/sync/sync.c',
  'src/applets/base/tac/tac.c',
  'src/applets/base/tail/tail.c',
  'src/applets/base/tee/tee.c',
  'src/applets/base/test/test.c',
  'src/applets/base/touch/touch.c',
  'src/applets/base/tree/tree.c',
  'src/applets/base/tree/tree_collect.c',
  'src/applets/base/tree/tree_render.c',
  'src/applets/base/tr/tr.c',
  'src/applets/base/truncate/truncate.c',
  'src/applets/base/truefalse/truefalse.c',
  'src/applets/base/tty/tty.c',
  'src/applets/base/uname/uname.c',
  'src/applets/base/unexpand/unexpand.c',
  'src/applets/base/unlink/unlink.c',
  'src/applets/base/uniq/uniq.c',
  'src/applets/base/wc/wc.c',
  'src/applets/base/whoami/whoami.c',
  'src/applets/base/xargs/xargs.c',
  'src/applets/base/xargs/xargs_exec.c',
  'src/applets/base/xargs/xargs_input.c',
  'src/applets/base/xargs/xargs_parse.c',
  'src/applets/base/yes/yes.c',
  'src/applets/base/which/which.c',
]

bx_shell_applet_sources = [
  'src/applets/shell/ash/ash.c',
  'src/applets/shell/ash/applet_command.c',
  'src/applets/shell/ash/ast.c',
  'src/applets/shell/ash/command_resolution.c',
  'src/applets/shell/ash/redirection.c',
  'src/applets/shell/ash/control.c',
  'src/applets/shell/ash/diagnostic.c',
  'src/applets/shell/ash/expansion.c',
  'src/applets/shell/ash/external_command.c',
  'src/applets/shell/ash/functions.c',
  'src/applets/shell/ash/input.c',
  'src/applets/shell/ash/lexer.c',
  'src/applets/shell/ash/pattern.c',
  'src/applets/shell/ash/pattern_expand.c',
  'src/applets/shell/ash/parser.c',
  'src/applets/shell/ash/process.c',
  'src/applets/shell/ash/scope.c',
  'src/applets/shell/ash/shell_context.c',
  'src/applets/shell/ash/shell_policy.c',
  'src/applets/shell/ash/syntax.c',
  'src/applets/shell/ash/variable_value.c',
  'src/applets/shell/ash/variables.c',
]

bx_text_applet_sources = [
  'src/applets/text/dd/dd.c',
  'src/applets/text/ed/buffer.c',
  'src/applets/text/ed/carg_parser.c',
  'src/applets/text/ed/global.c',
  'src/applets/text/ed/io.c',
  'src/applets/text/ed/main.c',
  'src/applets/text/ed/main_loop.c',
  'src/applets/text/ed/regex.c',
  'src/applets/text/ed/signal.c',
  'src/applets/text/man/man.c',
  'src/applets/text/mandoc/mandoc.c',
  'src/applets/text/fd/fd.c',
  'src/applets/text/fd/fd_exec.c',
  'src/applets/text/fd/fd_match.c',
  'src/applets/text/fd/fd_exec_render.c',
  'src/applets/text/fd/fd_output.c',
  'src/applets/text/fd/fd_parse.c',
  'src/applets/text/grep/grep.c',
  'src/applets/text/rg/rg.c',
]

bx_system_applet_sources = [
  'src/applets/system/chgrp/chgrp.c',
  'src/applets/system/chmod/chmod.c',
  'src/applets/system/chown/chown.c',
  'src/applets/system/chroot/chroot.c',
  'src/applets/system/dmesg/dmesg.c',
  'src/applets/system/fuser/fuser.c',
  'src/applets/system/getty/getty.c',
  'src/applets/system/init/init.c',
  'src/applets/system/init/init_command.c',
  'src/applets/system/init/init_config.c',
  'src/applets/system/init/init_runtime.c',
  'src/applets/system/syslogd/syslogd.c',
  'src/applets/system/install/install.c',
  'src/applets/system/kill/kill.c',
  'src/applets/system/killall/killall.c',
  'src/applets/system/mount/mount.c',
  'src/applets/system/nice/nice.c',
  'src/applets/system/peekfd/peekfd.c',
  'src/applets/system/prtstat/prtstat.c',
  'src/applets/system/ps/ps.c',
  'src/applets/system/pslog/pslog.c',
  'src/applets/system/psmisc/procfs.c',
  'src/applets/system/psmisc/signals.c',
  'src/applets/system/psmisc/psmisc_wrapper.c',
  'src/applets/system/pstree/pstree.c',
  'src/applets/system/reboot/reboot.c',
  'src/applets/system/screen/screen_applet.c',
  'src/applets/system/setsid/setsid.c',
  'src/applets/system/stty/stty.c',
  'src/applets/system/stdbuf/stdbuf.c',
  'src/applets/system/switch_root/switch_root.c',
  'src/applets/system/timeout/timeout.c',
  'src/applets/system/umount/umount.c',
]

bx_network_applet_sources = [
  'src/applets/network/dhcp/dhcp.c',
  'src/applets/network/ping/ping.c',
  'src/applets/network/wget/wget.c',
]

bx_search_sources = [
  'src/fswalk/walk_cycle.c',
  'src/fswalk/walk_dirent.c',
  'src/fswalk/walk_entry.c',
  'src/fswalk/walk_error.c',
  'src/fswalk/walk_path.c',
  'src/fswalk/walk.c',
  'src/search/binary_scan.c',
  'src/search/filter.c',
  'src/search/context_roll.c',
  'src/search/metadata.c',
  'src/search/dev_counters.c',
  'src/search/literal.c',
  'src/search/literal_plan.c',
  'src/search/literal_arm64_probe.c',
  'src/search/literal_x86_probe.c',
  'src/search/ignore.c',
  'src/search/ignore_program.c',
  'src/search/options.c',
  'src/search/pattern_vec.c',
  'src/search/pcre2_matcher.c',
  'src/search/record_stream.c',
  'src/search/rg_generate.c',
  'src/search/rg_parallel.c',
  'src/search/rg_publish.c',
  'src/search/rg_output.c',
  'src/search/rg_sched.c',
  'src/search/rg_decode.c',
  'src/search/rg_decode_reader.c',
  'src/search/rg_text.c',
  'src/search/rg_transform.c',
  'src/search/runtime_snapshot.c',
  'src/search/scanner.c',
  'src/search/search_file.c',
  'src/search/search_input.c',
  'src/search/search_path.c',
  'src/search/search_plan.c',
  'src/search/search_raw_presence.c',
  'src/search/search_buffered.c',
  'src/search/search_matcher.c',
  'src/search/search_multiline.c',
  'src/search/search_output.c',
  'src/search/search_scanner.c',
  'src/search/search_streaming.c',
  'src/search/search_run.c',
  'src/search/sort.c',
  'src/search/search.c',
  'src/search/traverse.c',
]

screen_native_sources = files(
  'src/applets/system/screen/acls.c',
  'src/applets/system/screen/ansi.c',
  'src/applets/system/screen/attacher.c',
  'src/applets/system/screen/backtick.c',
  'src/applets/system/screen/canvas.c',
  'src/applets/system/screen/comm.c',
  'src/applets/system/screen/display.c',
  'src/applets/system/screen/encoding.c',
  'src/applets/system/screen/fileio.c',
  'src/applets/system/screen/help.c',
  'src/applets/system/screen/input.c',
  'src/applets/system/screen/kmapdef.c',
  'src/applets/system/screen/layer.c',
  'src/applets/system/screen/layout.c',
  'src/applets/system/screen/list_display.c',
  'src/applets/system/screen/list_generic.c',
  'src/applets/system/screen/list_license.c',
  'src/applets/system/screen/list_window.c',
  'src/applets/system/screen/logfile.c',
  'src/applets/system/screen/mark.c',
  'src/applets/system/screen/misc.c',
  'src/applets/system/screen/process.c',
  'src/applets/system/screen/process_lookup.c',
  'src/applets/system/screen/process_parse.c',
  'src/applets/system/screen/process_window_nav.c',
  'src/applets/system/screen/process_action_store.c',
  'src/applets/system/screen/process_prompts.c',
  'src/applets/system/screen/process_resize_prompt.c',
  'src/applets/system/screen/pty.c',
  'src/applets/system/screen/resize.c',
  'src/applets/system/screen/sched.c',
  'src/applets/system/screen/screen.c',
  'src/applets/system/screen/search.c',
  'src/applets/system/screen/socket.c',
  'src/applets/system/screen/term.c',
  'src/applets/system/screen/termcap.c',
  'src/applets/system/screen/tty.c',
  'src/applets/system/screen/viewport.c',
  'src/applets/system/screen/window.c',
  'src/applets/system/screen/winmsg.c',
  'src/applets/system/screen/winmsgbuf.c',
  'src/applets/system/screen/winmsgcond.c',
)

sources = bx_core_sources
sources += bx_lib_sources
sources += bx_crypto_sources
sources += bx_archive_applet_sources
sources += bx_base_applet_sources
sources += bx_shell_applet_sources
sources += bx_text_applet_sources
sources += bx_system_applet_sources
sources += bx_network_applet_sources
sources += bx_search_sources

nc_sources = files(
  'src/applets/network/nc/nc.c',
  'src/applets/network/nc/atomicio.c',
  'src/applets/network/nc/bpf.c',
  'src/applets/network/nc/hexdump.c',
  'src/applets/network/nc/io.c',
  'src/applets/network/nc/log.c',
  'src/applets/network/nc/mptcp_pm.c',
  'src/applets/network/nc/network.c',
  'src/applets/network/nc/pcap.c',
  'src/applets/network/nc/proxy_proto.c',
  'src/applets/network/nc/quic.c',
  'src/applets/network/nc/socks.c',
  'src/applets/network/nc/tls.c',
  'src/applets/network/nc/tls_disabled.c',
  'src/applets/network/nc/utils.c',
)

nc_c_args = cc.get_supported_arguments([
  '-DBX_NC_TLS_DISABLED=1',
  '-D_GNU_SOURCE',
  '-D_DEFAULT_SOURCE',
  '-D__FAVOR_BSD',
  '-Wno-error',
  '-Wno-pedantic',
])

nc_lib = static_library('bx_nc',
  nc_sources,
  include_directories : include_directories('src', 'src/dispatch', 'src/applets/network/nc'),
  dependencies : [threads_dep],
  c_args : nc_c_args,
)

socat_sources = files(
  'src/applets/network/socat/socat.c',
  'src/applets/network/socat/upstream/socat.c',
  'src/applets/network/socat/upstream/xioinitialize.c',
  'src/applets/network/socat/upstream/xiohelp.c',
  'src/applets/network/socat/upstream/xioparam.c',
  'src/applets/network/socat/upstream/xiodiag.c',
  'src/applets/network/socat/upstream/xioopen.c',
  'src/applets/network/socat/upstream/xioopts.c',
  'src/applets/network/socat/upstream/xiosignal.c',
  'src/applets/network/socat/upstream/xiosigchld.c',
  'src/applets/network/socat/upstream/xioread.c',
  'src/applets/network/socat/upstream/xiowrite.c',
  'src/applets/network/socat/upstream/xiolayer.c',
  'src/applets/network/socat/upstream/xioshutdown.c',
  'src/applets/network/socat/upstream/xioclose.c',
  'src/applets/network/socat/upstream/xioexit.c',
  'src/applets/network/socat/upstream/xio-process.c',
  'src/applets/network/socat/upstream/xio-fd.c',
  'src/applets/network/socat/upstream/xio-fdnum.c',
  'src/applets/network/socat/upstream/xio-stdio.c',
  'src/applets/network/socat/upstream/xio-pipe.c',
  'src/applets/network/socat/upstream/xio-socketpair.c',
  'src/applets/network/socat/upstream/xio-gopen.c',
  'src/applets/network/socat/upstream/xio-creat.c',
  'src/applets/network/socat/upstream/xio-file.c',
  'src/applets/network/socat/upstream/xio-named.c',
  'src/applets/network/socat/upstream/xio-socket.c',
  'src/applets/network/socat/upstream/xio-interface.c',
  'src/applets/network/socat/upstream/xio-listen.c',
  'src/applets/network/socat/upstream/xio-unix.c',
  'src/applets/network/socat/upstream/xio-vsock.c',
  'src/applets/network/socat/upstream/xio-ip.c',
  'src/applets/network/socat/upstream/xio-ip4.c',
  'src/applets/network/socat/upstream/xio-ip6.c',
  'src/applets/network/socat/upstream/xio-ipapp.c',
  'src/applets/network/socat/upstream/xio-tcp.c',
  'src/applets/network/socat/upstream/xio-sctp.c',
  'src/applets/network/socat/upstream/xio-dccp.c',
  'src/applets/network/socat/upstream/xio-rawip.c',
  'src/applets/network/socat/upstream/xio-posixmq.c',
  'src/applets/network/socat/upstream/xio-socks.c',
  'src/applets/network/socat/upstream/xio-socks5.c',
  'src/applets/network/socat/upstream/xio-proxy.c',
  'src/applets/network/socat/upstream/xio-udp.c',
  'src/applets/network/socat/upstream/xio-udplite.c',
  'src/applets/network/socat/upstream/xio-progcall.c',
  'src/applets/network/socat/upstream/xio-exec.c',
  'src/applets/network/socat/upstream/xio-system.c',
  'src/applets/network/socat/upstream/xio-shell.c',
  'src/applets/network/socat/upstream/xio-termios.c',
  'src/applets/network/socat/upstream/xio-readline.c',
  'src/applets/network/socat/upstream/xio-pty.c',
  'src/applets/network/socat/upstream/xio-openssl.c',
  'src/applets/network/socat/upstream/xio-streams.c',
  'src/applets/network/socat/upstream/xio-namespaces.c',
  'src/applets/network/socat/upstream/xio-ascii.c',
  'src/applets/network/socat/upstream/xiolockfile.c',
  'src/applets/network/socat/upstream/xio-tcpwrap.c',
  'src/applets/network/socat/upstream/xio-fs.c',
  'src/applets/network/socat/upstream/xio-tun.c',
  'src/applets/network/socat/upstream/xio-stall.c',
  'src/applets/network/socat/upstream/xio-text.c',
  'src/applets/network/socat/upstream/xio-netlink.c',
  'src/applets/network/socat/upstream/error.c',
  'src/applets/network/socat/upstream/dalan.c',
  'src/applets/network/socat/upstream/procan.c',
  'src/applets/network/socat/upstream/procan-cdefs.c',
  'src/applets/network/socat/upstream/hostan.c',
  'src/applets/network/socat/upstream/fdname.c',
  'src/applets/network/socat/upstream/sysutils.c',
  'src/applets/network/socat/upstream/utils.c',
  'src/applets/network/socat/upstream/nestlex.c',
  'src/applets/network/socat/upstream/vsnprintf_r.c',
  'src/applets/network/socat/upstream/snprinterr.c',
  'src/applets/network/socat/upstream/sycls.c',
)

socat_c_args = cc.get_supported_arguments([
  '-D_DEFAULT_SOURCE',
  '-D_GNU_SOURCE',
  '-Dmain=bx_socat_main_impl',
  '-Dmsg=bx_socat_msg',
  '-Wno-error',
  '-Wno-pedantic',
  '-Wno-shadow',
  '-Wno-sign-compare',
  '-Wno-unused-but-set-variable',
  '-Wno-unused-function',
  '-Wno-unused-parameter',
  '-Wno-unused-variable',
])

socat_lib = static_library('bx_socat',
  socat_sources,
  include_directories : include_directories(
    'src',
    'src/dispatch',
    'src/applets/network/socat',
    'src/applets/network/socat/upstream',
  ),
  c_args : socat_c_args,
)

traceroute_sources = files(
  'src/applets/network/traceroute/traceroute.c',
  'src/applets/network/traceroute/as_lookups.c',
  'src/applets/network/traceroute/csum.c',
  'src/applets/network/traceroute/export.c',
  'src/applets/network/traceroute/extension.c',
  'src/applets/network/traceroute/mod-dccp.c',
  'src/applets/network/traceroute/mod-icmp.c',
  'src/applets/network/traceroute/mod-raw.c',
  'src/applets/network/traceroute/mod-tcp.c',
  'src/applets/network/traceroute/mod-tcpconn.c',
  'src/applets/network/traceroute/mod-udp.c',
  'src/applets/network/traceroute/module.c',
  'src/applets/network/traceroute/poll.c',
  'src/applets/network/traceroute/random.c',
  'src/applets/network/traceroute/time.c',
  'src/applets/network/traceroute/clif.c',
  'src/applets/network/traceroute/bpf_disabled.c',
  'src/applets/network/traceroute/xdp_disabled.c',
)

traceroute_c_args = cc.get_supported_arguments([
  '-D_DEFAULT_SOURCE',
  '-Dmain=bx_traceroute_main',
  '-Wno-error',
  '-Wno-pedantic',
])

traceroute_lib = static_library('bx_traceroute',
  traceroute_sources,
  include_directories : include_directories('src', 'src/applets/network/traceroute'),
  c_args : traceroute_c_args,
)

screen_termcap_dep = declare_dependency()
screen_termcap_found = false
foreach dep_name : ['tinfotw', 'ncursestw', 'tinfo', 'ncurses', 'tinfow', 'ncursesw']
  dep = dependency(dep_name,
    method : 'pkg-config',
    required : false,
    static : use_static_binary,
  )
  if dep.found() and cc.has_function('tgetent', dependencies : dep)
    screen_termcap_dep = dep
    screen_termcap_found = true
    break
  endif
endforeach
if not screen_termcap_found
  screen_termcap_found = cc.has_function('tgetent')
endif
if not screen_termcap_found
  foreach lib : ['tinfotw', 'ncursestw', 'curses', 'termcap', 'termlib', 'ncursesw', 'tinfow', 'ncurses', 'tinfo']
    dep = cc.find_library(lib, required : false)
    if dep.found() and cc.has_function('tgetent', dependencies : dep)
      screen_termcap_dep = dep
      screen_termcap_found = true
      break
    endif
  endforeach
endif
if not screen_termcap_found
  error('screen requires tgetent()')
endif

screen_openpty_dep = declare_dependency()
screen_openpty_found = cc.has_function('openpty')
if not screen_openpty_found
  foreach lib : ['util', 'openpty', 'bsd']
    dep = cc.find_library(lib, required : false)
    if dep.found() and cc.has_function('openpty', dependencies : dep)
      screen_openpty_dep = dep
      screen_openpty_found = true
      break
    endif
  endforeach
endif
if not screen_openpty_found
  error('screen requires openpty()')
endif

screen_deps = [screen_termcap_dep, screen_openpty_dep]
screen_crypt_dep = declare_dependency()
if not cc.has_function('crypt')
  if cc.has_function('crypt', args : ['-lcrypt'])
    screen_crypt_dep = declare_dependency(link_args : ['-lcrypt'])
    screen_deps += [screen_crypt_dep]
  endif
endif

screen_c_args = cc.get_supported_arguments([
  '-Wno-error',
])
screen_c_args += [
  '-D_GNU_SOURCE',
  '-D_DEFAULT_SOURCE',
  '-Ddflag=screen_dflag',
  '-Diflag=screen_iflag',
  '-Dmain=bx_screen_main_impl',
  '-Drflag=screen_rflag',
  '-DSCREENENCODINGS="' + screen_utf8_install_path + '"',
  '-DSYSTEM_SCREENRC="' + screen_system_rc_path + '"',
  '-Dxflag=screen_xflag',
  '-Dxrealloc=screen_xrealloc',
]

if use_static_binary
  screen_termcap_static_ok = cc.links(
    'int tgetent(char *, const char *); int main(void) { return tgetent(0, 0); }',
    dependencies : [screen_termcap_dep],
    args : bx_static_link_args,
    name : 'screen termcap static linkability probe',
  )
  if not screen_termcap_static_ok
    error('screen requires a static-capable termcap library when static_binary is enabled')
  endif
endif

screen_lib = static_library('bx_screen',
  screen_native_sources,
  include_directories : include_directories('src'),
  dependencies : screen_deps,
  c_args : screen_c_args,
)

edit_wrapper_sources = files(
  'src/applets/text/edit/edit_main.c',
  'src/applets/text/edit/bx_vim_runtime.c',
  'src/applets/text/edit/bx_vim_startup.c',
)

edit_wrapper_c_args = cc.get_supported_arguments([
  '-Wno-error',
  '-Wno-pedantic',
])
edit_wrapper_c_args += [
  '-DBX_EDIT_VIM_RUNTIME_INSTALL_DIR="' + edit_vim_runtime_install_path + '"',
  '-DBX_EDIT_VIM_RUNTIME_SOURCE_DIR="' + edit_vim_runtime_source_path + '"',
]

edit_wrapper_lib = static_library('bx_edit_wrapper',
  edit_wrapper_sources,
  include_directories : include_directories('src', 'src/dispatch'),
  c_args : edit_wrapper_c_args,
)

edit_vim_sources = files(
  'src/applets/text/edit/vim/src/alloc.c',
  'src/applets/text/edit/vim/src/arabic.c',
  'src/applets/text/edit/vim/src/arglist.c',
  'src/applets/text/edit/vim/src/autocmd.c',
  'src/applets/text/edit/vim/src/beval.c',
  'src/applets/text/edit/vim/src/blob.c',
  'src/applets/text/edit/vim/src/blowfish.c',
  'src/applets/text/edit/vim/src/buffer.c',
  'src/applets/text/edit/vim/src/bufwrite.c',
  'src/applets/text/edit/vim/src/change.c',
  'src/applets/text/edit/vim/src/charset.c',
  'src/applets/text/edit/vim/src/cindent.c',
  'src/applets/text/edit/vim/src/clientserver.c',
  'src/applets/text/edit/vim/src/clipboard.c',
  'src/applets/text/edit/vim/src/cmdexpand.c',
  'src/applets/text/edit/vim/src/cmdhist.c',
  'src/applets/text/edit/vim/src/crypt.c',
  'src/applets/text/edit/vim/src/crypt_zip.c',
  'src/applets/text/edit/vim/src/debugger.c',
  'src/applets/text/edit/vim/src/dict.c',
  'src/applets/text/edit/vim/src/diff.c',
  'src/applets/text/edit/vim/src/digraph.c',
  'src/applets/text/edit/vim/src/drawline.c',
  'src/applets/text/edit/vim/src/drawscreen.c',
  'src/applets/text/edit/vim/src/edit.c',
  'src/applets/text/edit/vim/src/eval.c',
  'src/applets/text/edit/vim/src/evalbuffer.c',
  'src/applets/text/edit/vim/src/evalfunc.c',
  'src/applets/text/edit/vim/src/evalvars.c',
  'src/applets/text/edit/vim/src/evalwindow.c',
  'src/applets/text/edit/vim/src/ex_cmds.c',
  'src/applets/text/edit/vim/src/ex_cmds2.c',
  'src/applets/text/edit/vim/src/ex_docmd.c',
  'src/applets/text/edit/vim/src/ex_eval.c',
  'src/applets/text/edit/vim/src/ex_getln.c',
  'src/applets/text/edit/vim/src/fileio.c',
  'src/applets/text/edit/vim/src/filepath.c',
  'src/applets/text/edit/vim/src/findfile.c',
  'src/applets/text/edit/vim/src/float.c',
  'src/applets/text/edit/vim/src/fold.c',
  'src/applets/text/edit/vim/src/fuzzy.c',
  'src/applets/text/edit/vim/src/gc.c',
  'src/applets/text/edit/vim/src/getchar.c',
  'src/applets/text/edit/vim/src/gui_xim.c',
  'src/applets/text/edit/vim/src/hardcopy.c',
  'src/applets/text/edit/vim/src/hashtab.c',
  'src/applets/text/edit/vim/src/help.c',
  'src/applets/text/edit/vim/src/highlight.c',
  'src/applets/text/edit/vim/src/if_cscope.c',
  'src/applets/text/edit/vim/src/if_xcmdsrv.c',
  'src/applets/text/edit/vim/src/indent.c',
  'src/applets/text/edit/vim/src/insexpand.c',
  'src/applets/text/edit/vim/src/json.c',
  'src/applets/text/edit/vim/src/linematch.c',
  'src/applets/text/edit/vim/src/list.c',
  'src/applets/text/edit/vim/src/locale.c',
  'src/applets/text/edit/vim/src/logfile.c',
  'src/applets/text/edit/vim/src/main.c',
  'src/applets/text/edit/vim/src/map.c',
  'src/applets/text/edit/vim/src/mark.c',
  'src/applets/text/edit/vim/src/match.c',
  'src/applets/text/edit/vim/src/mbyte.c',
  'src/applets/text/edit/vim/src/memfile.c',
  'src/applets/text/edit/vim/src/memline.c',
  'src/applets/text/edit/vim/src/menu.c',
  'src/applets/text/edit/vim/src/message.c',
  'src/applets/text/edit/vim/src/misc1.c',
  'src/applets/text/edit/vim/src/misc2.c',
  'src/applets/text/edit/vim/src/mouse.c',
  'src/applets/text/edit/vim/src/move.c',
  'src/applets/text/edit/vim/src/normal.c',
  'src/applets/text/edit/vim/src/ops.c',
  'src/applets/text/edit/vim/src/option.c',
  'src/applets/text/edit/vim/src/optionstr.c',
  'src/applets/text/edit/vim/src/os_unix.c',
  'src/applets/text/edit/vim/src/auto/pathdef.c',
  'src/applets/text/edit/vim/src/popupmenu.c',
  'src/applets/text/edit/vim/src/popupwin.c',
  'src/applets/text/edit/vim/src/profiler.c',
  'src/applets/text/edit/vim/src/pty.c',
  'src/applets/text/edit/vim/src/quickfix.c',
  'src/applets/text/edit/vim/src/regexp.c',
  'src/applets/text/edit/vim/src/register.c',
  'src/applets/text/edit/vim/src/screen.c',
  'src/applets/text/edit/vim/src/scriptfile.c',
  'src/applets/text/edit/vim/src/search.c',
  'src/applets/text/edit/vim/src/session.c',
  'src/applets/text/edit/vim/src/sha256.c',
  'src/applets/text/edit/vim/src/sign.c',
  'src/applets/text/edit/vim/src/sound.c',
  'src/applets/text/edit/vim/src/spell.c',
  'src/applets/text/edit/vim/src/spellfile.c',
  'src/applets/text/edit/vim/src/spellsuggest.c',
  'src/applets/text/edit/vim/src/strings.c',
  'src/applets/text/edit/vim/src/syntax.c',
  'src/applets/text/edit/vim/src/tabpanel.c',
  'src/applets/text/edit/vim/src/tag.c',
  'src/applets/text/edit/vim/src/term.c',
  'src/applets/text/edit/vim/src/terminal.c',
  'src/applets/text/edit/vim/src/testing.c',
  'src/applets/text/edit/vim/src/textformat.c',
  'src/applets/text/edit/vim/src/textobject.c',
  'src/applets/text/edit/vim/src/textprop.c',
  'src/applets/text/edit/vim/src/time.c',
  'src/applets/text/edit/vim/src/tuple.c',
  'src/applets/text/edit/vim/src/typval.c',
  'src/applets/text/edit/vim/src/ui.c',
  'src/applets/text/edit/vim/src/undo.c',
  'src/applets/text/edit/vim/src/usercmd.c',
  'src/applets/text/edit/vim/src/userfunc.c',
  'src/applets/text/edit/vim/src/version.c',
  'src/applets/text/edit/vim/src/vim9class.c',
  'src/applets/text/edit/vim/src/vim9cmds.c',
  'src/applets/text/edit/vim/src/vim9compile.c',
  'src/applets/text/edit/vim/src/vim9execute.c',
  'src/applets/text/edit/vim/src/vim9expr.c',
  'src/applets/text/edit/vim/src/vim9generics.c',
  'src/applets/text/edit/vim/src/vim9instr.c',
  'src/applets/text/edit/vim/src/vim9script.c',
  'src/applets/text/edit/vim/src/vim9type.c',
  'src/applets/text/edit/vim/src/viminfo.c',
  'src/applets/text/edit/vim/src/window.c',
  'src/applets/text/edit/vim/src/xdiff/xdiffi.c',
  'src/applets/text/edit/vim/src/xdiff/xemit.c',
  'src/applets/text/edit/vim/src/xdiff/xhistogram.c',
  'src/applets/text/edit/vim/src/xdiff/xpatience.c',
  'src/applets/text/edit/vim/src/xdiff/xprepare.c',
  'src/applets/text/edit/vim/src/xdiff/xutils.c',
)

edit_vim_deps = [screen_termcap_dep]
edit_vim_dl_dep = cc.find_library('dl', required : false)
if edit_vim_dl_dep.found()
  edit_vim_deps += [edit_vim_dl_dep]
endif

edit_vim_c_args = cc.get_supported_arguments([
  '-Wno-error',
  '-Wno-pedantic',
  '-Wno-cast-function-type',
  '-Wno-declaration-after-statement',
  '-Wno-sign-compare',
  '-Wno-unused-parameter',
  '-Wno-unused-function',
])
edit_vim_c_args += ['-DHAVE_CONFIG_H']

edit_vim_lib = static_library('bx_edit_vim',
  edit_vim_sources,
  include_directories : include_directories('src', 'src/applets/text/edit/vim/src', 'src/applets/text/edit/vim/src/proto'),
  dependencies : edit_vim_deps,
  c_args : edit_vim_c_args,
)

xxd_c_args = cc.get_supported_arguments([
  '-Wno-error',
])
xxd_c_args += [
  '-DUNIX',
]

xxd_lib = static_library('bx_xxd',
  'src/applets/text/xxd/xxd.c',
  include_directories : include_directories('src'),
  c_args : xxd_c_args,
)

jq_applet_sources = files(
  'src/applets/text/jq/jq_applet.c',
  'src/applets/text/jq/jq_cli.c',
  'src/lib/jq/pcre2_backend.c',
)

jq_vendor_sources = files(
  'src/vendor/jq/src/builtin.c',
  'src/vendor/jq/src/bytecode.c',
  'src/vendor/jq/src/compile.c',
  'src/vendor/jq/src/execute.c',
  'src/vendor/jq/src/jq_test.c',
  'src/vendor/jq/src/jv.c',
  'src/vendor/jq/src/jv_alloc.c',
  'src/vendor/jq/src/jv_aux.c',
  'src/vendor/jq/src/jv_dtoa.c',
  'src/vendor/jq/src/jv_dtoa_tsd.c',
  'src/vendor/jq/src/jv_file.c',
  'src/vendor/jq/src/jv_parse.c',
  'src/vendor/jq/src/jv_print.c',
  'src/vendor/jq/src/jv_unicode.c',
  'src/vendor/jq/src/linker.c',
  'src/vendor/jq/src/locfile.c',
  'src/vendor/jq/src/parser.c',
  'src/vendor/jq/src/lexer.c',
  'src/vendor/jq/src/util.c',
  'src/vendor/jq/vendor/decNumber/decContext.c',
  'src/vendor/jq/vendor/decNumber/decNumber.c',
)

jq_include_dirs = include_directories(
  'src',
  'src/dispatch',
  'src/applets/text/jq',
  'src/vendor/jq',
  'src/vendor/jq/src',
  'src/vendor/jq/vendor',
)

jq_c_args = cc.get_supported_arguments([
  '-Wno-error',
  '-Wno-pedantic',
  '-Wno-cast-qual',
  '-Wno-declaration-after-statement',
  '-Wno-sign-compare',
  '-Wno-unused-function',
  '-Wno-unused-parameter',
])
jq_c_args += [
  '-include',
  join_paths(meson.project_source_root(), 'src', 'vendor', 'jq', 'bx_jq_config.h'),
]

jq_vendor_lib = static_library('bx_jq_vendor',
  jq_vendor_sources,
  include_directories : jq_include_dirs,
  dependencies : [threads_dep, pcre2_dep],
  c_args : jq_c_args,
)

jq_lib = static_library('bx_jq_integration',
  jq_applet_sources,
  include_directories : jq_include_dirs,
  dependencies : [threads_dep, pcre2_dep],
  c_args : jq_c_args,
)

manual_utf8_locale = ''
manual_utf8_locale_candidates = [
  'C.UTF-8',
  'C.utf8',
  'en_US.UTF-8',
  'en_US.utf8',
]

locale_prog = find_program('locale', required : false)
if locale_prog.found()
  locale_list = run_command(locale_prog, '-a', check : false)
  if locale_list.returncode() == 0
    foreach loc : locale_list.stdout().strip().split('\n')
      low = loc.to_lower()
      if low.contains('.utf') and low.contains('8')
        if not manual_utf8_locale_candidates.contains(loc)
          manual_utf8_locale_candidates += [loc]
        endif
      endif
    endforeach
  endif
endif

manual_utf8_locale_check = '''
  #include <locale.h>
  int main(void) {
    return setlocale(LC_CTYPE, TEST_LOCALE) == NULL;
  }
'''

if can_run
  foreach loc : manual_utf8_locale_candidates
    locale_check = cc.run(
      manual_utf8_locale_check,
      args : ['-DTEST_LOCALE="@0@"'.format(loc)],
      name : 'manual utf8 locale ' + loc,
    )
    if locale_check.compiled() and locale_check.returncode() == 0
      manual_utf8_locale = loc
      break
    endif
  endforeach
endif

manual_have_wchar = false
manual_strl_prefix = '''
  #define _DEFAULT_SOURCE
  #define _GNU_SOURCE
  #include <string.h>
'''
manual_have_strlcat = cc.has_function(
  'strlcat',
  prefix : manual_strl_prefix,
)
manual_have_strlcpy = cc.has_function(
  'strlcpy',
  prefix : manual_strl_prefix,
)
if manual_utf8_locale != ''
  manual_wchar_check = '''
    #define _DEFAULT_SOURCE
    #define _GNU_SOURCE
    #define _XOPEN_SOURCE 700
    #include <locale.h>
    #include <stdlib.h>
    #include <string.h>
    #include <wchar.h>
    int main(void) {
      char buf[MB_CUR_MAX];
      mbstate_t ps;
      if (setlocale(LC_CTYPE, TEST_LOCALE) == NULL)
        return 1;
      if (sizeof(wchar_t) < 4)
        return 1;
      if (wcwidth(L' ') != 1 || wcwidth(0x2013) != 1)
        return 1;
      memset(&ps, 0, sizeof(ps));
      return wcrtomb(buf, 0x2013, &ps) == (size_t)-1;
    }
  '''
  if can_run
    wchar_check = cc.run(
      manual_wchar_check,
      args : ['-DTEST_LOCALE="@0@"'.format(manual_utf8_locale)],
      name : 'manual utf8 byte output',
    )
    manual_have_wchar = wchar_check.compiled() and wchar_check.returncode() == 0
  else
    manual_have_wchar = cc.links(
      manual_wchar_check,
      args : ['-DTEST_LOCALE="@0@"'.format(manual_utf8_locale)],
      name : 'manual utf8 byte output',
    )
  endif
endif

manual_sources = files(
  'src/lib/manual/apps/main.c',
  'src/lib/manual/core/manpath.c',
  'src/lib/manual/core/man_markdown.c',
  'src/lib/manual/core/markdown_man.c',
  'src/lib/manual/core/mdoc_man.c',
  'src/lib/manual/core/mdoc_markdown.c',
  'src/lib/manual/core/out.c',
  'src/lib/manual/core/eqn_term.c',
  'src/lib/manual/core/man_term.c',
  'src/lib/manual/core/mdoc_term.c',
  'src/lib/manual/core/roff_term.c',
  'src/lib/manual/core/term.c',
  'src/lib/manual/core/term_ascii.c',
  'src/lib/manual/core/term_tab.c',
  'src/lib/manual/core/term_tag.c',
  'src/lib/manual/core/tbl_term.c',
  'src/lib/manual/core/man.c',
  'src/lib/manual/core/man_macro.c',
  'src/lib/manual/core/man_validate.c',
  'src/lib/manual/core/att.c',
  'src/lib/manual/core/lib.c',
  'src/lib/manual/core/mdoc.c',
  'src/lib/manual/core/mdoc_argv.c',
  'src/lib/manual/core/mdoc_macro.c',
  'src/lib/manual/core/mdoc_state.c',
  'src/lib/manual/core/mdoc_validate.c',
  'src/lib/manual/core/st.c',
  'src/lib/manual/core/eqn.c',
  'src/lib/manual/core/roff.c',
  'src/lib/manual/core/roff_escape.c',
  'src/lib/manual/core/roff_validate.c',
  'src/lib/manual/core/tbl.c',
  'src/lib/manual/core/tbl_data.c',
  'src/lib/manual/core/tbl_layout.c',
  'src/lib/manual/core/tbl_opts.c',
  'src/lib/manual/core/arch.c',
  'src/lib/manual/core/chars.c',
  'src/lib/manual/core/mandoc.c',
  'src/lib/manual/core/mandoc_aux.c',
  'src/lib/manual/core/mandoc_msg.c',
  'src/lib/manual/core/mandoc_ohash.c',
  'src/lib/manual/core/mandoc_xr.c',
  'src/lib/manual/core/msec.c',
  'src/lib/manual/core/preconv.c',
  'src/lib/manual/core/read.c',
  'src/lib/manual/core/tag.c',
  'src/lib/manual/compat/compat_progname.c',
  'src/lib/manual/compat/compat_reallocarray.c',
  'src/lib/manual/compat/compat_recallocarray.c',
  'src/lib/manual/compat/compat_strtonum.c',
  'src/lib/manual/compat/compat_ohash.c',
)

if not manual_have_strlcat
  manual_sources += files('src/lib/manual/compat/compat_strlcat.c')
endif
if not manual_have_strlcpy
  manual_sources += files('src/lib/manual/compat/compat_strlcpy.c')
endif

manual_include_dirs = include_directories(
  'src',
  'src/lib/manual',
  'src/lib/manual/include',
  'src/lib/manual/data',
)

manual_deps = [zlib_dep, libbz2_dep, liblzma_dep, libzstd_dep]

manual_c_args = cc.get_supported_arguments([
  '-Wno-error',
  '-Wno-cast-qual',
  '-Wno-declaration-after-statement',
  '-Wno-missing-prototypes',
  '-Wno-sign-compare',
  '-Wno-unused-parameter',
  '-Wno-unused-variable',
])
manual_c_args += [
  '-D_GNU_SOURCE',
  '-DBX_MANUAL_HAVE_WCHAR=' + (manual_have_wchar ? '1' : '0'),
  '-DBX_MANUAL_UTF8_LOCALE="' + manual_utf8_locale + '"',
  '-Dmain=bx_mandoc_main_impl',
]

manual_lib = static_library('bx_manual',
  manual_sources,
  include_directories : manual_include_dirs,
  dependencies : manual_deps,
  c_args : manual_c_args,
)

bc_host_c_args = cc.get_supported_arguments([
  '-Wno-error',
])

bc_strgen = executable('bx_bc_strgen',
  'src/applets/base/bc/strgen.c',
  include_directories : include_directories('src/applets/base/bc/include'),
  native : true,
  c_args : bc_host_c_args,
)

bc_help_src = custom_target(
  'bx_bc_help_c',
  input : 'src/applets/base/bc/assets/bc_help.txt',
  output : 'bx_bc_help.c',
  command : [bc_strgen, '@INPUT@', '@OUTPUT@', '0', 'bc_help', '', 'BC_ENABLED', '0'],
)

bc_lib_src = custom_target(
  'bx_bc_lib_c',
  input : 'src/applets/base/bc/assets/lib.bc',
  output : 'bx_bc_lib.c',
  command : [bc_strgen, '@INPUT@', '@OUTPUT@', '0', 'bc_lib', 'bc_lib_name', 'BC_ENABLED', '1'],
)

bc_lib2_src = custom_target(
  'bx_bc_lib2_c',
  input : 'src/applets/base/bc/assets/lib2.bc',
  output : 'bx_bc_lib2.c',
  command : [bc_strgen, '@INPUT@', '@OUTPUT@', '0', 'bc_lib2', 'bc_lib2_name', 'BC_ENABLED && BC_ENABLE_EXTRA_MATH', '1'],
)

sources += [bc_help_src, bc_lib_src, bc_lib2_src]

applet_names = run_command(
  python3,
  files('tools/dispatch/generate_dispatch_tables.py'),
  '--meson-list',
  files('src/dispatch/applets.json'),
  check : true,
).stdout().strip().split('\n')

multicall_install_mode = get_option('multicall_install_mode')
install_sh_shortcut = get_option('install_sh_shortcut')
install_bash_shortcut = get_option('install_bash_shortcut')
install_system_conflict_shortcuts = get_option('install_system_conflict_shortcuts')
install_wget_shortcut = mira_embed_enabled
disabled_shell_shortcut_names = []
if not install_sh_shortcut
  disabled_shell_shortcut_names += ['sh']
endif
if not install_bash_shortcut
  disabled_shell_shortcut_names += ['bash']
endif
system_conflict_applet_names = ['ps', 'halt', 'poweroff', 'reboot', 'init', 'linuxrc', 'syslogd', 'ping', 'dmesg', 'kill', 'mount', 'screen', 'setsid', 'umount']
systemwide_disabled_applet_names = ['ps', 'tar', 'cpio', 'find', 'xargs']
install_bindir = join_paths(get_option('prefix'), get_option('bindir'))
install_bx_bindir = install_bindir
install_bx_path = join_paths(install_bx_bindir, 'bx')
applet_link_target = 'bx'
if install_bx_bindir != install_bindir
  applet_link_target = install_bx_path
endif

bx_include_dirs = include_directories('src', 'src/dispatch', 'src/applets/base/bc/include')

stdbuf_preload = shared_module(
  'stdbuf',
  [
  'src/lib/stdio_buffer_preload.c',
    'src/lib/size_parse.c',
  ],
  include_directories : bx_include_dirs,
  dependencies : [cc.find_library('m', required : false)],
  # This module is injected into arbitrary non-sanitized COMMAND processes.
  # Instrumenting it would add unresolved sanitizer-runtime dependencies.
  override_options : ['b_sanitize=none'],
  c_args : cc.get_supported_arguments(['-fno-sanitize=all']),
  install : true,
  install_dir : stdbuf_install_subdir,
)

bx_exe = executable('bx',
  sources,
  include_directories : bx_include_dirs,
  dependencies : bx_deps,
  # Dispatch entries and wrapper calls provide roots for these archives.
  # Keep archive membership demand-driven instead of forcing unreachable
  # implementation objects into the multicall binary.
  link_with : [
    nc_lib,
    socat_lib,
    screen_lib,
    edit_wrapper_lib,
    edit_vim_lib,
    xxd_lib,
    jq_vendor_lib,
    jq_lib,
    manual_lib,
    bx_zip_lib,
  ],
  # Traceroute transport modules self-register from constructors and have no
  # ordinary symbol edge from the frontend.
  link_whole : [traceroute_lib],
  link_args : bx_link_args,
  install : true,
  install_dir : install_bx_bindir
)

install_subdir('src/applets/system/screen/utf8encodings',
  install_dir : join_paths(get_option('datadir'), 'bx', 'screen')
)

install_subdir('src/applets/text/edit/runtime',
  install_dir : edit_vim_runtime_install_subdir,
  strip_directory : true
)

if multicall_install_mode == 'symlink'
  foreach applet : applet_names
    if disabled_shell_shortcut_names.contains(applet)
      continue
    endif
    if applet == 'wget' and not install_wget_shortcut
      continue
    endif
    if systemwide_disabled_applet_names.contains(applet)
      continue
    endif
    if system_conflict_applet_names.contains(applet) and not install_system_conflict_shortcuts
      continue
    endif
    install_symlink(applet,
      pointing_to : applet_link_target,
      install_dir : get_option('bindir')
    )
  endforeach
else
  foreach applet : applet_names
    if disabled_shell_shortcut_names.contains(applet)
      continue
    endif
    if applet == 'wget' and not install_wget_shortcut
      continue
    endif
    if systemwide_disabled_applet_names.contains(applet)
      continue
    endif
    if system_conflict_applet_names.contains(applet) and not install_system_conflict_shortcuts
      continue
    endif
    wrapper_conf = configuration_data()
    wrapper_conf.set('BX_PATH', install_bx_path)
    wrapper_conf.set('APPLET', applet)
    wrapper = configure_file(
      input : 'assets/install/applet-shebang.in',
      output : applet,
      configuration : wrapper_conf,
    )
    install_data(wrapper,
      install_dir : get_option('bindir'),
      install_mode : 'rwxr-xr-x'
    )
  endforeach
endif

if get_option('install_ls_colors')
  install_data('assets/lscolors.dircolors',
    install_dir : lscolors_install_subdir,
    rename : 'LS_COLORS'
  )

  lscolors_profile_conf = configuration_data()
  lscolors_profile_conf.set(
    'LSCOLORS_FILE',
    lscolors_install_path
  )
  lscolors_profile = configure_file(
    input : 'assets/install/bx-lscolors-profile.sh.in',
    output : 'bx-lscolors.sh',
    configuration : lscolors_profile_conf,
  )
  install_data(lscolors_profile,
    install_dir : join_paths(get_option('sysconfdir'), 'profile.d'),
    install_mode : 'rw-r--r--'
  )
endif
