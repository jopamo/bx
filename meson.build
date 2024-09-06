project('bx', 'c',
  version : '0.1',
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
  'src/main.c',
  'src/which.c',
  'src/applets.c',
  'src/libbx.c',
]

bx_exe = executable('bx',
  sources,
  include_directories : include_directories('src'),
  install : true
)

install_symlink('which',
  pointing_to : 'bx',
  install_dir : get_option('bindir')
)

install_symlink('true',
  pointing_to : 'bx',
  install_dir : get_option('bindir')
)

install_symlink('false',
  pointing_to : 'bx',
  install_dir : get_option('bindir')
)

if get_option('tests')
  python3 = find_program('python3')
  which_ref = find_program('which')
  unshare = find_program('unshare', required : false)

  test_which = find_program('tests/test_which.sh')
  test('which unit test', test_which, args : [bx_exe])
  
  test_multicall = find_program('tests/test_multicall.sh')
  test('multicall test', test_multicall, args : [bx_exe])

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
