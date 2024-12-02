project('bx', 'c',
  version : '0.1',
  meson_version : '>=1.6.0',
  default_options : ['warning_level=3', 'c_std=c11', 'werror=true']
)

add_project_arguments('-D_POSIX_C_SOURCE=200809L', '-DBX_VERSION="' + meson.project_version() + '"', language : 'c')

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
      warning('static_binary=auto: musl-gcc is available but this configure did not select it; using shared linkage. Re-run setup via scripts/meson-setup.sh or CC=musl-gcc.')
    endif
  elif static_link_supported
    use_static_binary = true
  else
    warning('static_binary=auto: toolchain lacks -static; using shared linkage')
  endif
endif

bx_link_args = []
if use_static_binary
  bx_link_args += ['-static']
endif

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
    args : ['-static'],
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

bx_deps = []
if mira_embed_enabled
  bx_deps += [mira_embed_dep]
endif

sources = [
  'src/common/args_common.c',
  'src/common/backup_ops.c',
  'src/common/copy_data.c',
  'src/common/copy_metadata.c',
  'src/common/copy_tree.c',
  'src/common/update_policy.c',
  'src/common/path_ops.c',
  'src/common/prompt_ops.c',
  'src/common/same_file.c',
  'src/common/stat_ops.c',
  'src/common/remove_ops.c',
  'src/common/fd_ops.c',
  'src/common/overwrite_ops.c',
  'src/common/md5.c',
  'src/common/digest_util.c',
  'src/main.c',
  'src/ln/ln.c',
  'src/link/link.c',
  'src/unlink/unlink.c',
  'src/readlink/readlink.c',
  'src/realpath/realpath.c',
  'src/basename/basename.c',
  'src/dirname/dirname.c',
  'src/cp/cp.c',
  'src/mv/mv.c',
  'src/expr/expr.c',
  'src/md5sum/md5sum.c',
  'src/base64/base64.c',
  'src/wget.c',
  'src/which.c',
  'src/applets.c',
  'src/libbx.c',
]

applet_names = ['which', 'ln', 'link', 'unlink', 'readlink', 'realpath', 'basename', 'dirname', 'cp', 'mv', 'wget', 'expr', 'md5sum', 'base64', 'true', 'false']

bx_exe = executable('bx',
  sources,
  include_directories : include_directories('src'),
  dependencies : bx_deps,
  link_args : bx_link_args,
  install : true
)

multicall_install_mode = get_option('multicall_install_mode')
install_bindir = join_paths(get_option('prefix'), get_option('bindir'))
install_bx_path = join_paths(install_bindir, 'bx')

if multicall_install_mode == 'symlink'
  foreach applet : applet_names
    install_symlink(applet,
      pointing_to : 'bx',
      install_dir : get_option('bindir')
    )
  endforeach
else
  foreach applet : applet_names
    wrapper_conf = configuration_data()
    wrapper_conf.set('BX_PATH', install_bx_path)
    wrapper_conf.set('APPLET', applet)
    wrapper = configure_file(
      input : 'scripts/applet-shebang.in',
      output : applet,
      configuration : wrapper_conf,
    )
    install_data(wrapper,
      install_dir : get_option('bindir'),
      install_mode : 'rwxr-xr-x'
    )
  endforeach
endif

if get_option('tests')
  python3 = find_program('python3')
  which_ref = find_program('which')
  cp_ref = find_program('cp')
  unshare = find_program('unshare', required : false)

  test_which = find_program('tests/test_which.sh')
  test('which unit test', test_which, args : [bx_exe])

  test_cp_differential = files('tests/cp/test_cp_differential.py')
  test('cp differential', python3, args : [test_cp_differential, cp_ref, bx_exe, meson.current_build_dir()])

  test_cp_cleanup_state = files('tests/cp/test_cp_cleanup_state.py')
  test('cp cleanup state', python3, args : [test_cp_cleanup_state, bx_exe, meson.current_build_dir()])

  test_cp_reference_resolution = files('tests/cp/test_cp_reference_resolution.py')
  test('cp reference resolution', python3, args : [test_cp_reference_resolution])

  test_fuzz_reference_resolution = files('tests/test_fuzz_reference_resolution.py')
  test('fuzz reference resolution', python3, args : [test_fuzz_reference_resolution])

  test_cp_preserve_metadata = files('tests/cp/test_cp_preserve_metadata.py')
  test('cp preserve metadata', python3, args : [test_cp_preserve_metadata, bx_exe, meson.current_build_dir()])

  test_security_context_unsupported = files('tests/test_security_context_unsupported.py')
  test('security-context unsupported', python3, args : [test_security_context_unsupported, bx_exe])

  test_mv_differential = files('tests/mv/test_mv_differential.py')
  mv_ref = find_program('mv')
  test('mv differential', python3, args : [test_mv_differential, mv_ref, bx_exe, meson.current_build_dir()])

  test_mv_cli_differential = files('tests/mv/test_mv_cli_differential.py')
  test('mv cli differential', python3, args : [test_mv_cli_differential, mv_ref, bx_exe, meson.current_build_dir()])

  expr_ref = find_program('expr')
  test_expr_differential = files('tests/expr/test_expr_differential.py')
  test('expr differential', python3, args : [test_expr_differential, expr_ref, bx_exe, meson.current_build_dir()])

  test_md5_vectors = executable(
    'test-md5-vectors',
    ['tests/md5sum/test_md5_vectors.c', 'src/common/md5.c'],
    include_directories : include_directories('src'),
  )
  test('md5 vectors', test_md5_vectors)

  test_md5sum_basic = find_program('tests/md5sum/test_md5sum_basic.sh')
  test('md5sum basic', test_md5sum_basic, args : [bx_exe])

  test_md5sum_check = find_program('tests/md5sum/test_md5sum_check.sh')
  test('md5sum check', test_md5sum_check, args : [bx_exe])

  test_base64_basic = find_program('tests/base64/test_base64_basic.sh')
  test('base64 basic', test_base64_basic, args : [bx_exe])

  test_ln_basic = find_program('tests/ln/test_ln_basic.sh')
  test('ln basic', test_ln_basic, args : [bx_exe])

  test_link_basic = find_program('tests/link/test_link_basic.sh')
  test('link basic', test_link_basic, args : [bx_exe])

  test_unlink_basic = find_program('tests/unlink/test_unlink_basic.sh')
  test('unlink basic', test_unlink_basic, args : [bx_exe])

  test_readlink_basic = find_program('tests/readlink/test_readlink_basic.sh')
  test('readlink basic', test_readlink_basic, args : [bx_exe])

  test_realpath_basic = find_program('tests/realpath/test_realpath_basic.sh')
  test('realpath basic', test_realpath_basic, args : [bx_exe])

  test_basename_basic = find_program('tests/basename/test_basename_basic.sh')
  test('basename basic', test_basename_basic, args : [bx_exe])

  test_dirname_basic = find_program('tests/dirname/test_dirname_basic.sh')
  test('dirname basic', test_dirname_basic, args : [bx_exe])

  test_mv_cross_fs_differential = files('tests/mv/test_mv_cross_fs_differential.py')
  test('mv cross-fs differential', python3, args : [test_mv_cross_fs_differential, mv_ref, bx_exe, meson.current_build_dir()])

  test_cp_fuzz = files('tests/cp/test_cp_fuzz.py')
  test('cp fuzz', python3, args : [test_cp_fuzz, cp_ref, bx_exe, meson.current_build_dir(), '20', '0'])

  test_mv_fuzz = files('tests/mv/test_mv_fuzz.py')
  test('mv fuzz', python3, args : [test_mv_fuzz, mv_ref, bx_exe, meson.current_build_dir(), '20', '0'])

  test_cp_cli_differential = files('tests/cp/test_cp_cli_differential.py')
  test('cp cli differential', python3, args : [test_cp_cli_differential, cp_ref, bx_exe, meson.current_build_dir()])
  
  test_multicall = find_program('tests/test_multicall.sh')
  test('multicall test', test_multicall, args : [bx_exe])

  test_install_applets = find_program('tests/test_install_applets.sh')
  test(
    'install applet wrappers',
    test_install_applets,
    args : [meson.current_build_dir(), install_bindir]
  )

  test_parity_cli = files('tests/test_parity_cli.py')
  test('which parity cli', python3, args : [test_parity_cli, which_ref, bx_exe, meson.current_build_dir()])

  test_parity_path = files('tests/test_parity_path.py')
  test('which parity path', python3, args : [test_parity_path, which_ref, bx_exe, meson.current_build_dir()])

  test_parity_dot_tilde = files('tests/test_parity_dot_tilde.py')
  test('which parity dot tilde', python3, args : [test_parity_dot_tilde, which_ref, bx_exe, meson.current_build_dir()])
  if unshare.found()
    test(
      'which parity dot tilde root',
      unshare,
      args : ['-Ur', python3, test_parity_dot_tilde, which_ref, bx_exe, meson.current_build_dir()],
      env : {'BX_PARITY_CASE' : 'show_tilde_root_home'},
    )
  endif

  test_parity_alias = files('tests/test_parity_alias.py')
  test('which parity alias', python3, args : [test_parity_alias, which_ref, bx_exe, meson.current_build_dir()])

  test_parity_functions = files('tests/test_parity_functions.py')
  test('which parity functions', python3, args : [test_parity_functions, which_ref, bx_exe, meson.current_build_dir()])

  test_parity_exit_status = files('tests/test_parity_exit_status.py')
  test('which parity exit status', python3, args : [test_parity_exit_status, which_ref, bx_exe, meson.current_build_dir()])
endif
