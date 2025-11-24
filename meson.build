project('bx', 'c',
  version : '0.1',
  meson_version : '>=1.6.0',
  default_options : ['warning_level=3', 'c_std=c11', 'werror=true']
)

add_project_arguments('-D_POSIX_C_SOURCE=200809L', '-DBX_VERSION="' + meson.project_version() + '"', language : 'c')

lscolors_install_subdir = join_paths(get_option('datadir'), 'bx')
lscolors_install_path = join_paths(join_paths(get_option('prefix'), lscolors_install_subdir), 'lscolors.dircolors')
add_project_arguments('-DBX_LSCOLORS_FILE="' + lscolors_install_path + '"', language : 'c')

screen_utf8_install_subdir = join_paths(get_option('datadir'), 'bx', 'screen', 'utf8encodings')
screen_utf8_install_path = join_paths(get_option('prefix'), screen_utf8_install_subdir)
screen_system_rc_path = join_paths(get_option('prefix'), get_option('sysconfdir'), 'bx', 'screenrc')

cc = meson.get_compiler('c')
warn_flags = [
  '-Wshadow',
  '-Wstrict-prototypes',
  '-Wmissing-prototypes',
  '-Wpointer-arith',
  '-Wcast-qual',
]
add_project_arguments(cc.get_supported_arguments(warn_flags), language : 'c')

static_binary_opt = get_option('static_binary')
static_link_supported = cc.has_link_argument('-static')
static_no_pie_supported = cc.has_link_argument('-no-pie')
compiler_targets_glibc = cc.has_header_symbol('features.h', '__GLIBC__')
musl_gcc = find_program('musl-gcc', required : false)

use_static_binary = false
if static_binary_opt == 'enabled'
  if not static_link_supported
    error('static_binary=enabled requires a toolchain that supports -static')
  endif
  use_static_binary = true
elif static_binary_opt == 'auto'
  if compiler_targets_glibc
    if musl_gcc.found()
      warning('static_binary=auto: musl-gcc is available but this configure did not select it; using shared linkage. Re-run setup via scripts/dev/meson-setup.sh or CC=musl-gcc.')
    endif
  elif static_link_supported
    use_static_binary = true
  else
    warning('static_binary=auto: toolchain lacks -static; using shared linkage')
  endif
endif

bx_static_link_args = []
if use_static_binary
  bx_static_link_args += ['-static']
  if static_no_pie_supported and cc.get_id() != 'clang'
    bx_static_link_args += ['-no-pie']
  endif
endif
bx_link_args = bx_static_link_args

mira_embed_opt = get_option('mira_embed')
mira_embed_dep = declare_dependency()
mira_embed_enabled = false
if mira_embed_opt != 'disabled'
  if use_static_binary
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
  else
    mira_embed_dep = dependency(
      'mira-embed',
      required : mira_embed_opt == 'enabled',
      method : 'pkg-config',
    )
    mira_embed_enabled = mira_embed_dep.found()
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
      error('mira-embed found but is not fully static-linkable with this toolchain/dependency set')
    endif
    warning('mira-embed found but is not fully static-linkable; disabling wget mira-embed backend')
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

bx_deps = [cc.find_library('m', required : false), threads_dep, pcre2_dep]
if mira_embed_enabled
  bx_deps += [mira_embed_dep]
endif

# shared bx runtime and helpers
bx_core_sources = [
  'src/bx/main.c',
  'src/bx/libbx.c',
  'src/dispatch/applets.c',
]

bx_lib_sources = [
  'src/lib/cli_common.c',
  'src/lib/args_common.c',
  'src/lib/argv_packer.c',
  'src/lib/backup_ops.c',
  'src/lib/color.c',
  'src/lib/child_runner.c',
  'src/lib/copy_data.c',
  'src/lib/copy_metadata.c',
  'src/lib/copy_tree.c',
  'src/lib/fd_ops.c',
  'src/lib/file_info_fmt.c',
  'src/lib/fopen_dash.c',
  'src/lib/id_parse.c',
  'src/lib/mode_parse.c',
  'src/lib/mount_table.c',
  'src/lib/output.c',
  'src/lib/overwrite_ops.c',
  'src/lib/path_ops.c',
  'src/lib/prompt_ops.c',
  'src/lib/remove_ops.c',
  'src/lib/same_file.c',
  'src/lib/signal_names.c',
  'src/lib/size_parse.c',
  'src/lib/stat_ops.c',
  'src/lib/statx_compat.c',
  'src/lib/time_parse.c',
  'src/lib/update_policy.c',
  'src/lib/xreadwrite.c',
]

bx_crypto_sources = [
  'src/crypto/digest_util.c',
  'src/crypto/digestsum.c',
  'src/crypto/md5.c',
  'src/crypto/sha1.c',
  'src/crypto/sha256.c',
  'src/crypto/sha512.c',
]

bx_archive_applet_sources = [
  'src/applets/archive/archive_common.c',
  'src/applets/archive/archive_fs.c',
  'src/applets/archive/cpio/cpio.c',
  'src/applets/archive/cpio/cpio_backend.c',
  'src/applets/archive/tar/tar.c',
  'src/applets/archive/tar/tar_backend.c',
]

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
  'src/applets/base/head/head.c',
  'src/applets/base/hostid/hostid.c',
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
  'src/applets/base/tr/tr.c',
  'src/applets/base/truncate/truncate.c',
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
  'src/search/filter.c',
  'src/search/metadata.c',
  'src/search/literal.c',
  'src/search/ignore.c',
  'src/search/options.c',
  'src/search/pcre2_matcher.c',
  'src/search/search.c',
  'src/search/walk.c',
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
  include_directories : include_directories('src/applets/network/traceroute'),
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
screen_crypt_dep = cc.find_library('crypt', required : false)
if screen_crypt_dep.found()
  screen_deps += [screen_crypt_dep]
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
  dependencies : screen_deps,
  c_args : screen_c_args,
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

applet_names = ['ash', 'sh', 'bc', 'cat', 'cut', 'comm', 'date', 'echo', 'ed', 'expand', 'fold', 'head', 'hostid', 'id', 'join', 'logname', 'nl', 'od', 'paste', 'nproc', 'numfmt', 'nc', 'netcat', 'which', 'ln', 'link', 'unlink', 'readlink', 'realpath', 'basename', 'dirname', 'dircolors', 'pathchk', 'ls', 'dir', 'vdir', 'stat', 'df', 'du', 'sync', 'dd', 'printf', 'env', 'printenv', 'pwd', 'tty', 'stty', 'screen', 'getty', 'setsid', 'dhcp', 'udhcpc', 'ping', 'nice', 'nohup', 'timeout', 'init', 'switch_root', 'mount', 'umount', 'chroot', 'fuser', 'kill', 'killall', 'peekfd', 'ps', 'pslog', 'pstree', 'prtstat', 'dmesg', 'reboot', 'halt', 'poweroff', 'chmod', 'chown', 'chgrp', 'rm', 'mkdir', 'rmdir', 'mkfifo', 'mknod', 'mktemp', 'touch', 'truncate', 'shred', 'install', 'cp', 'mv', 'wget', 'traceroute', 'expr', 'cksum', 'md5sum', 'sha1sum', 'base64', 'shuf', 'yes', 'whoami', 'sleep', 'uname', 'wc', 'uniq', 'unexpand', 'sum', 'tac', 'tail', 'tee', 'test', '[', 'tr', 'seq', 'split', 'sort', 'tar', 'cpio', 'true', 'false', 'grep', 'egrep', 'fgrep', 'rg', 'bxgrep', 'bxrg', 'fd', 'find', 'xargs']

multicall_install_mode = get_option('multicall_install_mode')
install_sh_shortcut = get_option('install_sh_shortcut')
install_system_conflict_shortcuts = get_option('install_system_conflict_shortcuts')
install_init_shortcut = get_option('install_init_shortcut')
system_conflict_applet_names = ['ps', 'halt', 'poweroff', 'reboot', 'ping', 'dmesg', 'kill', 'mount', 'screen', 'setsid', 'umount']
systemwide_disabled_applet_names = ['ps', 'grep', 'egrep', 'fgrep', 'bxgrep', 'rg', 'bxrg', 'fd', 'tar', 'cpio', 'find', 'xargs']
install_bindir = join_paths(get_option('prefix'), get_option('bindir'))
install_bx_bindir = install_bindir
if install_init_shortcut and install_bindir.startswith('/usr/')
  install_bx_bindir = '/bin'
endif
install_bx_path = join_paths(install_bx_bindir, 'bx')
applet_link_target = 'bx'
if install_bx_bindir != install_bindir
  applet_link_target = install_bx_path
endif

bx_include_dirs = include_directories('src', 'src/dispatch', 'src/applets/base/bc/include')

bx_exe = executable('bx',
  sources,
  include_directories : bx_include_dirs,
  dependencies : bx_deps,
  link_whole : [nc_lib, traceroute_lib, screen_lib],
  link_args : bx_link_args,
  install : true,
  install_dir : install_bx_bindir
)

install_subdir('src/applets/system/screen/utf8encodings',
  install_dir : join_paths(get_option('datadir'), 'bx', 'screen')
)

if multicall_install_mode == 'symlink'
  foreach applet : applet_names
    if applet == 'sh' and not install_sh_shortcut
      continue
    endif
    if applet == 'init' and not install_init_shortcut
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
    if applet == 'sh' and not install_sh_shortcut
      continue
    endif
    if applet == 'init' and not install_init_shortcut
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
      input : 'scripts/dev/applet-shebang.in',
      output : applet,
      configuration : wrapper_conf,
    )
    install_data(wrapper,
      install_dir : get_option('bindir'),
      install_mode : 'rwxr-xr-x'
    )
  endforeach
endif

if install_init_shortcut
  if install_bx_bindir != '/'
    install_symlink('init',
      pointing_to : install_bx_path,
      install_dir : '/'
    )
  endif

  if install_bx_bindir != '/sbin'
    install_symlink('init',
      pointing_to : install_bx_path,
      install_dir : '/sbin'
    )
  endif
endif

if get_option('install_ls_colors')
  install_data('config/lscolors.dircolors',
    install_dir : lscolors_install_subdir
  )

  lscolors_profile_conf = configuration_data()
  lscolors_profile_conf.set(
    'LSCOLORS_FILE',
    lscolors_install_path
  )
  lscolors_profile = configure_file(
    input : 'scripts/dev/bx-lscolors-profile.sh.in',
    output : 'bx-lscolors.sh',
    configuration : lscolors_profile_conf,
  )
  install_data(lscolors_profile,
    install_dir : join_paths(get_option('sysconfdir'), 'profile.d'),
    install_mode : 'rw-r--r--'
  )
endif

if get_option('tests')
  python3 = find_program('python3')
  which_ref = find_program('which')
  cp_ref = find_program('cp')
  unshare = find_program('unshare', required : false)

  test_which = find_program('tests/applets/base/which/test_which.sh')
  test('which unit test', test_which, args : [bx_exe])

  test_cp_differential = files('tests/applets/base/cp/test_cp_differential.py')
  test('cp differential', python3, args : [test_cp_differential, cp_ref, bx_exe, meson.current_build_dir()])

  test_cp_cleanup_state = files('tests/applets/base/cp/test_cp_cleanup_state.py')
  test('cp cleanup state', python3, args : [test_cp_cleanup_state, bx_exe, meson.current_build_dir()])

  test_cp_reference_resolution = files('tests/applets/base/cp/test_cp_reference_resolution.py')
  test('cp reference resolution', python3, args : [test_cp_reference_resolution])

  test_coreutils_symlink_reference_resolution = files('tests/test_coreutils_symlink_reference_resolution.py')
  test('coreutils symlink reference resolution', python3, args : [test_coreutils_symlink_reference_resolution])

  test_bx_symlink_dispatch_setup = files('tests/test_bx_symlink_dispatch_setup.py')
  test('bx symlink dispatch setup', python3, args : [test_bx_symlink_dispatch_setup])

  test_audit_thin_applets = files('tests/audit_thin_applets.py')
  test('audit thin applets', python3, args : [test_audit_thin_applets, bx_exe, meson.current_build_dir()])

  test_fuzz_reference_resolution = files('tests/test_fuzz_reference_resolution.py')
  test('fuzz reference resolution', python3, args : [test_fuzz_reference_resolution])

  test_covering_generator = files('tests/test_covering_generator.py')
  test('covering generator', python3, args : [test_covering_generator])

  test_cp_preserve_metadata = files('tests/applets/base/cp/test_cp_preserve_metadata.py')
  test('cp preserve metadata', python3, args : [test_cp_preserve_metadata, bx_exe, meson.current_build_dir()])

  test_security_context_unsupported = files('tests/test_security_context_unsupported.py')
  test('security-context unsupported', python3, args : [test_security_context_unsupported, bx_exe])

  test_mv_differential = files('tests/applets/base/mv/test_mv_differential.py')
  mv_ref = find_program('mv')
  test('mv differential', python3, args : [test_mv_differential, mv_ref, bx_exe, meson.current_build_dir()])

  test_mv_cli_differential = files('tests/applets/base/mv/test_mv_cli_differential.py')
  test('mv cli differential', python3, args : [test_mv_cli_differential, mv_ref, bx_exe, meson.current_build_dir()])

  expr_ref = find_program('expr')
  test_expr_differential = files('tests/applets/base/expr/test_expr_differential.py')
  test('expr differential', python3, args : [test_expr_differential, expr_ref, bx_exe, meson.current_build_dir()])

  test_md5_vectors = executable(
    'test-md5-vectors',
    ['tests/applets/base/md5sum/test_md5_vectors.c', 'src/crypto/md5.c'],
    include_directories : include_directories('src', 'src/dispatch'),
  )
  test('md5 vectors', test_md5_vectors)

  test_sha1_vectors = executable(
    'test-sha1-vectors',
    ['tests/applets/base/sha1sum/test_sha1_vectors.c', 'src/crypto/sha1.c'],
    include_directories : include_directories('src', 'src/dispatch'),
  )
  test('sha1 vectors', test_sha1_vectors)

  fast_track_p0_suite = ['fast-track', 'fast-track-p0']
  fast_track_p1_suite = ['fast-track', 'fast-track-p1']
  fast_track_p2_suite = ['fast-track', 'fast-track-p2']

  test_cat_basic = find_program('tests/applets/base/cat/test_cat_basic.sh')
  test('cat basic', test_cat_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_cat_smoke_differential = find_program('tests/applets/base/cat/test_cat_smoke_differential.sh')
  test('cat smoke differential', test_cat_smoke_differential, args : [bx_exe], suite : fast_track_p2_suite)

  test_comm_basic = find_program('tests/applets/base/comm/test_comm_basic.sh')
  test('comm basic', test_comm_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_cut_basic = find_program('tests/applets/base/cut/test_cut_basic.sh')
  test('cut basic', test_cut_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_date_basic = find_program('tests/applets/base/date/test_date_basic.sh')
  test('date basic', test_date_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_echo_basic = find_program('tests/applets/base/echo/test_echo_basic.sh')
  test('echo basic', test_echo_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_echo_differential = files('tests/applets/base/echo/test_echo_differential.py')
  test('echo differential', python3, args : [test_echo_differential, bx_exe, meson.current_build_dir()], suite : fast_track_p2_suite)

  test_bc_basic = find_program('tests/applets/base/bc/test_bc_basic.sh')
  test('bc basic', test_bc_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_ed_basic = find_program('tests/applets/text/ed/test_ed_basic.sh')
  test('ed basic', test_ed_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_expand_basic = find_program('tests/applets/base/expand/test_expand_basic.sh')
  test('expand basic', test_expand_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_fold_basic = find_program('tests/applets/base/fold/test_fold_basic.sh')
  test('fold basic', test_fold_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_head_basic = find_program('tests/applets/base/head/test_head_basic.sh')
  test('head basic', test_head_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_tail_basic = find_program('tests/applets/base/tail/test_tail_basic.sh')
  test('tail basic', test_tail_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_hostid_basic = find_program('tests/applets/base/hostid/test_hostid_basic.sh')
  test('hostid basic', test_hostid_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_id_basic = find_program('tests/applets/base/id/test_id_basic.sh')
  test('id basic', test_id_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_logname_basic = find_program('tests/applets/base/logname/test_logname_basic.sh')
  test('logname basic', test_logname_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_nproc_basic = find_program('tests/applets/base/nproc/test_nproc_basic.sh')
  test('nproc basic', test_nproc_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_numfmt_basic = find_program('tests/applets/base/numfmt/test_numfmt_basic.sh')
  test('numfmt basic', test_numfmt_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_join_basic = find_program('tests/applets/base/join/test_join_basic.sh')
  test('join basic', test_join_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_nl_basic = find_program('tests/applets/base/nl/test_nl_basic.sh')
  test('nl basic', test_nl_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_od_basic = find_program('tests/applets/base/od/test_od_basic.sh')
  test('od basic', test_od_basic, args : [bx_exe], suite : fast_track_p2_suite)
  test_od_smoke_differential = find_program('tests/applets/base/od/test_od_smoke_differential.sh')
  test('od smoke differential', test_od_smoke_differential, args : [bx_exe], suite : fast_track_p2_suite)
  test_od_audit = files('tests/applets/base/od/od_audit.py')
  test('od audit differential', python3, args : [test_od_audit, bx_exe, meson.current_build_dir()], suite : fast_track_p2_suite)
  test_od_fuzz = files('tests/applets/base/od/test_od_fuzz.py')
  test('od fuzz', python3, args : [test_od_fuzz, bx_exe, meson.current_build_dir(), '40', '0'], suite : fast_track_p2_suite)

  test_paste_basic = find_program('tests/applets/base/paste/test_paste_basic.sh')
  test('paste basic', test_paste_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_shuf_basic = find_program('tests/applets/base/shuf/test_shuf_basic.sh')
  test_md5sum_basic = find_program('tests/applets/base/md5sum/test_md5sum_basic.sh')
  test('md5sum basic', test_md5sum_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_md5sum_differential = files('tests/applets/base/md5sum/test_md5sum_differential.py')
  test('md5sum differential', python3, args : [test_md5sum_differential, bx_exe, meson.current_build_dir()], suite : fast_track_p2_suite)

  test_md5sum_check = find_program('tests/applets/base/md5sum/test_md5sum_check.sh')
  test('md5sum check', test_md5sum_check, args : [bx_exe], suite : fast_track_p2_suite)

  test_sha1sum_basic = find_program('tests/applets/base/sha1sum/test_sha1sum_basic.sh')
  test('sha1sum basic', test_sha1sum_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_sha1sum_differential = files('tests/applets/base/sha1sum/test_sha1sum_differential.py')
  test('sha1sum differential', python3, args : [test_sha1sum_differential, bx_exe, meson.current_build_dir()], suite : fast_track_p2_suite)

  test_sha1sum_check = find_program('tests/applets/base/sha1sum/test_sha1sum_check.sh')
  test('sha1sum check', test_sha1sum_check, args : [bx_exe], suite : fast_track_p2_suite)

  test_cksum_basic = find_program('tests/applets/base/cksum/test_cksum_basic.sh')
  test('cksum basic', test_cksum_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_cksum_check = find_program('tests/applets/base/cksum/test_cksum_check.sh')
  test('cksum check', test_cksum_check, args : [bx_exe], suite : fast_track_p2_suite)

  test_base64_basic = find_program('tests/applets/base/base64/test_base64_basic.sh')
  test('base64 basic', test_base64_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_base64_smoke_differential = find_program('tests/applets/base/base64/test_base64_smoke_differential.sh')
  test('base64 smoke differential', test_base64_smoke_differential, args : [bx_exe], suite : fast_track_p2_suite)

  test_shuf_basic = find_program('tests/applets/base/shuf/test_shuf_basic.sh')
  test('shuf basic', test_shuf_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_shuf_smoke_differential = find_program('tests/applets/base/shuf/test_shuf_smoke_differential.sh')
  test('shuf smoke differential', test_shuf_smoke_differential, args : [bx_exe], suite : fast_track_p1_suite)

  test_sort_basic = find_program('tests/applets/base/sort/test_sort_basic.sh')
  test('sort basic', test_sort_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_tr_basic = find_program('tests/applets/base/tr/test_tr_basic.sh')
  test('tr basic', test_tr_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_tr_smoke_differential = find_program('tests/applets/base/tr/test_tr_smoke_differential.sh')
  test('tr smoke differential', test_tr_smoke_differential, args : [bx_exe], suite : fast_track_p1_suite)

  test_tac_basic = find_program('tests/applets/base/tac/test_tac_basic.sh')
  test('tac basic', test_tac_basic, args : [bx_exe], suite : fast_track_p0_suite)

  test_ln_basic = find_program('tests/applets/base/ln/test_ln_basic.sh')
  test('ln basic', test_ln_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_ln_smoke_differential = find_program('tests/applets/base/ln/test_ln_smoke_differential.sh')
  test('ln smoke differential', test_ln_smoke_differential, args : [bx_exe], suite : fast_track_p2_suite)

  test_link_basic = find_program('tests/applets/base/link/test_link_basic.sh')
  test('link basic', test_link_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_link_smoke_differential = find_program('tests/applets/base/link/test_link_smoke_differential.sh')
  test('link smoke differential', test_link_smoke_differential, args : [bx_exe], suite : fast_track_p2_suite)

  test_unlink_basic = find_program('tests/applets/base/unlink/test_unlink_basic.sh')
  test('unlink basic', test_unlink_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_unlink_smoke_differential = find_program('tests/applets/base/unlink/test_unlink_smoke_differential.sh')
  test('unlink smoke differential', test_unlink_smoke_differential, args : [bx_exe], suite : fast_track_p2_suite)

  test_readlink_basic = find_program('tests/applets/base/readlink/test_readlink_basic.sh')
  test('readlink basic', test_readlink_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_readlink_smoke_differential = find_program('tests/applets/base/readlink/test_readlink_smoke_differential.sh')
  test('readlink smoke differential', test_readlink_smoke_differential, args : [bx_exe], suite : fast_track_p2_suite)

  test_realpath_basic = find_program('tests/applets/base/realpath/test_realpath_basic.sh')
  test('realpath basic', test_realpath_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_realpath_smoke_differential = find_program('tests/applets/base/realpath/test_realpath_smoke_differential.sh')
  test('realpath smoke differential', test_realpath_smoke_differential, args : [bx_exe], suite : fast_track_p2_suite)

  test_basename_basic = find_program('tests/applets/base/basename/test_basename_basic.sh')
  test('basename basic', test_basename_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_basename_smoke_differential = find_program('tests/applets/base/basename/test_basename_smoke_differential.sh')
  test('basename smoke differential', test_basename_smoke_differential, args : [bx_exe], suite : fast_track_p2_suite)

  test_dirname_basic = find_program('tests/applets/base/dirname/test_dirname_basic.sh')
  test('dirname basic', test_dirname_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_dirname_smoke_differential = find_program('tests/applets/base/dirname/test_dirname_smoke_differential.sh')
  test('dirname smoke differential', test_dirname_smoke_differential, args : [bx_exe], suite : fast_track_p2_suite)

  test_dircolors_basic = find_program('tests/applets/base/dircolors/test_dircolors_basic.sh')
  test('dircolors basic', test_dircolors_basic, args : [bx_exe, lscolors_install_path], suite : fast_track_p0_suite)

  test_pathchk_basic = find_program('tests/applets/base/pathchk/test_pathchk_basic.sh')
  test('pathchk basic', test_pathchk_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_pathchk_smoke_differential = find_program('tests/applets/base/pathchk/test_pathchk_smoke_differential.sh')
  test('pathchk smoke differential', test_pathchk_smoke_differential, args : [bx_exe], suite : fast_track_p2_suite)

  test_ls_basic = find_program('tests/applets/base/ls/test_ls_basic.sh')
  test('ls basic', test_ls_basic, args : [bx_exe], suite : fast_track_p0_suite)

  test_ls_smoke_differential = find_program('tests/applets/base/ls/test_ls_smoke_differential.sh')
  test('ls smoke differential', test_ls_smoke_differential, args : [bx_exe], suite : fast_track_p0_suite)

  test_stat_basic = find_program('tests/applets/base/stat/test_stat_basic.sh')
  test('stat basic', test_stat_basic, args : [bx_exe], suite : fast_track_p0_suite)

  test_stat_smoke_differential = find_program('tests/applets/base/stat/test_stat_smoke_differential.sh')
  test('stat smoke differential', test_stat_smoke_differential, args : [bx_exe], suite : fast_track_p0_suite)

  test_df_basic = find_program('tests/applets/base/df/test_df_basic.sh')
  test('df basic', test_df_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_df_smoke_differential = find_program('tests/applets/base/df/test_df_smoke_differential.sh')
  test('df smoke differential', test_df_smoke_differential, args : [bx_exe], suite : fast_track_p1_suite)

  test_du_basic = find_program('tests/applets/base/du/test_du_basic.sh')
  test('du basic', test_du_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_du_smoke_differential = find_program('tests/applets/base/du/test_du_smoke_differential.sh')
  test('du smoke differential', test_du_smoke_differential, args : [bx_exe], suite : fast_track_p1_suite)

  test_sync_basic = find_program('tests/applets/base/sync/test_sync_basic.sh')
  test('sync basic', test_sync_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_sync_smoke_differential = find_program('tests/applets/base/sync/test_sync_smoke_differential.sh')
  test('sync smoke differential', test_sync_smoke_differential, args : [bx_exe], suite : fast_track_p1_suite)

  test_dd_basic = find_program('tests/applets/text/dd/test_dd_basic.sh')
  test('dd basic', test_dd_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_dd_smoke_differential = find_program('tests/applets/text/dd/test_dd_smoke_differential.sh')
  test('dd smoke differential', test_dd_smoke_differential, args : [bx_exe], suite : fast_track_p1_suite)

  test_printf_basic = find_program('tests/applets/base/printf/test_printf_basic.sh')
  test('printf basic', test_printf_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_printf_smoke_differential = find_program('tests/applets/base/printf/test_printf_smoke_differential.sh')
  test('printf smoke differential', test_printf_smoke_differential, args : [bx_exe], suite : fast_track_p1_suite)

  test_env_basic = find_program('tests/applets/base/env/test_env_basic.sh')
  test('env basic', test_env_basic, args : [bx_exe], suite : fast_track_p0_suite)

  test_env_smoke_differential = find_program('tests/applets/base/env/test_env_smoke_differential.sh')
  test('env smoke differential', test_env_smoke_differential, args : [bx_exe], suite : fast_track_p0_suite)

  test_printenv_basic = find_program('tests/applets/base/printenv/test_printenv_basic.sh')
  test('printenv basic', test_printenv_basic, args : [bx_exe], suite : fast_track_p0_suite)

  test_printenv_smoke_differential = find_program('tests/applets/base/printenv/test_printenv_smoke_differential.sh')
  test('printenv smoke differential', test_printenv_smoke_differential, args : [bx_exe], suite : fast_track_p0_suite)

  test_pwd_basic = find_program('tests/applets/base/pwd/test_pwd_basic.sh')
  test('pwd basic', test_pwd_basic, args : [bx_exe], suite : fast_track_p2_suite)

  test_tty_basic = find_program('tests/applets/base/tty/test_tty_basic.sh')
  test('tty basic', test_tty_basic, args : [bx_exe], suite : fast_track_p0_suite)

  test_tty_smoke_differential = find_program('tests/applets/base/tty/test_tty_smoke_differential.sh')
  test('tty smoke differential', test_tty_smoke_differential, args : [bx_exe], suite : fast_track_p0_suite)

  test_stty_basic = find_program('tests/applets/system/stty/test_stty_basic.sh')
  test('stty basic', test_stty_basic, args : [bx_exe], suite : fast_track_p0_suite)

  test_stty_smoke_differential = find_program('tests/applets/system/stty/test_stty_smoke_differential.sh')
  test('stty smoke differential', test_stty_smoke_differential, args : [bx_exe], suite : fast_track_p0_suite)

  test_screen_basic = find_program('tests/applets/system/screen/test_screen_basic.sh')
  test('screen basic', test_screen_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_getty_basic = find_program('tests/applets/system/getty/test_getty_basic.sh')
  test('getty basic', test_getty_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_getty_console_vm_smoke = find_program('tests/applets/system/getty/test_getty_console_vm_smoke.sh')
  test('getty console vm smoke', test_getty_console_vm_smoke, args : [bx_exe], suite : fast_track_p1_suite)

  test_setsid_basic = find_program('tests/applets/system/setsid/test_setsid_basic.sh')
  test('setsid basic', test_setsid_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_dhcp_basic = files('tests/applets/network/dhcp/test_dhcp_basic.py')
  test('dhcp basic', python3, args : [test_dhcp_basic, bx_exe], suite : fast_track_p1_suite)

  test_udhcpc_busybox_smoke_differential = files('tests/applets/network/dhcp/test_udhcpc_busybox_smoke_differential.py')
  test('udhcpc busybox smoke differential', python3, args : [test_udhcpc_busybox_smoke_differential, bx_exe], suite : fast_track_p1_suite)

  test_ping_basic = find_program('tests/applets/network/ping/test_ping_basic.sh')
  test('ping basic', test_ping_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_init_basic = find_program('tests/applets/system/init/test_init_basic.sh')
  test('init basic', test_init_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_ash_basic = find_program('tests/applets/shell/ash/test_ash_basic.sh')
  test('ash basic', test_ash_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_sh_busybox_smoke_differential = find_program('tests/applets/shell/ash/test_sh_busybox_smoke_differential.sh')
  test('sh busybox smoke differential', test_sh_busybox_smoke_differential, args : [bx_exe], suite : fast_track_p1_suite)

  test_ash_interactive = files('tests/applets/shell/ash/test_ash_interactive.py')
  test('ash interactive startup', python3, args : [test_ash_interactive, bx_exe], suite : fast_track_p1_suite)
  test('sh interactive startup', python3, args : [test_ash_interactive, bx_exe, 'sh'], suite : fast_track_p1_suite)

  test_ash_rescue_rootfs_smoke = find_program('tests/applets/shell/ash/test_ash_rescue_rootfs_smoke.sh')
  test('ash rescue rootfs smoke', test_ash_rescue_rootfs_smoke, args : [bx_exe], suite : fast_track_p1_suite)

  test_nice_basic = find_program('tests/applets/system/nice/test_nice_basic.sh')
  test('nice basic', test_nice_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_nice_smoke_differential = find_program('tests/applets/system/nice/test_nice_smoke_differential.sh')
  test('nice smoke differential', test_nice_smoke_differential, args : [bx_exe], suite : fast_track_p1_suite)

  test_nohup_basic = find_program('tests/applets/base/nohup/test_nohup_basic.sh')
  test('nohup basic', test_nohup_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_nohup_smoke_differential = find_program('tests/applets/base/nohup/test_nohup_smoke_differential.sh')
  test('nohup smoke differential', test_nohup_smoke_differential, args : [bx_exe], suite : fast_track_p1_suite)

  test_timeout_basic = find_program('tests/applets/system/timeout/test_timeout_basic.sh')
  test('timeout basic', test_timeout_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_timeout_smoke_differential = find_program('tests/applets/system/timeout/test_timeout_smoke_differential.sh')
  test('timeout smoke differential', test_timeout_smoke_differential, args : [bx_exe], suite : fast_track_p1_suite)

  test_sleep_basic = find_program('tests/applets/base/sleep/test_sleep_basic.sh')
  test('sleep basic', test_sleep_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_nc_basic = files('tests/applets/network/nc/test_nc_basic.py')
  test('nc basic', python3, args : [test_nc_basic, bx_exe], suite : fast_track_p1_suite)

  test_nc_busybox_smoke_differential = files('tests/applets/network/nc/test_nc_busybox_smoke_differential.py')
  test('nc busybox smoke differential', python3, args : [test_nc_busybox_smoke_differential, bx_exe], suite : fast_track_p1_suite)

  test_wget_basic = find_program('tests/applets/network/wget/test_wget_basic.sh')
  test('wget basic', test_wget_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_switch_root_basic = find_program('tests/applets/system/switch_root/test_switch_root_basic.sh')
  test('switch_root basic', test_switch_root_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_switch_root_initramfs_smoke = find_program('tests/applets/system/switch_root/test_switch_root_initramfs_smoke.sh')
  test('switch_root initramfs smoke', test_switch_root_initramfs_smoke, args : [bx_exe], suite : fast_track_p1_suite)

  test_chroot_basic = find_program('tests/applets/system/chroot/test_chroot_basic.sh')
  test('chroot basic', test_chroot_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_chroot_smoke_differential = find_program('tests/applets/system/chroot/test_chroot_smoke_differential.sh')
  test('chroot smoke differential', test_chroot_smoke_differential, args : [bx_exe], suite : fast_track_p1_suite)

  test_mount_basic = find_program('tests/applets/system/mount/test_mount_basic.sh')
  test('mount basic', test_mount_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_umount_basic = find_program('tests/applets/system/umount/test_umount_basic.sh')
  test('umount basic', test_umount_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_umount_shutdown_flow = find_program('tests/applets/system/umount/test_umount_shutdown_flow.sh')
  test('umount shutdown-flow integration', test_umount_shutdown_flow, args : [bx_exe], suite : fast_track_p1_suite)

  test_kill_basic = find_program('tests/applets/system/kill/test_kill_basic.sh')
  test('kill basic', test_kill_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_kill_functional = files('tests/applets/system/kill/test_kill_functional.py')
  test('kill functional', python3, args : [test_kill_functional, bx_exe], suite : fast_track_p1_suite)

  test_kill_differential = files('tests/applets/system/kill/test_kill_differential.py')
  test('kill differential', python3, args : [test_kill_differential, bx_exe, meson.current_build_dir()], suite : fast_track_p1_suite)

  test_ps_basic = find_program('tests/applets/system/ps/test_ps_basic.sh')
  test('ps basic', test_ps_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_psmisc_basic = find_program('tests/applets/system/psmisc/test_psmisc_basic.sh')
  test('psmisc basic', test_psmisc_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_dmesg_basic = find_program('tests/applets/system/dmesg/test_dmesg_basic.sh')
  test('dmesg basic', test_dmesg_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_reboot_basic = find_program('tests/applets/system/reboot/test_reboot_basic.sh')
  test('reboot basic', test_reboot_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_halt_basic = find_program('tests/applets/system/halt/test_halt_basic.sh')
  test('halt basic', test_halt_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_poweroff_basic = find_program('tests/applets/system/poweroff/test_poweroff_basic.sh')
  test('poweroff basic', test_poweroff_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_chmod_basic = find_program('tests/applets/system/chmod/test_chmod_basic.sh')
  test('chmod basic', test_chmod_basic, args : [bx_exe], suite : fast_track_p0_suite)

  test_chmod_symbolic_x = find_program('tests/applets/system/chmod/test_chmod_symbolic_x.sh')
  test('chmod symbolic x', test_chmod_symbolic_x, args : [bx_exe], suite : fast_track_p0_suite)

  test_chmod_recursive = find_program('tests/applets/system/chmod/test_chmod_recursive.sh')
  test('chmod recursive', test_chmod_recursive, args : [bx_exe], suite : fast_track_p0_suite)

  test_chmod_invalid_modes = find_program('tests/applets/system/chmod/test_chmod_invalid_modes.sh')
  test('chmod invalid modes', test_chmod_invalid_modes, args : [bx_exe], suite : fast_track_p0_suite)

  test_chmod_smoke_differential = find_program('tests/applets/system/chmod/test_chmod_smoke_differential.sh')
  test('chmod smoke differential', test_chmod_smoke_differential, args : [bx_exe], suite : fast_track_p0_suite)

  test_chown_basic = find_program('tests/applets/system/chown/test_chown_basic.sh')
  test('chown basic', test_chown_basic, args : [bx_exe], suite : fast_track_p0_suite)

  test_chown_smoke_differential = find_program('tests/applets/system/chown/test_chown_smoke_differential.sh')
  test('chown smoke differential', test_chown_smoke_differential, args : [bx_exe], suite : fast_track_p0_suite)

  test_chgrp_basic = find_program('tests/applets/system/chgrp/test_chgrp_basic.sh')
  test('chgrp basic', test_chgrp_basic, args : [bx_exe], suite : fast_track_p0_suite)

  test_chgrp_smoke_differential = find_program('tests/applets/system/chgrp/test_chgrp_smoke_differential.sh')
  test('chgrp smoke differential', test_chgrp_smoke_differential, args : [bx_exe], suite : fast_track_p0_suite)

  test_rm_basic = find_program('tests/applets/base/rm/test_rm_basic.sh')
  test('rm basic', test_rm_basic, args : [bx_exe], suite : fast_track_p0_suite)

  test_rm_smoke_differential = find_program('tests/applets/base/rm/test_rm_smoke_differential.sh')
  test('rm smoke differential', test_rm_smoke_differential, args : [bx_exe], suite : fast_track_p0_suite)

  test_mkdir_basic = find_program('tests/applets/base/mkdir/test_mkdir_basic.sh')
  test('mkdir basic', test_mkdir_basic, args : [bx_exe], suite : fast_track_p0_suite)

  test_mkdir_smoke_differential = find_program('tests/applets/base/mkdir/test_mkdir_smoke_differential.sh')
  test('mkdir smoke differential', test_mkdir_smoke_differential, args : [bx_exe], suite : fast_track_p0_suite)

  test_rmdir_basic = find_program('tests/applets/base/rmdir/test_rmdir_basic.sh')
  test('rmdir basic', test_rmdir_basic, args : [bx_exe], suite : fast_track_p0_suite)

  test_rmdir_smoke_differential = find_program('tests/applets/base/rmdir/test_rmdir_smoke_differential.sh')
  test('rmdir smoke differential', test_rmdir_smoke_differential, args : [bx_exe], suite : fast_track_p0_suite)

  test_mkfifo_basic = find_program('tests/applets/base/mkfifo/test_mkfifo_basic.sh')
  test('mkfifo basic', test_mkfifo_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_mkfifo_smoke_differential = find_program('tests/applets/base/mkfifo/test_mkfifo_smoke_differential.sh')
  test('mkfifo smoke differential', test_mkfifo_smoke_differential, args : [bx_exe], suite : fast_track_p1_suite)

  test_mknod_basic = find_program('tests/applets/base/mknod/test_mknod_basic.sh')
  test('mknod basic', test_mknod_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_mknod_smoke_differential = find_program('tests/applets/base/mknod/test_mknod_smoke_differential.sh')
  test('mknod smoke differential', test_mknod_smoke_differential, args : [bx_exe], suite : fast_track_p1_suite)

  test_mktemp_basic = find_program('tests/applets/base/mktemp/test_mktemp_basic.sh')
  test('mktemp basic', test_mktemp_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_mktemp_smoke_differential = find_program('tests/applets/base/mktemp/test_mktemp_smoke_differential.sh')
  test('mktemp smoke differential', test_mktemp_smoke_differential, args : [bx_exe], suite : fast_track_p1_suite)

  test_touch_basic = find_program('tests/applets/base/touch/test_touch_basic.sh')
  test('touch basic', test_touch_basic, args : [bx_exe], suite : fast_track_p0_suite)

  test_touch_smoke_differential = find_program('tests/applets/base/touch/test_touch_smoke_differential.sh')
  test('touch smoke differential', test_touch_smoke_differential, args : [bx_exe], suite : fast_track_p0_suite)

  test_truncate_basic = find_program('tests/applets/base/truncate/test_truncate_basic.sh')
  test('truncate basic', test_truncate_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_truncate_smoke_differential = find_program('tests/applets/base/truncate/test_truncate_smoke_differential.sh')
  test('truncate smoke differential', test_truncate_smoke_differential, args : [bx_exe], suite : fast_track_p1_suite)

  test_shred_basic = find_program('tests/applets/base/shred/test_shred_basic.sh')
  test('shred basic', test_shred_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_shred_smoke_differential = find_program('tests/applets/base/shred/test_shred_smoke_differential.sh')
  test('shred smoke differential', test_shred_smoke_differential, args : [bx_exe], suite : fast_track_p1_suite)

  test_install_basic = find_program('tests/applets/system/install/test_install_basic.sh')
  test('install basic', test_install_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_install_smoke_differential = find_program('tests/applets/system/install/test_install_smoke_differential.sh')
  test('install smoke differential', test_install_smoke_differential, args : [bx_exe], suite : fast_track_p1_suite)

  test_tar_basic = find_program('tests/applets/archive/tar/test_tar_basic.sh')
  test('tar basic', test_tar_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_tar_differential = files('tests/applets/archive/tar/test_tar_differential.py')
  tar_ref = find_program('tar', required : false)
  tar_differential_args = [test_tar_differential, bx_exe, meson.current_build_dir()]
  if tar_ref.found()
    tar_differential_args += [tar_ref.full_path()]
  endif
  test('tar differential', python3, args : tar_differential_args)

  test_cpio_basic = find_program('tests/applets/archive/cpio/test_cpio_basic.sh')
  test('cpio basic', test_cpio_basic, args : [bx_exe], suite : fast_track_p1_suite)

  test_cpio_differential = files('tests/applets/archive/cpio/test_cpio_differential.py')
  cpio_ref = find_program('cpio', required : false)
  cpio_differential_args = [test_cpio_differential, bx_exe, meson.current_build_dir()]
  if cpio_ref.found()
    cpio_differential_args += [cpio_ref.full_path()]
  endif
  test('cpio differential', python3, args : cpio_differential_args)

  test_tier1_safe_replace = find_program('tests/test_tier1_safe_replace.sh')
  test('tier1 safe-replace smoke', test_tier1_safe_replace, args : [bx_exe])

  test_mv_cross_fs_differential = files('tests/applets/base/mv/test_mv_cross_fs_differential.py')
  test('mv cross-fs differential', python3, args : [test_mv_cross_fs_differential, mv_ref, bx_exe, meson.current_build_dir()])

  dd_ref = find_program('dd')
  test_dd_differential = files('tests/applets/text/dd/test_dd_differential.py')
  test('dd differential', python3, args : [test_dd_differential, dd_ref, bx_exe, meson.current_build_dir()])

  test_dd_fuzz = files('tests/applets/text/dd/test_dd_fuzz.py')
  test('dd fuzz', python3, args : [test_dd_fuzz, dd_ref, bx_exe, meson.current_build_dir(), '40', '0'])

  test_cp_fuzz = files('tests/applets/base/cp/test_cp_fuzz.py')
  test('cp fuzz', python3, args : [test_cp_fuzz, cp_ref, bx_exe, meson.current_build_dir(), '20', '0'])

  test_mv_fuzz = files('tests/applets/base/mv/test_mv_fuzz.py')
  test('mv fuzz', python3, args : [test_mv_fuzz, mv_ref, bx_exe, meson.current_build_dir(), '20', '0'])

  test_cp_cli_differential = files('tests/applets/base/cp/test_cp_cli_differential.py')
  test('cp cli differential', python3, args : [test_cp_cli_differential, cp_ref, bx_exe, meson.current_build_dir()])
  
  test_multicall = find_program('tests/test_multicall.sh')
  test('multicall test', test_multicall, args : [bx_exe])

  test_new_applets = find_program('tests/test_new_applets.sh')
  test('new applets basic', test_new_applets, args : [bx_exe])

  test_install_applets = find_program('tests/test_install_applets.sh')
  test(
    'install applet wrappers',
    test_install_applets,
    args : [
      meson.current_build_dir(),
      install_bindir,
      install_bx_path,
      install_sh_shortcut ? 'true' : 'false',
      install_system_conflict_shortcuts ? 'true' : 'false',
      install_init_shortcut ? 'true' : 'false',
    ]
  )

  test_install_missing_shortcuts = find_program('tests/test_install_missing_shortcuts.sh')
  test('install missing shortcuts', test_install_missing_shortcuts, args : [bx_exe])

  test_parity_cli = files('tests/test_parity_cli.py')
  test('which parity cli', python3, args : [test_parity_cli, which_ref, bx_exe, meson.current_build_dir()])

  test_parity_path = files('tests/test_parity_path.py')
  test('which parity path', python3, args : [test_parity_path, which_ref, bx_exe, meson.current_build_dir()])

  test_parity_dot_tilde = files('tests/test_parity_dot_tilde.py')
  test('which parity dot tilde', python3, args : [test_parity_dot_tilde, which_ref, bx_exe, meson.current_build_dir()])
  if unshare.found()
    test_parity_dot_tilde_root = find_program('tests/test_parity_dot_tilde_root.sh')
    test(
      'which parity dot tilde root',
      test_parity_dot_tilde_root,
      args : [python3, test_parity_dot_tilde, which_ref, bx_exe, meson.current_build_dir()],
      env : {'BX_PARITY_CASE' : 'show_tilde_root_home'},
    )
  endif

  test_parity_alias = files('tests/test_parity_alias.py')
  test('which parity alias', python3, args : [test_parity_alias, which_ref, bx_exe, meson.current_build_dir()])

  test_parity_functions = files('tests/test_parity_functions.py')
  test('which parity functions', python3, args : [test_parity_functions, which_ref, bx_exe, meson.current_build_dir()])

  test_parity_exit_status = files('tests/test_parity_exit_status.py')
  test('which parity exit status', python3, args : [test_parity_exit_status, which_ref, bx_exe, meson.current_build_dir()])
  test_grep_basic = find_program('tests/applets/text/grep/test_grep_basic.sh')
  test('grep basic', test_grep_basic, args : [bx_exe], suite : fast_track_p2_suite)
  test_grep_differential = find_program('tests/applets/text/grep/test_grep_differential.py')
  test('grep differential', python3, args : [test_grep_differential, bx_exe, meson.current_build_dir()], suite : fast_track_p2_suite)

  test_rg_basic = find_program('tests/applets/text/rg/test_rg_basic.sh')
  test('rg basic', test_rg_basic, args : [bx_exe], suite : fast_track_p2_suite)
  test_rg_differential = find_program('tests/applets/text/rg/test_rg_differential.py')
  test('rg differential', python3, args : [test_rg_differential, bx_exe, meson.current_build_dir()], suite : fast_track_p2_suite)

  test_fd_basic = find_program('tests/applets/text/fd/test_fd_basic.sh')
  test('fd basic', test_fd_basic, args : [bx_exe], suite : fast_track_p2_suite)
  test_fd_differential = find_program('tests/applets/text/fd/test_fd_differential.py')
  test('fd differential', python3, args : [test_fd_differential, bx_exe, meson.current_build_dir()], suite : fast_track_p2_suite)

  test_find_basic = find_program('tests/applets/base/find/test_find_basic.sh')
  test('find basic', test_find_basic, args : [bx_exe], suite : fast_track_p2_suite)
  test_find_differential = files('tests/applets/base/find/test_find_differential.py')
  test('find differential', python3, args : [test_find_differential, bx_exe, meson.current_build_dir()], suite : fast_track_p2_suite)

  test_xargs_basic = find_program('tests/applets/base/xargs/test_xargs_basic.sh')
  test('xargs basic', test_xargs_basic, args : [bx_exe], suite : fast_track_p2_suite)
  test_xargs_differential = find_program('tests/applets/base/xargs/test_xargs_differential.py')
  test('xargs differential', python3, args : [test_xargs_differential, bx_exe, meson.current_build_dir()], suite : fast_track_p2_suite)
  test_pipeline_differential = find_program('tests/applets/base/xargs/test_pipeline_differential.py')
  test('pipeline differential', python3, args : [test_pipeline_differential, bx_exe, meson.current_build_dir()], suite : fast_track_p2_suite)
  test_exec_status_ownership = find_program('tests/applets/shared/test_exec_status_ownership.sh')
  test('shared exec status ownership', test_exec_status_ownership, args : [bx_exe], suite : fast_track_p2_suite)
  test_exec_substrate_shape = find_program('tests/applets/shared/test_exec_substrate_shape.sh')
  test('shared exec substrate shape', test_exec_substrate_shape, args : [bx_exe], suite : fast_track_p2_suite)
endif
