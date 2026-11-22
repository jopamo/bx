sed_lib = static_library('bx_sed',
  'compile.c',
  'main.c',
  'misc.c',
  'process.c',
  include_directories : bx_include_dirs,
  c_args : ['-D_GNU_SOURCE'],
)

install_data('LICENSE',
  install_dir : get_option('datadir') / 'licenses' / 'bx' / 'sed',
)
