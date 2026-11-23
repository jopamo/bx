# Require actual libm calls so math.c can inspect the exceptions they raise.
awk_math_args = cc.get_supported_arguments(
  ['-fno-builtin-exp', '-fno-builtin-log', '-fno-builtin-pow', '-fno-builtin-sqrt'],
)

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
  c_args : ['-D_GNU_SOURCE', '-Wno-unused-parameter', '-Wno-missing-prototypes', '-Wno-shadow', '-Wno-cast-qual'] + awk_math_args,
)

install_data('LICENSE', 'LICENSE.byacc',
  install_dir : get_option('datadir') / 'licenses' / 'bx' / 'awk',
)
