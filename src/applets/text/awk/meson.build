awk_lib = static_library('bx_awk',
  'awkgram.tab.c',
  'b.c',
  'lex.c',
  'lib.c',
  'main.c',
  'math.c',
  'parse.c',
  'proctab.c',
  'run.c',
  'tran.c',
  include_directories : bx_include_dirs,
  c_args : ['-D_GNU_SOURCE', '-Wno-unused-parameter', '-Wno-missing-prototypes', '-Wno-shadow', '-Wno-cast-qual'],
)

install_data('LICENSE', 'LICENSE.byacc',
  install_dir : get_option('datadir') / 'licenses' / 'bx' / 'awk',
)
