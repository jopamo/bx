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
  'src/main.c',
  'src/cp/cp.c',
  'src/mv/mv.c',
  'src/which.c',
  'src/applets.c',
  'src/libbx.c',
]

applet_names = ['which', 'cp', 'true', 'false']

bx_exe = executable('bx',
  sources,
  include_directories : include_directories('src'),
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

  test_cp_preserve_metadata = files('tests/cp/test_cp_preserve_metadata.py')
  test('cp preserve metadata', python3, args : [test_cp_preserve_metadata, bx_exe, meson.current_build_dir()])

  test_mv_differential = files('tests/mv/test_mv_differential.py')
  mv_ref = find_program('mv')
  test('mv differential', python3, args : [test_mv_differential, mv_ref, bx_exe, meson.current_build_dir()])

  test_mv_cli_differential = files('tests/mv/test_mv_cli_differential.py')
  test('mv cli differential', python3, args : [test_mv_cli_differential, mv_ref, bx_exe, meson.current_build_dir()])

  test_mv_cross_fs_differential = files('tests/mv/test_mv_cross_fs_differential.py')
  test('mv cross-fs differential', python3, args : [test_mv_cross_fs_differential, mv_ref, bx_exe, meson.current_build_dir()])

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
