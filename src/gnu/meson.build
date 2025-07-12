# Vendored GNU sources for bx.
# Keep the source groupings explicit and hand-maintained: one shared GNU
# runtime layer plus imported GNU applet layers.

bx_gnu_shared_inc = include_directories(
  'shared',
  'shared/gnulib',
  'shared/paxutils',
)

bx_gnu_tar_inc = include_directories(
  'shared',
  'shared/gnulib',
  'shared/paxutils',
  'tar',
)

bx_gnu_cpio_inc = include_directories(
  'cpio',
  'shared',
  'shared/gnulib',
  'shared/paxutils',
)

bx_gnu_shared_gnulib_sources = files(
  'shared/gnulib/acl-errno-valid.c',
  'shared/gnulib/alignalloc.c',
  'shared/gnulib/areadlinkat-with-size.c',
  'shared/gnulib/argmatch.c',
  'shared/gnulib/argp-ba.c',
  'shared/gnulib/argp-eexst.c',
  'shared/gnulib/argp-fmtstream.c',
  'shared/gnulib/argp-fs-xinl.c',
  'shared/gnulib/argp-help.c',
  'shared/gnulib/argp-parse.c',
  'shared/gnulib/argp-pv.c',
  'shared/gnulib/argp-pvh.c',
  'shared/gnulib/argp-version-etc.c',
  'shared/gnulib/argp-xinl.c',
  'shared/gnulib/backup-find.c',
  'shared/gnulib/backupfile.c',
  'shared/gnulib/basename-lgpl.c',
  'shared/gnulib/basename.c',
  'shared/gnulib/bitrotate.c',
  'shared/gnulib/btoc32.c',
  'shared/gnulib/btowc.c',
  'shared/gnulib/c-ctype.c',
  'shared/gnulib/c32_apply_type_test.c',
  'shared/gnulib/c32_get_type_test.c',
  'shared/gnulib/c32isalnum.c',
  'shared/gnulib/c32isprint.c',
  'shared/gnulib/c32tolower.c',
  'shared/gnulib/c32toupper.c',
  'shared/gnulib/chdir-long.c',
  'shared/gnulib/close-stream.c',
  'shared/gnulib/closeout.c',
  'shared/gnulib/dirname-lgpl.c',
  'shared/gnulib/dirname.c',
  'shared/gnulib/error.c',
  'shared/gnulib/exclude.c',
  'shared/gnulib/exitfail.c',
  'shared/gnulib/fcntl.c',
  'shared/gnulib/fdutimensat.c',
  'shared/gnulib/file-has-acl.c',
  'shared/gnulib/fopen.c',
  'shared/gnulib/fseeko.c',
  'shared/gnulib/full-read.c',
  'shared/gnulib/full-write.c',
  'shared/gnulib/getlocalename_l-unsafe.c',
  'shared/gnulib/getopt.c',
  'shared/gnulib/getopt1.c',
  'shared/gnulib/getprogname.c',
  'shared/gnulib/gettime.c',
  'shared/gnulib/hard-locale.c',
  'shared/gnulib/hash.c',
  'shared/gnulib/hashcode-string1.c',
  'shared/gnulib/human.c',
  'shared/gnulib/ialloc.c',
  'shared/gnulib/imaxtostr.c',
  'shared/gnulib/issymlinkat.c',
  'shared/gnulib/localcharset.c',
  'shared/gnulib/localename-environ.c',
  'shared/gnulib/localename-unsafe.c',
  'shared/gnulib/malloc/dynarray_emplace_enlarge.c',
  'shared/gnulib/malloc/dynarray_finalize.c',
  'shared/gnulib/malloc/dynarray_resize.c',
  'shared/gnulib/mbrtoc32.c',
  'shared/gnulib/mbrtowc.c',
  'shared/gnulib/mbscasecmp.c',
  'shared/gnulib/mbszero.c',
  'shared/gnulib/mcel.c',
  'shared/gnulib/memeq.c',
  'shared/gnulib/modechange.c',
  'shared/gnulib/next-prime.c',
  'shared/gnulib/nl_langinfo.c',
  'shared/gnulib/nstrftime.c',
  'shared/gnulib/obstack.c',
  'shared/gnulib/offtostr.c',
  'shared/gnulib/openat-die.c',
  'shared/gnulib/openat-proc.c',
  'shared/gnulib/openat2.c',
  'shared/gnulib/opendirat.c',
  'shared/gnulib/parse-datetime.c',
  'shared/gnulib/posix_memalign.c',
  'shared/gnulib/printf-args.c',
  'shared/gnulib/printf-parse.c',
  'shared/gnulib/priv-set.c',
  'shared/gnulib/progname.c',
  'shared/gnulib/quotearg.c',
  'shared/gnulib/realloc.c',
  'shared/gnulib/reallocarray.c',
  'shared/gnulib/regex.c',
  'shared/gnulib/renameatu.c',
  'shared/gnulib/safe-read.c',
  'shared/gnulib/safe-write.c',
  'shared/gnulib/same-inode.c',
  'shared/gnulib/save-cwd.c',
  'shared/gnulib/savedir.c',
  'shared/gnulib/setlocale_null-unlocked.c',
  'shared/gnulib/setlocale_null.c',
  'shared/gnulib/stat-time.c',
  'shared/gnulib/stdopen.c',
  'shared/gnulib/streq.c',
  'shared/gnulib/stripslash.c',
  'shared/gnulib/strnul.c',
  'shared/gnulib/tempname.c',
  'shared/gnulib/time_rz.c',
  'shared/gnulib/timespec.c',
  'shared/gnulib/umaxtostr.c',
  'shared/gnulib/vasnprintf.c',
  'shared/gnulib/vaszprintf.c',
  'shared/gnulib/version-etc-fsf.c',
  'shared/gnulib/version-etc.c',
  'shared/gnulib/xalignalloc.c',
  'shared/gnulib/xalloc-die.c',
  'shared/gnulib/xgetcwd.c',
  'shared/gnulib/xmalloc.c',
  'shared/gnulib/xsize.c',
  'shared/gnulib/xstrtoumax.c',
  'shared/gnulib/xvasprintf.c',
)

bx_gnu_shared_pax_sources = files(
  'shared/paxutils/mkdtempat.c',
  'shared/paxutils/paxerror.c',
  'shared/paxutils/paxexit-status.c',
  'shared/paxutils/paxexit.c',
  'shared/paxutils/paxnames.c',
  'shared/paxutils/rtapelib.c',
  'shared/paxutils/wordsplit.c',
  'shared/paxutils/xattr-at.c',
)

bx_gnu_tar_sources = files(
  'tar/buffer.c',
  'tar/checkpoint.c',
  'tar/compare.c',
  'tar/create.c',
  'tar/delete.c',
  'tar/exit.c',
  'tar/exclist.c',
  'tar/extract.c',
  'tar/xheader.c',
  'tar/incremen.c',
  'tar/list.c',
  'tar/map.c',
  'tar/misc.c',
  'tar/names.c',
  'tar/sparse.c',
  'tar/suffix.c',
  'tar/system.c',
  'tar/tar.c',
  'tar/transform.c',
  'tar/unlink.c',
  'tar/update.c',
  'tar/utf8.c',
  'tar/warning.c',
  'tar/xattrs.c',
)

bx_gnu_cpio_sources = files(
  'cpio/copyin.c',
  'cpio/copyout.c',
  'cpio/copypass.c',
  'cpio/defer.c',
  'cpio/dstring.c',
  'cpio/global.c',
  'cpio/fatal.c',
  'cpio/main.c',
  'cpio/tar.c',
  'cpio/util.c',
  'cpio/filemode.c',
  'cpio/idcache.c',
  'cpio/makepath.c',
  'cpio/userspec.c',
)

bx_gnu_acl_dep = cc.find_library('acl', required : true)

bx_gnu_shared_deps = [bx_gnu_acl_dep]
gnu_applet_deps += bx_gnu_shared_deps

# GNU shared runtime is built exactly once.
# Applet-specific include directories apply only to applet sources, never to
# shared gnulib/paxutils sources.
bx_gnu_common_c_args = cc.get_supported_arguments([
  '-DHAVE_CONFIG_H=1',
  '-ffat-lto-objects',
  '-Wno-error',
  '-Wno-unused-value',
  '-Wno-duplicated-branches',
  '-Wno-unused-but-set-variable',
  '-Wno-unused-macros',
  '-Wno-old-style-definition',
  '-Wno-sign-compare',
  '-Wno-format-nonliteral',
  '-Wno-shadow',
  '-Wno-missing-prototypes',
  '-Wno-missing-declarations',
  '-Wno-cast-qual',
])

bx_gnu_shared_raw_lib = static_library('bx_gnu_shared_raw',
  bx_gnu_shared_gnulib_sources + bx_gnu_shared_pax_sources,
  include_directories : bx_gnu_shared_inc,
  dependencies : bx_gnu_shared_deps,
  c_args : bx_gnu_common_c_args,
  override_options : ['c_std=gnu23'],
)

bx_gnu_tar_raw_lib = static_library('bx_gnu_tar_raw',
  bx_gnu_tar_sources,
  include_directories : bx_gnu_tar_inc,
  dependencies : bx_gnu_shared_deps,
  c_args : bx_gnu_common_c_args + ['-Dmain=bx_tar_main'],
  override_options : ['c_std=gnu23'],
)

bx_gnu_cpio_raw_lib = static_library('bx_gnu_cpio_raw',
  bx_gnu_cpio_sources,
  include_directories : bx_gnu_cpio_inc,
  dependencies : bx_gnu_shared_deps,
  c_args : bx_gnu_common_c_args + ['-Dmain=bx_cpio_main'],
  override_options : ['c_std=gnu23'],
)

objcopy_prog = find_program('objcopy')
bx_gnu_tar_keep_globals = files('bx_gnu_tar_keep_global_symbols.txt')
bx_gnu_cpio_keep_globals = files('bx_gnu_cpio_keep_global_symbols.txt')

gnu_combine_tar_command = [
  cc.cmd_array(),
  '-nostdlib',
  '-fno-lto',
  '-r',
  '-Wl,--undefined=bx_tar_main',
  '-Wl,--start-group',
  '@INPUT0@',
  '@INPUT1@',
  '-Wl,--end-group',
  '-o',
  '@OUTPUT@',
]

bx_gnu_tar_combined_object = custom_target('bx_gnu_tar_combined_object',
  input : [bx_gnu_tar_raw_lib, bx_gnu_shared_raw_lib],
  output : 'bx_gnu_tar.combined.o',
  command : gnu_combine_tar_command,
)

gnu_localize_tar_command = [
  objcopy_prog,
  '--remove-section=.gnu.lto_*',
  '--remove-section=.gnu.debuglto_*',
  '--keep-global-symbols=@INPUT1@',
  '@INPUT0@',
  '@OUTPUT@',
]

bx_gnu_tar_object = custom_target('bx_gnu_tar_object',
  input : [bx_gnu_tar_combined_object, bx_gnu_tar_keep_globals],
  output : 'bx_gnu_tar.o',
  command : gnu_localize_tar_command,
)

gnu_combine_cpio_command = [
  cc.cmd_array(),
  '-nostdlib',
  '-fno-lto',
  '-r',
  '-Wl,--undefined=bx_cpio_main',
  '-Wl,--start-group',
  '@INPUT0@',
  '@INPUT1@',
  '-Wl,--end-group',
  '-o',
  '@OUTPUT@',
]

bx_gnu_cpio_combined_object = custom_target('bx_gnu_cpio_combined_object',
  input : [bx_gnu_cpio_raw_lib, bx_gnu_shared_raw_lib],
  output : 'bx_gnu_cpio.combined.o',
  command : gnu_combine_cpio_command,
)

gnu_localize_cpio_command = [
  objcopy_prog,
  '--remove-section=.gnu.lto_*',
  '--remove-section=.gnu.debuglto_*',
  '--keep-global-symbols=@INPUT1@',
  '@INPUT0@',
  '@OUTPUT@',
]

bx_gnu_cpio_object = custom_target('bx_gnu_cpio_object',
  input : [bx_gnu_cpio_combined_object, bx_gnu_cpio_keep_globals],
  output : 'bx_gnu_cpio.o',
  command : gnu_localize_cpio_command,
)

gnu_applet_objects += [bx_gnu_tar_object, bx_gnu_cpio_object]
