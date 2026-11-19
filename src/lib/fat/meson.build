fat_config = configuration_data()
fat_config.set10('BX_FAT_LINUX_BLOCK', cc.compiles('''
  #include <stddef.h>
  #include <linux/fs.h>
  #include <sys/ioctl.h>
  int main(void) { return BLKSSZGET == BLKGETSIZE64; }
''', name : 'Linux block device geometry'))
configure_file(output : 'fat-config.h', configuration : fat_config)
fat_lib = static_library(
  'bx_fat',
  files(
    'io.c', 'format/mkfs_msdos.c', 'repair/boot.c', 'repair/check.c',
    'repair/dir.c', 'repair/fat.c', 'repair/fsutil.c',
    '../../applets/system/fat/mkfs_fat.c', '../../applets/system/fat/fsck_fat.c',
    '../../applets/system/fat/mkfs_dos.c',
  ),
  c_args : ['-D_GNU_SOURCE', '-D_FILE_OFFSET_BITS=64'],
  include_directories : [bx_include_dirs, include_directories('.')],
)
