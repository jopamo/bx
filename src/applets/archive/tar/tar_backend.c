#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "applets/archive/archive_codec.h"
#include "applets/archive/archive_common.h"
#include "applets/archive/archive_fs.h"
#include "applets/archive/tar/tar_backend.h"
#include "applets/archive/tar/tar_create.h"
#include "applets/archive/tar/tar_id_map.h"
#include "applets/archive/tar/tar_names.h"
#include "applets/archive/tar/tar_report.h"
#include "applets/archive/tar/tar_reader.h"
#include "applets/archive/tar/tar_select.h"
#include "applets/archive/tar/tar_stream.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/copy_data.h"
#include "lib/mode_parse.h"
#include "lib/path_ops.h"
#include "lib/size_parse.h"
#include "lib/time_parse.h"
#include "lib/thread_count.h"
#include "lib/xreadwrite.h"

#ifdef S_ISVTX
#define BX_TAR_STICKY_BIT S_ISVTX
#elif defined(S_ISTXT)
#define BX_TAR_STICKY_BIT S_ISTXT
#else
#define BX_TAR_STICKY_BIT 01000
#endif

enum bx_tar_mode {
    BX_TAR_MODE_NONE = 0,
    BX_TAR_MODE_CATENATE,
    BX_TAR_MODE_CREATE,
    BX_TAR_MODE_COMPARE,
    BX_TAR_MODE_TEST_LABEL,
    BX_TAR_MODE_LIST,
    BX_TAR_MODE_EXTRACT,
    BX_TAR_MODE_APPEND,
    BX_TAR_MODE_UPDATE,
    BX_TAR_MODE_DELETE,
};

struct bx_tar_options {
    enum bx_tar_mode mode;
    const char* unsupported_mode;
    const char* unsupported_external_compress_option;
    char* unsupported_external_compress_program;
    bool saw_mode_option;
    const char* archive_path;
    bool to_stdout;
    bool keep_old_files;
    bool overwrite;
    bool unlink_first;
    bool recursive_unlink;
    bool verbose_reports;
    bool report_mapped_names;
    bool report_block_numbers;
    bool report_totals;
    char* index_file_path;
    const struct bx_archive_codec* codec;
    bool auto_compress;
    bool absolute_names;
    bool touch_mtime;
    bool sort_name;
    const char* starting_file;
    bool format_ustar;
    bool numeric_owner;
    bool owner_set;
    bool group_set;
    uid_t owner;
    gid_t group;
    struct bx_tar_id_map owner_map;
    struct bx_tar_id_map group_map;
    bool fixed_mtime;
    struct timespec mtime;
    bool xattrs;
    bool acls;
    bool no_mt;
    char* mode_text;
    bool newer_active;
    bool newer_use_ctime;
    struct timespec newer_time;
    size_t strip_components;
    int threads;
    int compress_threads;
    uintmax_t mt_chunk_size;
    const char* one_top_level;
    struct bx_tar_transform_rule name_transform;
    struct bx_tar_create_options create_options;
    struct bx_archive_name_list source_archives;
};

enum bx_tar_option_arg_mode {
    BX_TAR_OPTARG_NONE = 0,
    BX_TAR_OPTARG_REQUIRED,
};

enum bx_tar_option_effect {
    BX_TAR_OPT_NOOP = 0,
    BX_TAR_OPT_MODE_CATENATE,
    BX_TAR_OPT_MODE_CREATE,
    BX_TAR_OPT_MODE_COMPARE,
    BX_TAR_OPT_MODE_TEST_LABEL,
    BX_TAR_OPT_MODE_LIST,
    BX_TAR_OPT_MODE_EXTRACT,
    BX_TAR_OPT_MODE_APPEND,
    BX_TAR_OPT_MODE_UPDATE,
    BX_TAR_OPT_MODE_DELETE,
    BX_TAR_OPT_MODE_UNSUPPORTED,
    BX_TAR_OPT_ARCHIVE_PATH,
    BX_TAR_OPT_DIRECTORY,
    BX_TAR_OPT_TO_STDOUT,
    BX_TAR_OPT_KEEP_OLD_FILES,
    BX_TAR_OPT_OVERWRITE,
    BX_TAR_OPT_UNLINK_FIRST,
    BX_TAR_OPT_RECURSIVE_UNLINK,
    BX_TAR_OPT_INDEX_FILE,
    BX_TAR_OPT_VERBOSE,
    BX_TAR_OPT_REPORT_MAPPED_NAMES,
    BX_TAR_OPT_BLOCK_NUMBER,
    BX_TAR_OPT_TOTALS,
    BX_TAR_OPT_EXCLUDE,
    BX_TAR_OPT_EXCLUDE_FROM,
    BX_TAR_OPT_ADD_FILE,
    BX_TAR_OPT_FILES_FROM,
    BX_TAR_OPT_FILES_FROM_NULL_ON,
    BX_TAR_OPT_FILES_FROM_NULL_OFF,
    BX_TAR_OPT_FILES_FROM_VERBATIM_ON,
    BX_TAR_OPT_FILES_FROM_VERBATIM_OFF,
    BX_TAR_OPT_UNQUOTE_ON,
    BX_TAR_OPT_UNQUOTE_OFF,
    BX_TAR_OPT_NO_RECURSION,
    BX_TAR_OPT_RECURSION,
    BX_TAR_OPT_ANCHORED_ON,
    BX_TAR_OPT_ANCHORED_OFF,
    BX_TAR_OPT_IGNORE_CASE_ON,
    BX_TAR_OPT_IGNORE_CASE_OFF,
    BX_TAR_OPT_WILDCARDS_ON,
    BX_TAR_OPT_WILDCARDS_OFF,
    BX_TAR_OPT_WILDCARDS_MATCH_SLASH_ON,
    BX_TAR_OPT_WILDCARDS_MATCH_SLASH_OFF,
    BX_TAR_OPT_EXCLUDE_CACHES,
    BX_TAR_OPT_EXCLUDE_CACHES_ALL,
    BX_TAR_OPT_EXCLUDE_CACHES_UNDER,
    BX_TAR_OPT_EXCLUDE_IGNORE,
    BX_TAR_OPT_EXCLUDE_IGNORE_RECURSIVE,
    BX_TAR_OPT_EXCLUDE_TAG,
    BX_TAR_OPT_EXCLUDE_TAG_ALL,
    BX_TAR_OPT_EXCLUDE_TAG_UNDER,
    BX_TAR_OPT_EXCLUDE_VCS,
    BX_TAR_OPT_EXCLUDE_VCS_IGNORES,
    BX_TAR_OPT_REMOVE_FILES,
    BX_TAR_OPT_THREADS,
    BX_TAR_OPT_COMPRESS_THREADS,
    BX_TAR_OPT_MT_CHUNK_SIZE,
    BX_TAR_OPT_NO_MT,
    BX_TAR_OPT_BZIP2_ON,
    BX_TAR_OPT_GZIP_ON,
    BX_TAR_OPT_XZ_ON,
    BX_TAR_OPT_ZSTD_ON,
    BX_TAR_OPT_EXTERNAL_COMPRESS_PROGRAM,
    BX_TAR_OPT_AUTO_COMPRESS_ON,
    BX_TAR_OPT_AUTO_COMPRESS_OFF,
    BX_TAR_OPT_ABSOLUTE_NAMES_ON,
    BX_TAR_OPT_TOUCH_MTIME_ON,
    BX_TAR_OPT_NUMERIC_OWNER,
    BX_TAR_OPT_NEWER,
    BX_TAR_OPT_NEWER_MTIME,
    BX_TAR_OPT_STARTING_FILE,
    BX_TAR_OPT_STRIP_COMPONENTS,
    BX_TAR_OPT_ONE_TOP_LEVEL,
    BX_TAR_OPT_TRANSFORM,
    BX_TAR_OPT_FORMAT,
    BX_TAR_OPT_SORT,
    BX_TAR_OPT_MTIME,
    BX_TAR_OPT_MODE,
    BX_TAR_OPT_OWNER,
    BX_TAR_OPT_GROUP,
    BX_TAR_OPT_GROUP_MAP,
    BX_TAR_OPT_OWNER_MAP,
    BX_TAR_OPT_XATTRS_ON,
    BX_TAR_OPT_XATTRS_OFF,
    BX_TAR_OPT_ACLS_ON,
    BX_TAR_OPT_ACLS_OFF,
    BX_TAR_OPT_WARNING,
};

struct bx_tar_long_option_spec {
    const char* name;
    enum bx_tar_option_arg_mode arg_mode;
    enum bx_tar_option_effect effect;
};

struct bx_tar_short_option_spec {
    char name;
    const char* display;
    enum bx_tar_option_arg_mode arg_mode;
    enum bx_tar_option_effect effect;
};

static const struct bx_tar_long_option_spec bx_tar_long_options[] = {
    {"--catenate", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_CATENATE},
    {"--concatenate", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_CATENATE},
    {"--create", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_CREATE},
    {"--delete", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_DELETE},
    {"--diff", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_COMPARE},
    {"--compare", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_COMPARE},
    {"--append", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_APPEND},
    {"--test-label", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_TEST_LABEL},
    {"--list", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_LIST},
    {"--update", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_UPDATE},
    {"--extract", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_EXTRACT},
    {"--get", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_EXTRACT},
    {"--check-device", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--listed-incremental", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--incremental", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--hole-detection", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--ignore-failed-read", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--level", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--no-check-device", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--no-seek", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--seek", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--occurrence", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--sparse-version", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--sparse", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--add-file", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_ADD_FILE},
    {"--directory", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_DIRECTORY},
    {"--exclude", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_EXCLUDE},
    {"--exclude-backups", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--exclude-caches", BX_TAR_OPTARG_NONE, BX_TAR_OPT_EXCLUDE_CACHES},
    {"--exclude-caches-all", BX_TAR_OPTARG_NONE, BX_TAR_OPT_EXCLUDE_CACHES_ALL},
    {"--exclude-caches-under", BX_TAR_OPTARG_NONE, BX_TAR_OPT_EXCLUDE_CACHES_UNDER},
    {"--exclude-ignore", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_EXCLUDE_IGNORE},
    {"--exclude-ignore-recursive", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_EXCLUDE_IGNORE_RECURSIVE},
    {"--exclude-tag", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_EXCLUDE_TAG},
    {"--exclude-tag-all", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_EXCLUDE_TAG_ALL},
    {"--exclude-tag-under", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_EXCLUDE_TAG_UNDER},
    {"--exclude-vcs", BX_TAR_OPTARG_NONE, BX_TAR_OPT_EXCLUDE_VCS},
    {"--exclude-vcs-ignores", BX_TAR_OPTARG_NONE, BX_TAR_OPT_EXCLUDE_VCS_IGNORES},
    {"--exclude-from", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_EXCLUDE_FROM},
    {"--no-null", BX_TAR_OPTARG_NONE, BX_TAR_OPT_FILES_FROM_NULL_OFF},
    {"--no-unquote", BX_TAR_OPTARG_NONE, BX_TAR_OPT_UNQUOTE_OFF},
    {"--no-verbatim-files-from", BX_TAR_OPTARG_NONE, BX_TAR_OPT_FILES_FROM_VERBATIM_OFF},
    {"--null", BX_TAR_OPTARG_NONE, BX_TAR_OPT_FILES_FROM_NULL_ON},
    {"--files-from", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_FILES_FROM},
    {"--unquote", BX_TAR_OPTARG_NONE, BX_TAR_OPT_UNQUOTE_ON},
    {"--verbatim-files-from", BX_TAR_OPTARG_NONE, BX_TAR_OPT_FILES_FROM_VERBATIM_ON},
    {"--no-recursion", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NO_RECURSION},
    {"--recursion", BX_TAR_OPTARG_NONE, BX_TAR_OPT_RECURSION},
    {"--anchored", BX_TAR_OPTARG_NONE, BX_TAR_OPT_ANCHORED_ON},
    {"--ignore-case", BX_TAR_OPTARG_NONE, BX_TAR_OPT_IGNORE_CASE_ON},
    {"--no-anchored", BX_TAR_OPTARG_NONE, BX_TAR_OPT_ANCHORED_OFF},
    {"--no-ignore-case", BX_TAR_OPTARG_NONE, BX_TAR_OPT_IGNORE_CASE_OFF},
    {"--no-wildcards", BX_TAR_OPTARG_NONE, BX_TAR_OPT_WILDCARDS_OFF},
    {"--no-wildcards-match-slash", BX_TAR_OPTARG_NONE, BX_TAR_OPT_WILDCARDS_MATCH_SLASH_OFF},
    {"--wildcards", BX_TAR_OPTARG_NONE, BX_TAR_OPT_WILDCARDS_ON},
    {"--wildcards-match-slash", BX_TAR_OPTARG_NONE, BX_TAR_OPT_WILDCARDS_MATCH_SLASH_ON},
    {"--keep-old-files", BX_TAR_OPTARG_NONE, BX_TAR_OPT_KEEP_OLD_FILES},
    {"--keep-directory-symlink", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--keep-newer-files", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--no-overwrite-dir", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--one-top-level", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_ONE_TOP_LEVEL},
    {"--overwrite", BX_TAR_OPTARG_NONE, BX_TAR_OPT_OVERWRITE},
    {"--overwrite-dir", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--recursive-unlink", BX_TAR_OPTARG_NONE, BX_TAR_OPT_RECURSIVE_UNLINK},
    {"--remove-files", BX_TAR_OPTARG_NONE, BX_TAR_OPT_REMOVE_FILES},
    {"--threads", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_THREADS},
    {"--compress-threads", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_COMPRESS_THREADS},
    {"--mt-chunk-size", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_MT_CHUNK_SIZE},
    {"--no-mt", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NO_MT},
    {"--skip-old-files", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--unlink-first", BX_TAR_OPTARG_NONE, BX_TAR_OPT_UNLINK_FIRST},
    {"--verify", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--ignore-command-error", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--no-ignore-command-error", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--to-stdout", BX_TAR_OPTARG_NONE, BX_TAR_OPT_TO_STDOUT},
    {"--to-command", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--atime-preserve", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--clamp-mtime", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--delay-directory-restore", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--no-delay-directory-restore", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--group", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_GROUP},
    {"--numeric-owner", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NUMERIC_OWNER},
    {"--owner", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_OWNER},
    {"--group-map", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_GROUP_MAP},
    {"--owner-map", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_OWNER_MAP},
    {"--mode", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_MODE},
    {"--mtime", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_MTIME},
    {"--touch", BX_TAR_OPTARG_NONE, BX_TAR_OPT_TOUCH_MTIME_ON},
    {"--no-same-owner", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--no-same-permissions", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--preserve-permissions", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--same-permissions", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--same-owner", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--set-mtime-command", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--set-mtime-format", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--sort", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_SORT},
    {"--preserve-order", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--same-order", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--acls", BX_TAR_OPTARG_NONE, BX_TAR_OPT_ACLS_ON},
    {"--no-acls", BX_TAR_OPTARG_NONE, BX_TAR_OPT_ACLS_OFF},
    {"--no-selinux", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--no-xattrs", BX_TAR_OPTARG_NONE, BX_TAR_OPT_XATTRS_OFF},
    {"--selinux", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--xattrs", BX_TAR_OPTARG_NONE, BX_TAR_OPT_XATTRS_ON},
    {"--xattrs-exclude", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--xattrs-include", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--force-local", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--file", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_ARCHIVE_PATH},
    {"--info-script", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--new-volume-script", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--tape-length", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--multi-volume", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--rmt-command", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--rsh-command", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--volno-file", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--blocking-factor", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--read-full-records", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--ignore-zeros", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--record-size", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--format", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_FORMAT},
    {"--old-archive", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--portability", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--posix", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--pax-option", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--label", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--auto-compress", BX_TAR_OPTARG_NONE, BX_TAR_OPT_AUTO_COMPRESS_ON},
    {"--use-compress-program", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_EXTERNAL_COMPRESS_PROGRAM},
    {"--bzip2", BX_TAR_OPTARG_NONE, BX_TAR_OPT_BZIP2_ON},
    {"--xz", BX_TAR_OPTARG_NONE, BX_TAR_OPT_XZ_ON},
    {"--lzip", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--lzma", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--lzop", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--zstd", BX_TAR_OPTARG_NONE, BX_TAR_OPT_ZSTD_ON},
    {"--compress", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--uncompress", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--gzip", BX_TAR_OPTARG_NONE, BX_TAR_OPT_GZIP_ON},
    {"--gunzip", BX_TAR_OPTARG_NONE, BX_TAR_OPT_GZIP_ON},
    {"--ungzip", BX_TAR_OPTARG_NONE, BX_TAR_OPT_GZIP_ON},
    {"--no-auto-compress", BX_TAR_OPTARG_NONE, BX_TAR_OPT_AUTO_COMPRESS_OFF},
    {"--backup", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--suffix", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--hard-dereference", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--dereference", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--starting-file", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_STARTING_FILE},
    {"--newer-mtime", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NEWER_MTIME},
    {"--newer", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NEWER},
    {"--after-date", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NEWER},
    {"--one-file-system", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--absolute-names", BX_TAR_OPTARG_NONE, BX_TAR_OPT_ABSOLUTE_NAMES_ON},
    {"--strip-components", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_STRIP_COMPONENTS},
    {"--transform", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_TRANSFORM},
    {"--xform", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_TRANSFORM},
    {"--checkpoint", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--checkpoint-action", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--full-time", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--index-file", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_INDEX_FILE},
    {"--check-links", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--no-quote-chars", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--quote-chars", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--quoting-style", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--block-number", BX_TAR_OPTARG_NONE, BX_TAR_OPT_BLOCK_NUMBER},
    {"--show-defaults", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--show-omitted-dirs", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--show-snapshot-field-ranges", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--show-transformed-names", BX_TAR_OPTARG_NONE, BX_TAR_OPT_REPORT_MAPPED_NAMES},
    {"--show-stored-names", BX_TAR_OPTARG_NONE, BX_TAR_OPT_REPORT_MAPPED_NAMES},
    {"--totals", BX_TAR_OPTARG_NONE, BX_TAR_OPT_TOTALS},
    {"--utc", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--verbose", BX_TAR_OPTARG_NONE, BX_TAR_OPT_VERBOSE},
    {"--warning", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_WARNING},
    {"--interactive", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--confirmation", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--restrict", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {NULL, BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
};

static const struct bx_tar_short_option_spec bx_tar_short_options[] = {
    {'A', "-A", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_CATENATE},
    {'c', "-c", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_CREATE},
    {'d', "-d", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_COMPARE},
    {'r', "-r", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_APPEND},
    {'t', "-t", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_LIST},
    {'u', "-u", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_UPDATE},
    {'x', "-x", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_EXTRACT},
    {'g', "-g", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {'G', "-G", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {'n', "-n", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {'S', "-S", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {'C', "-C", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_DIRECTORY},
    {'X', "-X", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_EXCLUDE_FROM},
    {'T', "-T", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_FILES_FROM},
    {'k', "-k", BX_TAR_OPTARG_NONE, BX_TAR_OPT_KEEP_OLD_FILES},
    {'U', "-U", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {'W', "-W", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {'O', "-O", BX_TAR_OPTARG_NONE, BX_TAR_OPT_TO_STDOUT},
    {'m', "-m", BX_TAR_OPTARG_NONE, BX_TAR_OPT_TOUCH_MTIME_ON},
    {'p', "-p", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {'F', "-F", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {'L', "-L", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {'M', "-M", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {'b', "-b", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {'B', "-B", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {'i', "-i", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {'H', "-H", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_FORMAT},
    {'V', "-V", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {'a', "-a", BX_TAR_OPTARG_NONE, BX_TAR_OPT_AUTO_COMPRESS_ON},
    {'I', "-I", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_EXTERNAL_COMPRESS_PROGRAM},
    {'j', "-j", BX_TAR_OPTARG_NONE, BX_TAR_OPT_BZIP2_ON},
    {'J', "-J", BX_TAR_OPTARG_NONE, BX_TAR_OPT_XZ_ON},
    {'Z', "-Z", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {'z', "-z", BX_TAR_OPTARG_NONE, BX_TAR_OPT_GZIP_ON},
    {'h', "-h", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {'K', "-K", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_STARTING_FILE},
    {'N', "-N", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NEWER},
    {'l', "-l", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {'P', "-P", BX_TAR_OPTARG_NONE, BX_TAR_OPT_ABSOLUTE_NAMES_ON},
    {'s', "-s", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {'R', "-R", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {'v', "-v", BX_TAR_OPTARG_NONE, BX_TAR_OPT_VERBOSE},
    {'w', "-w", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {'o', "-o", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {'f', "-f", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_ARCHIVE_PATH},
    {'?', "-?", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {'\0', NULL, BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
};

static const char* bx_tar_progname(char** argv, int argc) {
    return bx_cli_progname((argc > 0) ? argv[0] : NULL, "tar");
}

static void bx_tar_report_previous_errors(const struct bx_diag_ctx* diag) {
    fprintf(stderr, "%s: Exiting with failure status due to previous errors\n", diag->progname);
}

static void bx_tar_release_mapped_name(struct bx_tar_mapped_name* name) {
    free(name->owned);
    name->owned = NULL;
    name->text = NULL;
}

static struct bx_tar_stream_options
bx_tar_make_stream_options(const struct bx_tar_options* options) {
    return (struct bx_tar_stream_options){
        .format_ustar = options->format_ustar,
        .numeric_owner = options->numeric_owner,
        .owner_set = options->owner_set,
        .group_set = options->group_set,
        .fixed_mtime = options->fixed_mtime,
        .mode_text = options->mode_text,
        .owner = options->owner,
        .group = options->group,
        .mtime = options->mtime,
        .owner_map = &options->owner_map,
        .group_map = &options->group_map,
    };
}

static bool bx_tar_validate_mode_arg(const char* text, struct bx_diag_ctx* diag) {
    struct bx_mode_parse_params params = {
        .initial_mode = 07777u,
        .result_mask = 07777u,
        .max_numeric_mode = 07777u,
        .umask_value = 0u,
        .sticky_bit = BX_TAR_STICKY_BIT,
        .x_policy = BX_MODE_X_IF_DIRECTORY_OR_ANY_EXEC,
        .is_directory = true,
        .apply_umask_when_who_omitted = true,
        .allow_setuid = true,
        .allow_setgid = true,
        .allow_sticky = true,
    };
    mode_t parsed = 0u;

    if (bx_mode_parse(text, &params, &parsed)) {
        return true;
    }
    bx_diag(diag, "invalid mode '%s'", text);
    return false;
}

static void bx_tar_clear_unsupported_external_compress_program(struct bx_tar_options* options) {
    free(options->unsupported_external_compress_program);
    options->unsupported_external_compress_program = NULL;
    options->unsupported_external_compress_option = NULL;
}

static void bx_tar_set_codec_option(struct bx_tar_options* options,
                                    const struct bx_archive_codec* codec) {
    bx_tar_clear_unsupported_external_compress_program(options);
    options->codec = codec;
}

static bool bx_tar_set_unsupported_external_compress_program(struct bx_tar_options* options,
                                                             const char* display,
                                                             const char* value) {
    bx_tar_clear_unsupported_external_compress_program(options);
    options->unsupported_external_compress_program = xstrdup(value);
    options->unsupported_external_compress_option = display;
    return true;
}

static const char* bx_tar_program_basename(const char* path) {
    const char* slash;

    if (path == NULL) {
        return NULL;
    }
    slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

static bool bx_tar_internal_xz_threads_arg_supported(const char* text) {
    char* end = NULL;
    long value;

    if (text == NULL || *text == '\0') {
        return false;
    }
    errno = 0;
    value = strtol(text, &end, 10);
    return errno == 0
        && end != NULL
        && *end == '\0'
        && value >= 0
        && value <= INT_MAX;
}

static bool bx_tar_internal_xz_short_token_supported(const char* token, char** saveptr) {
    size_t i;

    for (i = 1u; token[i] != '\0'; i++) {
        switch (token[i]) {
            case 'd':
            case 'c':
            case 'z':
            case 'k':
                break;
            case 'T': {
                const char* threads = token[i + 1u] != '\0'
                    ? token + i + 1u
                    : strtok_r(NULL, " \t\r\n", saveptr);

                return bx_tar_internal_xz_threads_arg_supported(threads);
            }
            default:
                return false;
        }
    }

    return true;
}

static bool bx_tar_internal_xz_token_supported(const char* token, char** saveptr) {
    if (strcmp(token, "--decompress") == 0 || strcmp(token, "--uncompress") == 0) {
        return true;
    }
    if (strcmp(token, "--stdout") == 0 || strcmp(token, "--compress") == 0) {
        return true;
    }
    if (strcmp(token, "--keep") == 0) {
        return true;
    }
    if (strcmp(token, "--threads") == 0) {
        return bx_tar_internal_xz_threads_arg_supported(strtok_r(NULL, " \t\r\n", saveptr));
    }
    if (strncmp(token, "--threads=", 10u) == 0) {
        return bx_tar_internal_xz_threads_arg_supported(token + 10u);
    }
    if (token[0] == '-' && token[1] != '\0' && token[1] != '-') {
        return bx_tar_internal_xz_short_token_supported(token, saveptr);
    }
    return false;
}

static bool bx_tar_try_set_internal_xz_compress_program(struct bx_tar_options* options,
                                                        const char* value,
                                                        bool* recognized_out) {
    char* copy;
    char* saveptr = NULL;
    char* token;
    const char* program;

    *recognized_out = false;
    if (value == NULL) {
        return true;
    }

    copy = xstrdup(value);
    token = strtok_r(copy, " \t\r\n", &saveptr);
    if (token == NULL) {
        free(copy);
        return true;
    }

    program = bx_tar_program_basename(token);
    if (strcmp(program, "xz") != 0
        && strcmp(program, "unxz") != 0
        && strcmp(program, "xzcat") != 0) {
        free(copy);
        return true;
    }

    while ((token = strtok_r(NULL, " \t\r\n", &saveptr)) != NULL) {
        if (!bx_tar_internal_xz_token_supported(token, &saveptr)) {
            free(copy);
            return true;
        }
    }

    bx_tar_set_codec_option(options, bx_archive_codec_xz());
    *recognized_out = true;
    free(copy);
    return true;
}

static bool bx_tar_apply_external_compress_program(struct bx_tar_options* options,
                                                   const char* display,
                                                   const char* value) {
    bool recognized = false;

    if (!bx_tar_try_set_internal_xz_compress_program(options, value, &recognized)) {
        return false;
    }
    if (recognized) {
        return true;
    }
    return bx_tar_set_unsupported_external_compress_program(options, display, value);
}

static const struct bx_archive_codec* bx_tar_codec_from_suffix(const char* path) {
    if (bx_archive_codec_matches_path_suffix(bx_archive_codec_gzip(), path)) {
        return bx_archive_codec_gzip();
    }
    if (bx_archive_codec_matches_path_suffix(bx_archive_codec_bzip2(), path)) {
        return bx_archive_codec_bzip2();
    }
    if (bx_archive_codec_matches_path_suffix(bx_archive_codec_xz(), path)) {
        return bx_archive_codec_xz();
    }
    if (bx_archive_codec_matches_path_suffix(bx_archive_codec_zstd(), path)) {
        return bx_archive_codec_zstd();
    }
    return NULL;
}

static const struct bx_archive_codec* bx_tar_output_codec(const struct bx_tar_options* options) {
    if (options->codec != NULL) {
        return options->codec;
    }
    if (options->auto_compress && options->archive_path != NULL) {
        const struct bx_archive_codec* codec = bx_tar_codec_from_suffix(options->archive_path);

        if (codec != NULL) {
            return codec;
        }
    }
    return bx_archive_codec_none();
}

static const struct bx_archive_codec* bx_tar_input_required_codec(const struct bx_tar_options* options) {
    if (options->codec != NULL) {
        return options->codec;
    }
    if (options->auto_compress && options->archive_path != NULL) {
        return bx_tar_codec_from_suffix(options->archive_path);
    }
    return NULL;
}

static bool bx_tar_output_is_compressed(const struct bx_tar_options* options) {
    return bx_tar_output_codec(options) != bx_archive_codec_none();
}

static size_t bx_tar_effective_compress_threads(const struct bx_tar_options* options) {
    if (options->no_mt) {
        return 1u;
    }
    if (options->compress_threads >= 0) {
        return bx_thread_count_resolve(options->compress_threads);
    }
    if (options->threads >= 0) {
        return bx_thread_count_resolve(options->threads);
    }
    return 1u;
}

static uint64_t bx_tar_total_archive_size_from_body(size_t body_bytes) {
    size_t with_trailer = body_bytes + 2u * BX_TAR_BLOCK_SIZE;
    size_t record_size = BX_TAR_BLOCK_SIZE * 20u;
    size_t rem = with_trailer % record_size;

    if (rem == 0u) {
        return with_trailer;
    }
    return with_trailer + (record_size - rem);
}

static bool bx_tar_file_sink_write(void* user, const void* data, size_t len) {
    FILE* stream = user;
    return fwrite(data, 1u, len, stream) == len;
}

struct bx_tar_codec_stream_create_ctx {
    const struct bx_archive_fs_list* files;
    bx_tar_stream_fs_entry_producer_fn producer;
    void* producer_user;
    const struct bx_tar_options* options;
    uint64_t total_bytes_written;
};

struct bx_tar_codec_stream_sink_adapter {
    const struct bx_archive_codec_stream_sink* sink;
};

static bool bx_tar_codec_stream_sink_write(void* user, const void* data, size_t len) {
    const struct bx_tar_codec_stream_sink_adapter* adapter = user;
    return adapter->sink->write(adapter->sink->user, data, len);
}

static bool bx_tar_write_create_stream_body(const struct bx_archive_fs_list* files,
                                            bx_tar_stream_fs_entry_producer_fn producer,
                                            void* producer_user,
                                            const struct bx_tar_stream_options* stream_options,
                                            const struct bx_tar_stream_sink* sink,
                                            uint64_t* total_bytes_written_out,
                                            struct bx_diag_ctx* diag) {
    size_t bytes_written = 0u;
    bool ok = files != NULL
        ? bx_tar_stream_write_fs_list_body(files, stream_options, sink, &bytes_written, diag)
        : bx_tar_stream_write_fs_entries_body(producer,
                                              producer_user,
                                              stream_options,
                                              sink,
                                              &bytes_written,
                                              diag);

    *total_bytes_written_out = bytes_written;
    if (!ok) {
        return false;
    }
    if (!bx_tar_stream_write_trailer(sink, bytes_written, diag)) {
        return false;
    }
    *total_bytes_written_out = bx_tar_total_archive_size_from_body(bytes_written);
    return true;
}

static bool bx_tar_codec_stream_produce(void* user,
                                        const struct bx_archive_codec_stream_sink* sink,
                                        struct bx_diag_ctx* diag) {
    struct bx_tar_codec_stream_create_ctx* ctx = user;
    struct bx_tar_codec_stream_sink_adapter adapter = {
        .sink = sink,
    };
    struct bx_tar_stream_options stream_options = bx_tar_make_stream_options(ctx->options);
    struct bx_tar_stream_sink tar_sink = {
        .user = &adapter,
        .write = bx_tar_codec_stream_sink_write,
        .callback_owns_errors = true,
    };
    uint64_t total_bytes_written = 0u;

    ctx->total_bytes_written = 0u;
    if (!bx_tar_write_create_stream_body(ctx->files,
                                         ctx->producer,
                                         ctx->producer_user,
                                         &stream_options,
                                         &tar_sink,
                                         &total_bytes_written,
                                         diag)) {
        ctx->total_bytes_written = total_bytes_written;
        return false;
    }
    ctx->total_bytes_written = total_bytes_written;
    return true;
}

static bool bx_tar_write_create_archive_direct(const struct bx_archive_fs_list* files,
                                               const struct bx_tar_options* options,
                                               uint64_t* total_bytes_written_out,
                                               struct bx_diag_ctx* diag) {
    struct bx_tar_codec_stream_create_ctx create_ctx = {
        .files = files,
        .options = options,
    };
    const struct bx_archive_codec* codec = bx_tar_output_codec(options);
    struct bx_archive_codec_stream_sink sink = {
        .user = NULL,
        .write = bx_tar_file_sink_write,
    };
    struct bx_tar_stream_options stream_options = bx_tar_make_stream_options(options);
    struct bx_archive_output_file output = {0};
    bool ok;

    *total_bytes_written_out = 0u;

    if (!bx_archive_output_file_open(&output, options->archive_path, diag)) {
        return false;
    }
    sink.user = output.stream;
    create_ctx.total_bytes_written = 0u;
    if (codec == bx_archive_codec_none()) {
        struct bx_tar_stream_sink tar_sink = {
            .user = output.stream,
            .write = bx_tar_file_sink_write,
        };

        ok = bx_tar_write_create_stream_body(files,
                                             NULL,
                                             NULL,
                                             &stream_options,
                                             &tar_sink,
                                             total_bytes_written_out,
                                             diag);
    }
    else {
        ok = bx_archive_codec_run_encode_stream(codec,
                                                bx_tar_codec_stream_produce,
                                                &create_ctx,
                                                &sink,
                                                diag);
        *total_bytes_written_out = create_ctx.total_bytes_written;
    }
    if (ok && !bx_archive_output_file_finish(&output, diag)) {
        ok = false;
    }
    if (!ok) {
        bx_archive_output_file_discard(&output);
    }
    return ok;
}

static bool bx_tar_write_create_archive_mt_direct(const struct bx_archive_fs_list* files,
                                                  const struct bx_tar_options* options,
                                                  size_t compress_threads,
                                                  uint64_t* total_bytes_written_out,
                                                  struct bx_diag_ctx* diag) {
    struct bx_tar_codec_stream_create_ctx create_ctx = {
        .files = files,
        .options = options,
    };
    struct bx_archive_codec_stream_sink sink = {
        .user = NULL,
        .write = bx_tar_file_sink_write,
    };
    struct bx_archive_output_file output = {0};
    const struct bx_archive_codec* codec = bx_tar_output_codec(options);
    size_t chunk_size = options->mt_chunk_size != 0u ? (size_t)options->mt_chunk_size : (1u << 20);
    size_t max_inflight = compress_threads > (SIZE_MAX / 4u) ? compress_threads : compress_threads * 4u;
    bool ok;

    *total_bytes_written_out = 0u;

    if (!bx_archive_output_file_open(&output, options->archive_path, diag)) {
        return false;
    }
    sink.user = output.stream;
    create_ctx.total_bytes_written = 0u;
    ok = bx_archive_codec_run_encode_mt_stream(codec,
                                               bx_tar_codec_stream_produce,
                                               &create_ctx,
                                               &sink,
                                               &(struct bx_archive_codec_mt_options){
                                                   .thread_count = compress_threads,
                                                   .chunk_size = chunk_size,
                                                   .max_inflight_chunks = max_inflight,
                                               },
                                               diag);
    *total_bytes_written_out = create_ctx.total_bytes_written;
    if (ok && !bx_archive_output_file_finish(&output, diag)) {
        ok = false;
    }
    if (!ok) {
        bx_archive_output_file_discard(&output);
    }
    return ok;
}

static bool bx_tar_write_create_archive_stream_direct(bx_tar_stream_fs_entry_producer_fn producer,
                                                      void* producer_user,
                                                      const struct bx_tar_options* options,
                                                      uint64_t* total_bytes_written_out,
                                                      struct bx_diag_ctx* diag) {
    struct bx_tar_codec_stream_create_ctx create_ctx = {
        .files = NULL,
        .producer = producer,
        .producer_user = producer_user,
        .options = options,
    };
    const struct bx_archive_codec* codec = bx_tar_output_codec(options);
    struct bx_archive_codec_stream_sink sink = {
        .user = NULL,
        .write = bx_tar_file_sink_write,
    };
    struct bx_tar_stream_options stream_options = bx_tar_make_stream_options(options);
    struct bx_archive_output_file output = {0};
    bool ok;

    *total_bytes_written_out = 0u;

    if (!bx_archive_output_file_open(&output, options->archive_path, diag)) {
        return false;
    }
    sink.user = output.stream;
    create_ctx.total_bytes_written = 0u;
    if (codec == bx_archive_codec_none()) {
        struct bx_tar_stream_sink tar_sink = {
            .user = output.stream,
            .write = bx_tar_file_sink_write,
        };

        ok = bx_tar_write_create_stream_body(NULL,
                                             producer,
                                             producer_user,
                                             &stream_options,
                                             &tar_sink,
                                             total_bytes_written_out,
                                             diag);
    }
    else {
        ok = bx_archive_codec_run_encode_stream(codec,
                                                bx_tar_codec_stream_produce,
                                                &create_ctx,
                                                &sink,
                                                diag);
        *total_bytes_written_out = create_ctx.total_bytes_written;
    }
    if (ok && !bx_archive_output_file_finish(&output, diag)) {
        ok = false;
    }
    if (!ok) {
        bx_archive_output_file_discard(&output);
    }
    return ok;
}

static bool bx_tar_write_create_archive_stream_mt_direct(bx_tar_stream_fs_entry_producer_fn producer,
                                                         void* producer_user,
                                                         const struct bx_tar_options* options,
                                                         size_t compress_threads,
                                                         uint64_t* total_bytes_written_out,
                                                         struct bx_diag_ctx* diag) {
    struct bx_tar_codec_stream_create_ctx create_ctx = {
        .files = NULL,
        .producer = producer,
        .producer_user = producer_user,
        .options = options,
    };
    struct bx_archive_codec_stream_sink sink = {
        .user = NULL,
        .write = bx_tar_file_sink_write,
    };
    struct bx_archive_output_file output = {0};
    const struct bx_archive_codec* codec = bx_tar_output_codec(options);
    size_t chunk_size = options->mt_chunk_size != 0u ? (size_t)options->mt_chunk_size : (1u << 20);
    size_t max_inflight = compress_threads > (SIZE_MAX / 4u) ? compress_threads : compress_threads * 4u;
    bool ok;

    *total_bytes_written_out = 0u;

    if (!bx_archive_output_file_open(&output, options->archive_path, diag)) {
        return false;
    }
    sink.user = output.stream;
    create_ctx.total_bytes_written = 0u;
    ok = bx_archive_codec_run_encode_mt_stream(codec,
                                               bx_tar_codec_stream_produce,
                                               &create_ctx,
                                               &sink,
                                               &(struct bx_archive_codec_mt_options){
                                                   .thread_count = compress_threads,
                                                   .chunk_size = chunk_size,
                                                   .max_inflight_chunks = max_inflight,
                                               },
                                               diag);
    *total_bytes_written_out = create_ctx.total_bytes_written;
    if (ok && !bx_archive_output_file_finish(&output, diag)) {
        ok = false;
    }
    if (!ok) {
        bx_archive_output_file_discard(&output);
    }
    return ok;
}

struct bx_tar_extract_state {
    const struct bx_tar_options* options;
    FILE* report_stream;
    const struct bx_tar_select_plan* select_plan;
    struct bx_archive_pending_dirs dirs;
    struct bx_archive_parent_dir_cache parent_dir_cache;
    struct bx_tar_name_policy name_policy;
    bool warned_absolute;
    bool warned_dotdot;
    bool starting_file_reached;
    bool* matched_members;
    int status;
    int current_fd;
    char* current_dest_path;
    mode_t current_mode_bits;
    struct timespec current_mtime;
    uid_t current_owner;
    gid_t current_group;
    bool current_owner_mapped;
    bool current_group_mapped;
    bool current_sparse;
    size_t current_sparse_extent_index;
    size_t current_sparse_extent_offset;
    size_t current_sparse_logical_offset;
    uint64_t total_bytes_read;
    enum {
        BX_TAR_EXTRACT_STREAM_NONE = 0,
        BX_TAR_EXTRACT_STREAM_STDOUT,
        BX_TAR_EXTRACT_STREAM_FILE,
    } current_stream_mode;
};

struct bx_tar_list_state {
    const struct bx_tar_options* options;
    FILE* report_stream;
    bool starting_file_reached;
    const struct bx_tar_select_plan* select_plan;
    struct bx_tar_name_policy name_policy;
    bool warned_absolute;
    bool warned_dotdot;
    bool* matched_members;
    uint64_t total_bytes_read;
};

struct bx_tar_compare_state {
    const struct bx_tar_options* options;
    FILE* report_stream;
    const struct bx_tar_select_plan* select_plan;
    struct bx_tar_name_policy name_policy;
    bool warned_absolute;
    bool warned_dotdot;
    bool* matched_members;
    int status;
    int current_fd;
    char* current_fs_path;
    bool current_skip;
    bool current_compare_contents;
    bool current_sparse;
    bool current_reported_content_diff;
    size_t current_sparse_extent_index;
    size_t current_sparse_extent_offset;
    size_t current_sparse_logical_offset;
    uint64_t total_bytes_read;
};

static bool* bx_tar_alloc_matched_members(const struct bx_tar_select_plan* select_plan) {
    bool* matched_members;

    if (select_plan->len == 0u) {
        return NULL;
    }
    matched_members = xmalloc(select_plan->len * sizeof(*matched_members));
    memset(matched_members, 0, select_plan->len * sizeof(*matched_members));
    return matched_members;
}

static int bx_tar_timespec_compare(struct timespec left, struct timespec right);

static void bx_tar_extract_state_init(struct bx_tar_extract_state* state,
                                      const struct bx_tar_options* options,
                                      const struct bx_tar_select_plan* select_plan,
                                      FILE* report_stream) {
    memset(state, 0, sizeof(*state));
    state->options = options;
    state->report_stream = report_stream;
    state->select_plan = select_plan;
    state->starting_file_reached = options->starting_file == NULL;
    state->matched_members = bx_tar_alloc_matched_members(select_plan);
    state->name_policy = (struct bx_tar_name_policy){
        .absolute_names = options->absolute_names,
        .strip_components = options->strip_components,
        .one_top_level = options->one_top_level,
        .transform = options->name_transform.active ? &options->name_transform : NULL,
    };
    state->current_fd = -1;
}

static void bx_tar_extract_state_cleanup(struct bx_tar_extract_state* state) {
    if (state->current_fd >= 0) {
        close(state->current_fd);
        state->current_fd = -1;
    }
    free(state->current_dest_path);
    state->current_dest_path = NULL;
    free(state->matched_members);
    bx_archive_parent_dir_cache_cleanup(&state->parent_dir_cache);
    bx_archive_pending_dirs_free(&state->dirs);
}

static void bx_tar_list_state_init(struct bx_tar_list_state* state,
                                   const struct bx_tar_options* options,
                                   const struct bx_tar_select_plan* select_plan,
                                   FILE* report_stream) {
    memset(state, 0, sizeof(*state));
    state->options = options;
    state->report_stream = report_stream;
    state->starting_file_reached = options->starting_file == NULL;
    state->select_plan = select_plan;
    state->name_policy = (struct bx_tar_name_policy){
        .absolute_names = options->absolute_names,
        .strip_components = options->strip_components,
        .one_top_level = options->one_top_level,
        .transform = options->name_transform.active ? &options->name_transform : NULL,
    };
    state->matched_members = bx_tar_alloc_matched_members(select_plan);
}

static void bx_tar_list_state_cleanup(struct bx_tar_list_state* state) {
    free(state->matched_members);
}

static void bx_tar_compare_state_init(struct bx_tar_compare_state* state,
                                      const struct bx_tar_options* options,
                                      const struct bx_tar_select_plan* select_plan,
                                      FILE* report_stream) {
    memset(state, 0, sizeof(*state));
    state->options = options;
    state->report_stream = report_stream;
    state->select_plan = select_plan;
    state->matched_members = bx_tar_alloc_matched_members(select_plan);
    state->current_fd = -1;
    state->name_policy = (struct bx_tar_name_policy){
        .absolute_names = options->absolute_names,
        .strip_components = options->strip_components,
        .one_top_level = options->one_top_level,
        .transform = options->name_transform.active ? &options->name_transform : NULL,
    };
}

static void bx_tar_compare_state_cleanup(struct bx_tar_compare_state* state) {
    if (state->current_fd >= 0) {
        close(state->current_fd);
        state->current_fd = -1;
    }
    free(state->current_fs_path);
    state->current_fs_path = NULL;
    free(state->matched_members);
}

static bool bx_tar_starting_file_gate_reached(bool* reached_io,
                                              const char* starting_file,
                                              const struct bx_tar_entry* entry) {
    if (*reached_io) {
        return true;
    }
    if (starting_file != NULL && strcmp(entry->name, starting_file) == 0) {
        *reached_io = true;
        return true;
    }
    return false;
}

static void bx_tar_warn_name_adjustments(const struct bx_diag_ctx* diag,
                                         bool stripped_absolute,
                                         bool* warned_absolute,
                                         bool stripped_dotdot,
                                         bool* warned_dotdot) {
    if (stripped_absolute && !*warned_absolute) {
        fprintf(stderr, "%s: Removing leading '/' from member names\n", diag->progname);
        *warned_absolute = true;
    }
    if (stripped_dotdot && !*warned_dotdot) {
        fprintf(stderr, "%s: Removing leading '../' from member names\n", diag->progname);
        *warned_dotdot = true;
    }
}

static void bx_tar_report_empty_name(const struct bx_tar_entry* entry,
                                     const struct bx_diag_ctx* diag) {
    fprintf(stderr, "%s: %s: transforms to empty name\n", diag->progname, entry->name);
}

static bool bx_tar_report_totals_line(bool writing,
                                      uint64_t total_bytes,
                                      struct bx_diag_ctx* diag) {
    if (fprintf(stderr,
                "Total bytes %s: %" PRIu64 "\n",
                writing ? "written" : "read",
                total_bytes) < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
    return true;
}

static bool bx_tar_report_archive_end_if_requested(const struct bx_tar_options* options,
                                                   FILE* report_stream,
                                                   uint64_t block_index,
                                                   enum bx_tar_stream_end_kind end_kind,
                                                   struct bx_diag_ctx* diag) {
    if (!options->report_block_numbers || report_stream == NULL) {
        return true;
    }
    return bx_tar_report_archive_end(report_stream,
                                     block_index,
                                     end_kind == BX_TAR_STREAM_END_ZERO_BLOCKS,
                                     diag);
}

static uint64_t bx_tar_extract_report_block_index(const struct bx_tar_entry* entry) {
    return entry->header_block_index + 1u;
}

static uint64_t bx_tar_reported_total_bytes_read(uint64_t block_index,
                                                 enum bx_tar_stream_end_kind end_kind,
                                                 uint64_t total_bytes_read) {
    if (end_kind == BX_TAR_STREAM_END_ZERO_BLOCKS && block_index <= SIZE_MAX / BX_TAR_BLOCK_SIZE) {
        return bx_tar_total_archive_size_from_body((size_t)block_index * BX_TAR_BLOCK_SIZE);
    }
    return total_bytes_read;
}

static void bx_tar_extract_invalidate_parent_cache(struct bx_tar_extract_state* state) {
    bx_archive_parent_dir_cache_invalidate(&state->parent_dir_cache);
}

static void bx_tar_extract_clear_current_stream(struct bx_tar_extract_state* state) {
    if (state->current_fd >= 0) {
        close(state->current_fd);
        state->current_fd = -1;
    }
    state->current_stream_mode = BX_TAR_EXTRACT_STREAM_NONE;
    state->current_mode_bits = 0u;
    state->current_mtime.tv_sec = 0;
    state->current_mtime.tv_nsec = 0;
    state->current_owner = 0;
    state->current_group = 0;
    state->current_owner_mapped = false;
    state->current_group_mapped = false;
    state->current_sparse = false;
    state->current_sparse_extent_index = 0u;
    state->current_sparse_extent_offset = 0u;
    state->current_sparse_logical_offset = 0u;
    free(state->current_dest_path);
    state->current_dest_path = NULL;
}

static bool bx_tar_extract_map_entry_ids(const struct bx_tar_extract_state* state,
                                         const struct bx_tar_entry* entry,
                                         uid_t* owner_out,
                                         gid_t* group_out,
                                         bool* owner_mapped_out,
                                         bool* group_mapped_out) {
    *owner_out = entry->uid;
    *group_out = entry->gid;
    *owner_mapped_out = false;
    *group_mapped_out = false;

    if (state->options->owner_map.len > 0u
        && bx_tar_id_map_apply_owner(&state->options->owner_map,
                                     entry->uid,
                                     entry->uname,
                                     owner_out,
                                     NULL)) {
        *owner_mapped_out = true;
    }
    if (state->options->group_map.len > 0u
        && bx_tar_id_map_apply_group(&state->options->group_map,
                                     entry->gid,
                                     entry->gname,
                                     group_out,
                                     NULL)) {
        *group_mapped_out = true;
    }

    return true;
}

static bool bx_tar_extract_apply_path_ownership(const char* path,
                                                bool nofollow,
                                                uid_t owner,
                                                gid_t group,
                                                bool owner_mapped,
                                                bool group_mapped,
                                                struct bx_diag_ctx* diag) {
    int rc;

    if (!owner_mapped && !group_mapped) {
        return true;
    }

    rc = nofollow
        ? lchown(path, owner_mapped ? owner : (uid_t)-1, group_mapped ? group : (gid_t)-1)
        : chown(path, owner_mapped ? owner : (uid_t)-1, group_mapped ? group : (gid_t)-1);
    if (rc != 0) {
        bx_diag(diag, "%s: %s", path, strerror(errno));
        return false;
    }
    return true;
}

static bool bx_tar_extract_prepare_parent_dirs_safe(struct bx_tar_extract_state* state,
                                                    const char* dest_path,
                                                    struct bx_diag_ctx* diag) {
    return bx_archive_ensure_parent_dirs_safe_cached(dest_path, &state->parent_dir_cache, diag);
}

static bool bx_tar_extract_remove_empty_dir_default(const char* path,
                                                    struct bx_diag_ctx* diag) {
    if (rmdir(path) == 0) {
        return true;
    }
    if (errno == ENOTEMPTY || errno == EEXIST) {
        bx_diag(diag, "%s: %s", path, strerror(EEXIST));
        return false;
    }
    bx_diag(diag, "%s: %s", path, strerror(errno));
    return false;
}

static bool bx_tar_extract_prepare_final_non_dir_target(struct bx_tar_extract_state* state,
                                                        const struct bx_tar_entry* entry,
                                                        const char* dest_path,
                                                        struct bx_diag_ctx* diag) {
    struct stat st;

    if (lstat(dest_path, &st) != 0) {
        if (errno == ENOENT) {
            return true;
        }
        bx_diag(diag, "%s: %s", dest_path, strerror(errno));
        return false;
    }

    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        bx_tar_extract_invalidate_parent_cache(state);
        if (unlink(dest_path) != 0) {
            bx_diag(diag, "%s: %s", dest_path, strerror(errno));
            return false;
        }
        return true;
    }

    if (state->options->recursive_unlink) {
        bx_tar_extract_invalidate_parent_cache(state);
        return bx_archive_remove_path_tree(dest_path, diag);
    }
    if (state->options->overwrite) {
        bx_diag(diag, "%s: %s", dest_path, strerror(EISDIR));
        return false;
    }
    if (state->options->unlink_first) {
        bx_tar_extract_invalidate_parent_cache(state);
        if (rmdir(dest_path) != 0) {
            bx_diag(diag, "%s: %s", dest_path, strerror(errno));
            return false;
        }
        return true;
    }

    (void)entry;
    bx_tar_extract_invalidate_parent_cache(state);
    return bx_tar_extract_remove_empty_dir_default(dest_path, diag);
}

static bool bx_tar_extract_prepare_final_dir_target(struct bx_tar_extract_state* state,
                                                    const char* dest_path,
                                                    bool* mkdir_needed_out,
                                                    struct bx_diag_ctx* diag) {
    struct stat st;

    *mkdir_needed_out = false;
    if (lstat(dest_path, &st) != 0) {
        if (errno == ENOENT) {
            *mkdir_needed_out = true;
            return true;
        }
        bx_diag(diag, "%s: %s", dest_path, strerror(errno));
        return false;
    }

    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        bx_tar_extract_invalidate_parent_cache(state);
        if (!bx_archive_remove_path_tree(dest_path, diag)) {
            return false;
        }
        *mkdir_needed_out = true;
        return true;
    }

    if (state->options->recursive_unlink) {
        bx_tar_extract_invalidate_parent_cache(state);
        if (!bx_archive_remove_path_tree(dest_path, diag)) {
            return false;
        }
        *mkdir_needed_out = true;
    }
    return true;
}

static void bx_tar_compare_clear_current_stream(struct bx_tar_compare_state* state) {
    if (state->current_fd >= 0) {
        close(state->current_fd);
        state->current_fd = -1;
    }
    free(state->current_fs_path);
    state->current_fs_path = NULL;
    state->current_skip = false;
    state->current_compare_contents = false;
    state->current_sparse = false;
    state->current_reported_content_diff = false;
    state->current_sparse_extent_index = 0u;
    state->current_sparse_extent_offset = 0u;
    state->current_sparse_logical_offset = 0u;
}

static bool bx_tar_compare_report_stdout(struct bx_tar_compare_state* state,
                                         struct bx_diag_ctx* diag,
                                         const char* format,
                                         const char* name,
                                         const char* extra) {
    state->status = state->status < 1 ? 1 : state->status;
    if (!bx_tar_report_printf(state->report_stream,
                              diag,
                              format,
                              name,
                              extra)) {
        state->status = 2;
        return false;
    }
    return true;
}

static void bx_tar_compare_report_stderr(struct bx_tar_compare_state* state,
                                         struct bx_diag_ctx* diag,
                                         const char* format,
                                         const char* name,
                                         const char* reason) {
    fprintf(stderr, "%s: ", diag->progname);
    fprintf(stderr, format, name, reason);
    state->status = state->status < 1 ? 1 : state->status;
}

static void bx_tar_compare_report_error(struct bx_tar_compare_state* state,
                                        struct bx_diag_ctx* diag,
                                        const char* format,
                                        const char* name,
                                        const char* reason) {
    fprintf(stderr, "%s: ", diag->progname);
    fprintf(stderr, format, name, reason);
    state->status = 2;
}

static enum bx_tar_kind bx_tar_kind_from_stat_mode(mode_t mode) {
    if (S_ISREG(mode)) {
        return BX_TAR_KIND_REG;
    }
    if (S_ISDIR(mode)) {
        return BX_TAR_KIND_DIR;
    }
    if (S_ISLNK(mode)) {
        return BX_TAR_KIND_SYMLINK;
    }
    if (S_ISFIFO(mode)) {
        return BX_TAR_KIND_FIFO;
    }
    return BX_TAR_KIND_REG;
}

static bool bx_tar_compare_kind_matches(enum bx_tar_kind archive_kind, mode_t fs_mode) {
    if (archive_kind == BX_TAR_KIND_HARDLINK) {
        return S_ISREG(fs_mode);
    }
    return bx_tar_kind_from_stat_mode(fs_mode) == archive_kind;
}

static bool bx_tar_compare_verify_zero_range(int fd,
                                             size_t start_offset,
                                             size_t len,
                                             struct bx_tar_compare_state* state,
                                             const struct bx_tar_entry* entry,
                                             struct bx_diag_ctx* diag) {
    unsigned char buffer[8192];

    if (len == 0u) {
        return true;
    }
    if (lseek(fd, (off_t)start_offset, SEEK_SET) < 0) {
        bx_tar_compare_report_error(state, diag, "%s: Cannot open: %s\n", entry->name, strerror(errno));
        return false;
    }

    while (len > 0u) {
        size_t chunk = len > sizeof(buffer) ? sizeof(buffer) : len;
        ssize_t nread = read(fd, buffer, chunk);

        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            bx_tar_compare_report_error(state, diag, "%s: Cannot open: %s\n", entry->name, strerror(errno));
            return false;
        }
        if ((size_t)nread != chunk) {
            if (!state->current_reported_content_diff) {
                state->current_reported_content_diff = true;
                return bx_tar_compare_report_stdout(state, diag, "%s: Contents differ\n", entry->name, NULL);
            }
            return false;
        }
        for (size_t i = 0u; i < chunk; i++) {
            if (buffer[i] != 0u) {
                if (!state->current_reported_content_diff) {
                    state->current_reported_content_diff = true;
                    return bx_tar_compare_report_stdout(state, diag, "%s: Contents differ\n", entry->name, NULL);
                }
                return false;
            }
        }
        len -= chunk;
    }
    return true;
}

static bool bx_tar_extract_write_zero_bytes(size_t zero_len,
                                            struct bx_diag_ctx* diag) {
    unsigned char zeros[4096] = {0};

    while (zero_len > 0u) {
        size_t chunk = zero_len > sizeof(zeros) ? sizeof(zeros) : zero_len;

        if (!bx_xwrite_all(STDOUT_FILENO, zeros, chunk)) {
            bx_diag(diag, "write error: %s", strerror(errno));
            return false;
        }
        zero_len -= chunk;
    }
    return true;
}

static bool bx_tar_compare_one_entry(struct bx_tar_compare_state* state,
                                     const struct bx_tar_entry* entry,
                                     struct bx_diag_ctx* diag) {
    struct stat st;
    struct bx_tar_mapped_name clean_name = {0};
    bool stripped_absolute;
    bool stripped_dotdot;

    bx_tar_compare_clear_current_stream(state);
    if (!bx_tar_select_plan_match(state->select_plan,
                                  entry->name,
                                  state->select_plan->len == 0u,
                                  state->matched_members,
                                  NULL)) {
        state->current_skip = true;
        return true;
    }

    clean_name = bx_tar_map_member_name(entry->name,
                                        &state->name_policy,
                                        &stripped_absolute,
                                        &stripped_dotdot);
    bx_tar_warn_name_adjustments(diag,
                                 stripped_absolute,
                                 &state->warned_absolute,
                                 stripped_dotdot,
                                 &state->warned_dotdot);
    if (clean_name.text[0] == '\0') {
        bx_tar_report_empty_name(entry, diag);
        bx_tar_release_mapped_name(&clean_name);
        state->current_skip = true;
        return true;
    }
    state->current_fs_path = clean_name.owned != NULL ? clean_name.owned : xstrdup(clean_name.text);
    clean_name.owned = NULL;
    clean_name.text = state->current_fs_path;

    if (lstat(state->current_fs_path, &st) != 0) {
        if (errno == ENOENT) {
            bx_tar_compare_report_stderr(state,
                                         diag,
                                         "%s: Warning: Cannot stat: %s\n",
                                         entry->name,
                                         strerror(errno));
        }
        else {
            bx_tar_compare_report_error(state,
                                        diag,
                                        "%s: Cannot stat: %s\n",
                                        entry->name,
                                        strerror(errno));
        }
        state->current_skip = true;
        return true;
    }

    if (!bx_tar_compare_kind_matches(entry->kind, st.st_mode)) {
        if (!bx_tar_compare_report_stdout(state, diag, "%s: File type differs\n", entry->name, NULL)) {
            return false;
        }
    }
    if ((st.st_mode & 07777u) != (entry->mode & 07777u)) {
        if (!bx_tar_compare_report_stdout(state, diag, "%s: Mode differs\n", entry->name, NULL)) {
            return false;
        }
    }
    if (st.st_mtim.tv_sec != entry->mtime.tv_sec) {
        if (!bx_tar_compare_report_stdout(state, diag, "%s: Mod time differs\n", entry->name, NULL)) {
            return false;
        }
    }

    if (entry->kind == BX_TAR_KIND_SYMLINK) {
        char* target = bx_path_readlink_dup(state->current_fs_path);

        if (target == NULL) {
            bx_tar_compare_report_error(state,
                                        diag,
                                        "%s: Cannot readlink: %s\n",
                                        entry->name,
                                        strerror(errno));
            return true;
        }
        if (strcmp(target, entry->linkname) != 0
            && !bx_tar_compare_report_stdout(state, diag, "%s: Symlink differs\n", entry->name, NULL)) {
            free(target);
            return false;
        }
        free(target);
        state->current_skip = true;
        return true;
    }
    if (entry->kind == BX_TAR_KIND_HARDLINK) {
        bool link_abs = false;
        bool link_dotdot = false;
        struct bx_tar_mapped_name mapped_target = bx_tar_map_member_name(entry->linkname,
                                                                         &state->name_policy,
                                                                         &link_abs,
                                                                         &link_dotdot);
        struct stat target_st;
        bool linked = false;

        (void)link_abs;
        (void)link_dotdot;
        if (lstat(mapped_target.text, &target_st) == 0) {
            linked = target_st.st_dev == st.st_dev && target_st.st_ino == st.st_ino;
        }
        if (!linked
            && !bx_tar_compare_report_stdout(state, diag, "%s: Not linked to %s\n", entry->name, entry->linkname)) {
            bx_tar_release_mapped_name(&mapped_target);
            return false;
        }
        bx_tar_release_mapped_name(&mapped_target);
        state->current_skip = true;
        return true;
    }
    if (entry->kind != BX_TAR_KIND_REG) {
        state->current_skip = true;
        return true;
    }

    if ((size_t)st.st_size != entry->size) {
        if (!bx_tar_compare_report_stdout(state, diag, "%s: Size differs\n", entry->name, NULL)) {
            return false;
        }
        state->current_skip = true;
        return true;
    }

    state->current_fd = open(state->current_fs_path, O_RDONLY);
    if (state->current_fd < 0) {
        bx_tar_compare_report_error(state,
                                    diag,
                                    "%s: Cannot open: %s\n",
                                    entry->name,
                                    strerror(errno));
        state->current_skip = true;
        return true;
    }
    state->current_compare_contents = true;
    state->current_sparse = entry->sparse;
    return true;
}

static bool bx_tar_compare_entry_payload(struct bx_tar_compare_state* state,
                                         const struct bx_tar_entry* entry,
                                         const unsigned char* data,
                                         size_t len,
                                         struct bx_diag_ctx* diag) {
    unsigned char buffer[8192];

    if (state->current_skip || !state->current_compare_contents || len == 0u) {
        return true;
    }

    if (!state->current_sparse) {
        size_t offset = 0u;

        while (offset < len) {
            size_t chunk = len - offset;
            ssize_t nread;

            if (chunk > sizeof(buffer)) {
                chunk = sizeof(buffer);
            }
            nread = read(state->current_fd, buffer, chunk);
            if (nread < 0) {
                if (errno == EINTR) {
                    continue;
                }
                bx_tar_compare_report_error(state,
                                            diag,
                                            "%s: Cannot open: %s\n",
                                            entry->name,
                                            strerror(errno));
                state->current_compare_contents = false;
                return true;
            }
            if ((size_t)nread != chunk || memcmp(buffer, data + offset, chunk) != 0) {
                if (!state->current_reported_content_diff) {
                    state->current_reported_content_diff = true;
                    if (!bx_tar_compare_report_stdout(state, diag, "%s: Contents differ\n", entry->name, NULL)) {
                        return false;
                    }
                }
                state->current_compare_contents = false;
                return true;
            }
            offset += chunk;
        }
        return true;
    }

    while (len > 0u) {
        const struct bx_tar_sparse_extent* extent;
        size_t chunk;
        ssize_t nread;

        while (state->current_sparse_extent_index < entry->extent_count
               && state->current_sparse_extent_offset
                   == entry->extents[state->current_sparse_extent_index].size) {
            state->current_sparse_extent_index++;
            state->current_sparse_extent_offset = 0u;
        }
        if (state->current_sparse_extent_index >= entry->extent_count) {
            state->current_compare_contents = false;
            return true;
        }

        extent = &entry->extents[state->current_sparse_extent_index];
        if (state->current_sparse_extent_offset == 0u) {
            if (!bx_tar_compare_verify_zero_range(state->current_fd,
                                                  state->current_sparse_logical_offset,
                                                  extent->offset - state->current_sparse_logical_offset,
                                                  state,
                                                  entry,
                                                  diag)) {
                state->current_compare_contents = false;
                return state->status < 2;
            }
            if (lseek(state->current_fd, (off_t)extent->offset, SEEK_SET) < 0) {
                bx_tar_compare_report_error(state,
                                            diag,
                                            "%s: Cannot open: %s\n",
                                            entry->name,
                                            strerror(errno));
                state->current_compare_contents = false;
                return true;
            }
            state->current_sparse_logical_offset = extent->offset;
        }

        chunk = extent->size - state->current_sparse_extent_offset;
        if (chunk > len) {
            chunk = len;
        }
        {
            size_t compared = 0u;

            while (compared < chunk) {
                size_t read_chunk = chunk - compared;

                if (read_chunk > sizeof(buffer)) {
                    read_chunk = sizeof(buffer);
                }
                while (true) {
                    nread = read(state->current_fd, buffer, read_chunk);
                    if (nread < 0 && errno == EINTR) {
                        continue;
                    }
                    break;
                }
                if (nread < 0) {
                    bx_tar_compare_report_error(state,
                                                diag,
                                                "%s: Cannot open: %s\n",
                                                entry->name,
                                                strerror(errno));
                    state->current_compare_contents = false;
                    return true;
                }
                if ((size_t)nread != read_chunk || memcmp(buffer, data + compared, read_chunk) != 0) {
                    if (!state->current_reported_content_diff) {
                        state->current_reported_content_diff = true;
                        if (!bx_tar_compare_report_stdout(state, diag, "%s: Contents differ\n", entry->name, NULL)) {
                            return false;
                        }
                    }
                    state->current_compare_contents = false;
                    return true;
                }
                compared += read_chunk;
            }
        }

        data += chunk;
        len -= chunk;
        state->current_sparse_extent_offset += chunk;
        state->current_sparse_logical_offset += chunk;
    }
    return true;
}

static bool bx_tar_compare_end_entry(struct bx_tar_compare_state* state,
                                     const struct bx_tar_entry* entry,
                                     struct bx_diag_ctx* diag) {
    if (state->current_compare_contents && state->current_sparse) {
        while (state->current_sparse_extent_index < entry->extent_count
               && state->current_sparse_extent_offset
                   == entry->extents[state->current_sparse_extent_index].size) {
            state->current_sparse_extent_index++;
            state->current_sparse_extent_offset = 0u;
        }
        if (state->current_sparse_extent_index == entry->extent_count
            && !bx_tar_compare_verify_zero_range(state->current_fd,
                                                 state->current_sparse_logical_offset,
                                                 entry->size - state->current_sparse_logical_offset,
                                                 state,
                                                 entry,
                                                 diag)) {
            bx_tar_compare_clear_current_stream(state);
            return state->status < 2;
        }
    }
    bx_tar_compare_clear_current_stream(state);
    return true;
}

static int bx_tar_compare_finish(struct bx_tar_compare_state* state,
                                 struct bx_diag_ctx* diag) {
    if (bx_tar_select_plan_report_unmatched(state->select_plan, state->matched_members, diag)) {
        state->status = 2;
    }
    if (state->status == 2) {
        bx_tar_report_previous_errors(diag);
    }
    return state->status;
}

static bool bx_tar_extract_sparse_payload_complete(const struct bx_tar_extract_state* state,
                                                   const struct bx_tar_entry* entry) {
    size_t extent_index = state->current_sparse_extent_index;
    size_t extent_offset = state->current_sparse_extent_offset;

    while (extent_index < entry->extent_count
           && extent_offset == entry->extents[extent_index].size) {
        extent_index++;
        extent_offset = 0u;
    }
    return extent_index == entry->extent_count && extent_offset == 0u;
}

static bool bx_tar_extract_sparse_payload(struct bx_tar_extract_state* state,
                                          const struct bx_tar_entry* entry,
                                          const unsigned char* data,
                                          size_t len,
                                          struct bx_diag_ctx* diag) {
    const unsigned char* cursor = data;

    while (len > 0u) {
        const struct bx_tar_sparse_extent* extent;
        size_t chunk;

        while (state->current_sparse_extent_index < entry->extent_count
               && state->current_sparse_extent_offset
                   == entry->extents[state->current_sparse_extent_index].size) {
            state->current_sparse_extent_index++;
            state->current_sparse_extent_offset = 0u;
        }
        if (state->current_sparse_extent_index >= entry->extent_count) {
            bx_diag(diag, "invalid sparse payload");
            return false;
        }

        extent = &entry->extents[state->current_sparse_extent_index];
        if (state->current_sparse_extent_offset == 0u) {
            if (state->current_stream_mode == BX_TAR_EXTRACT_STREAM_STDOUT) {
                if (extent->offset > state->current_sparse_logical_offset
                    && !bx_tar_extract_write_zero_bytes(extent->offset
                                                            - state->current_sparse_logical_offset,
                                                        diag)) {
                    return false;
                }
                state->current_sparse_logical_offset = extent->offset;
            }
            else if (state->current_stream_mode == BX_TAR_EXTRACT_STREAM_FILE
                     && lseek(state->current_fd, (off_t)extent->offset, SEEK_SET) < 0) {
                bx_diag(diag, "%s: %s", state->current_dest_path, strerror(errno));
                return false;
            }
        }

        chunk = extent->size - state->current_sparse_extent_offset;
        if (chunk > len) {
            chunk = len;
        }
        if (state->current_stream_mode == BX_TAR_EXTRACT_STREAM_STDOUT) {
            if (!bx_xwrite_all(STDOUT_FILENO, cursor, chunk)) {
                bx_diag(diag, "write error: %s", strerror(errno));
                return false;
            }
            state->current_sparse_logical_offset += chunk;
        }
        else if (!bx_archive_write_regular_payload(state->current_fd, cursor, chunk, false, diag)) {
            return false;
        }

        cursor += chunk;
        len -= chunk;
        state->current_sparse_extent_offset += chunk;
    }

    return true;
}

static bool bx_tar_extract_one_entry(struct bx_tar_extract_state* state,
                                     const struct bx_tar_entry* entry,
                                     struct bx_diag_ctx* diag) {
    struct bx_tar_mapped_name clean_name = {0};
    char* dest_path = NULL;
    const char* extract_dir = NULL;
    const char* report_name;
    bool stripped_absolute;
    bool stripped_dotdot;
    uid_t mapped_owner = 0;
    gid_t mapped_group = 0;
    bool owner_mapped = false;
    bool group_mapped = false;

    if (!bx_tar_starting_file_gate_reached(&state->starting_file_reached,
                                           state->options->starting_file,
                                           entry)) {
        bx_tar_extract_clear_current_stream(state);
        return true;
    }

    if (!bx_tar_select_plan_match(state->select_plan,
                                  entry->name,
                                  state->select_plan->len == 0u,
                                  state->matched_members,
                                  &extract_dir)) {
        bx_tar_extract_clear_current_stream(state);
        return true;
    }

    clean_name = bx_tar_map_member_name(entry->name,
                                        &state->name_policy,
                                        &stripped_absolute,
                                        &stripped_dotdot);
    bx_tar_warn_name_adjustments(diag,
                                 stripped_absolute,
                                 &state->warned_absolute,
                                 stripped_dotdot,
                                 &state->warned_dotdot);
    if (clean_name.text[0] == '\0') {
        bx_tar_report_empty_name(entry, diag);
        bx_tar_release_mapped_name(&clean_name);
        bx_tar_extract_clear_current_stream(state);
        return true;
    }
    report_name = state->options->report_mapped_names ? clean_name.text : entry->name;

    if (state->options->to_stdout) {
        bool ok = true;
        bx_tar_extract_clear_current_stream(state);
        if (state->options->verbose_reports
            && !(state->options->report_block_numbers
                     ? bx_tar_report_member_line_with_block(state->report_stream,
                                                            bx_tar_extract_report_block_index(entry),
                                                            report_name,
                                                            entry->kind == BX_TAR_KIND_DIR,
                                                            diag)
                     : bx_tar_report_member_line(state->report_stream,
                                                 report_name,
                                                 entry->kind == BX_TAR_KIND_DIR,
                                                 diag))) {
            bx_tar_release_mapped_name(&clean_name);
            return false;
        }
        if (entry->kind == BX_TAR_KIND_REG) {
            state->current_stream_mode = BX_TAR_EXTRACT_STREAM_STDOUT;
            state->current_sparse = entry->sparse;
        }
        bx_tar_release_mapped_name(&clean_name);
        return ok;
    }

    bx_tar_extract_map_entry_ids(state,
                                 entry,
                                 &mapped_owner,
                                 &mapped_group,
                                 &owner_mapped,
                                 &group_mapped);

    dest_path = extract_dir ? bx_path_join(extract_dir, clean_name.text) : xstrdup(clean_name.text);

    if (state->options->keep_old_files
        && access(dest_path, F_OK) == 0
        && entry->kind != BX_TAR_KIND_DIR) {
        fprintf(stderr, "%s: %s: Cannot open: File exists\n", diag->progname, entry->name);
        state->status = 2;
        bx_tar_release_mapped_name(&clean_name);
        free(dest_path);
        bx_tar_extract_clear_current_stream(state);
        return true;
    }
    if (state->options->verbose_reports
        && !(state->options->report_block_numbers
                 ? bx_tar_report_member_line_with_block(state->report_stream,
                                                        bx_tar_extract_report_block_index(entry),
                                                        report_name,
                                                        entry->kind == BX_TAR_KIND_DIR,
                                                        diag)
                 : bx_tar_report_member_line(state->report_stream,
                                             report_name,
                                             entry->kind == BX_TAR_KIND_DIR,
                                             diag))) {
        bx_tar_release_mapped_name(&clean_name);
        free(dest_path);
        return false;
    }
    bx_tar_release_mapped_name(&clean_name);

    if (!bx_tar_extract_prepare_parent_dirs_safe(state, dest_path, diag)) {
        free(dest_path);
        return false;
    }

    if (entry->kind == BX_TAR_KIND_DIR) {
        bool mkdir_needed = false;

        if (mkdir(dest_path, 0777u) != 0) {
            if (errno != EEXIST) {
                bx_diag(diag, "%s: %s", dest_path, strerror(errno));
                free(dest_path);
                return false;
            }
            if (!bx_tar_extract_prepare_final_dir_target(state, dest_path, &mkdir_needed, diag)) {
                free(dest_path);
                return false;
            }
            if (mkdir_needed && mkdir(dest_path, 0777u) != 0) {
                bx_diag(diag, "%s: %s", dest_path, strerror(errno));
                free(dest_path);
                return false;
            }
        }
        bx_archive_pending_dirs_record(&state->dirs,
                                       dest_path,
                                       entry->mode,
                                       !state->options->touch_mtime,
                                       entry->mtime);
        if (!bx_tar_extract_apply_path_ownership(dest_path,
                                                 false,
                                                 mapped_owner,
                                                 mapped_group,
                                                 owner_mapped,
                                                 group_mapped,
                                                 diag)) {
            free(dest_path);
            return false;
        }
        free(dest_path);
        bx_tar_extract_clear_current_stream(state);
        return true;
    }

    if (entry->kind == BX_TAR_KIND_REG) {
        int fd;

        fd = open(dest_path, O_WRONLY | O_CREAT | O_EXCL, entry->mode & 07777u);
        if (fd < 0) {
            if (errno != EEXIST && errno != EISDIR) {
                bx_diag(diag, "%s: %s", dest_path, strerror(errno));
                free(dest_path);
                return false;
            }
            if (!bx_tar_extract_prepare_final_non_dir_target(state, entry, dest_path, diag)) {
                free(dest_path);
                state->status = 2;
                bx_tar_extract_clear_current_stream(state);
                return true;
            }
            fd = open(dest_path, O_WRONLY | O_CREAT | O_EXCL, entry->mode & 07777u);
            if (fd < 0) {
                bx_diag(diag, "%s: %s", dest_path, strerror(errno));
                free(dest_path);
                return false;
            }
        }
        bx_tar_extract_clear_current_stream(state);
        state->current_fd = fd;
        state->current_dest_path = dest_path;
        state->current_mode_bits = entry->mode;
        state->current_mtime = entry->mtime;
        state->current_owner = mapped_owner;
        state->current_group = mapped_group;
        state->current_owner_mapped = owner_mapped;
        state->current_group_mapped = group_mapped;
        state->current_stream_mode = BX_TAR_EXTRACT_STREAM_FILE;
        state->current_sparse = entry->sparse;
        return true;
    }
    else if (entry->kind == BX_TAR_KIND_SYMLINK) {
        if (!bx_tar_extract_prepare_final_non_dir_target(state, entry, dest_path, diag)) {
            free(dest_path);
            state->status = 2;
            return true;
        }
        if (symlink(entry->linkname, dest_path) != 0) {
            bx_diag(diag, "%s: %s", dest_path, strerror(errno));
            free(dest_path);
            return false;
        }
        if (!state->options->touch_mtime
            && !bx_archive_set_path_mtime(dest_path, entry->mtime, true, diag)) {
            free(dest_path);
            return false;
        }
        if (!bx_tar_extract_apply_path_ownership(dest_path,
                                                 true,
                                                 mapped_owner,
                                                 mapped_group,
                                                 owner_mapped,
                                                 group_mapped,
                                                 diag)) {
            free(dest_path);
            return false;
        }
    }
    else if (entry->kind == BX_TAR_KIND_HARDLINK) {
        bool target_stripped_absolute = false;
        bool target_stripped_dotdot = false;
        struct bx_tar_mapped_name mapped_target = bx_tar_map_member_name(entry->linkname,
                                                                         &state->name_policy,
                                                                         &target_stripped_absolute,
                                                                         &target_stripped_dotdot);
        char* target = extract_dir ? bx_path_join(extract_dir, mapped_target.text)
                                   : xstrdup(mapped_target.text);
        (void)target_stripped_absolute;
        (void)target_stripped_dotdot;
        bx_tar_release_mapped_name(&mapped_target);
        if (!bx_tar_extract_prepare_final_non_dir_target(state, entry, dest_path, diag)) {
            free(dest_path);
            free(target);
            state->status = 2;
            return true;
        }
        if (link(target, dest_path) != 0) {
            bx_diag(diag, "%s: %s", dest_path, strerror(errno));
            free(dest_path);
            free(target);
            return false;
        }
        free(target);
        if (!bx_tar_extract_apply_path_ownership(dest_path,
                                                 false,
                                                 mapped_owner,
                                                 mapped_group,
                                                 owner_mapped,
                                                 group_mapped,
                                                 diag)) {
            free(dest_path);
            return false;
        }
    }
    else if (entry->kind == BX_TAR_KIND_FIFO) {
        if (!bx_tar_extract_prepare_final_non_dir_target(state, entry, dest_path, diag)) {
            free(dest_path);
            state->status = 2;
            return true;
        }
        if (mkfifo(dest_path, entry->mode & 07777u) != 0) {
            bx_diag(diag, "%s: %s", dest_path, strerror(errno));
            free(dest_path);
            return false;
        }
        if (!state->options->touch_mtime
            && !bx_archive_set_path_mtime(dest_path, entry->mtime, false, diag)) {
            free(dest_path);
            return false;
        }
        if (!bx_tar_extract_apply_path_ownership(dest_path,
                                                 false,
                                                 mapped_owner,
                                                 mapped_group,
                                                 owner_mapped,
                                                 group_mapped,
                                                 diag)) {
            free(dest_path);
            return false;
        }
    }

    free(dest_path);
    bx_tar_extract_clear_current_stream(state);
    return true;
}

static bool bx_tar_extract_entry_payload(struct bx_tar_extract_state* state,
                                         const struct bx_tar_entry* entry,
                                         const unsigned char* data,
                                         size_t len,
                                         struct bx_diag_ctx* diag) {
    if (state->current_stream_mode == BX_TAR_EXTRACT_STREAM_NONE || len == 0u) {
        return true;
    }
    if (state->current_sparse) {
        return bx_tar_extract_sparse_payload(state, entry, data, len, diag);
    }
    if (state->current_stream_mode == BX_TAR_EXTRACT_STREAM_STDOUT) {
        if (!bx_xwrite_all(STDOUT_FILENO, data, len)) {
            bx_diag(diag, "write error: %s", strerror(errno));
            return false;
        }
        return true;
    }
    return bx_archive_write_regular_payload(state->current_fd, data, len, false, diag);
}

static bool bx_tar_extract_end_entry(struct bx_tar_extract_state* state,
                                     const struct bx_tar_entry* entry,
                                     struct bx_diag_ctx* diag) {
    char* dest_path = state->current_dest_path;
    int fd = state->current_fd;
    mode_t mode = state->current_mode_bits;
    struct timespec mtime = state->current_mtime;

    (void)entry;

    if (state->current_stream_mode != BX_TAR_EXTRACT_STREAM_FILE) {
        if (state->current_stream_mode == BX_TAR_EXTRACT_STREAM_STDOUT && state->current_sparse) {
            if (!bx_tar_extract_sparse_payload_complete(state, entry)) {
                bx_diag(diag, "invalid sparse payload");
                bx_tar_extract_clear_current_stream(state);
                return false;
            }
            if (entry->size > state->current_sparse_logical_offset
                && !bx_tar_extract_write_zero_bytes(entry->size
                                                        - state->current_sparse_logical_offset,
                                                    diag)) {
                bx_tar_extract_clear_current_stream(state);
                return false;
            }
        }
        bx_tar_extract_clear_current_stream(state);
        return true;
    }

    if (state->current_sparse) {
        if (!bx_tar_extract_sparse_payload_complete(state, entry)) {
            bx_diag(diag, "invalid sparse payload");
            bx_tar_extract_clear_current_stream(state);
            return false;
        }
        if (ftruncate(fd, (off_t)entry->size) != 0) {
            bx_diag(diag, "%s: %s", dest_path, strerror(errno));
            bx_tar_extract_clear_current_stream(state);
            return false;
        }
    }
    if (state->current_owner_mapped || state->current_group_mapped) {
        if (fchown(fd,
                   state->current_owner_mapped ? state->current_owner : (uid_t)-1,
                   state->current_group_mapped ? state->current_group : (gid_t)-1) != 0) {
            bx_diag(diag, "%s: %s", dest_path, strerror(errno));
            bx_tar_extract_clear_current_stream(state);
            return false;
        }
    }
    if (fchmod(fd, mode & 07777u) != 0) {
        bx_diag(diag, "%s: %s", dest_path, strerror(errno));
        bx_tar_extract_clear_current_stream(state);
        return false;
    }
    if (!state->options->touch_mtime
        && !bx_archive_set_fd_mtime(fd, dest_path, mtime, diag)) {
        bx_tar_extract_clear_current_stream(state);
        return false;
    }

    state->current_fd = -1;
    state->current_dest_path = NULL;
    state->current_stream_mode = BX_TAR_EXTRACT_STREAM_NONE;
    state->current_mode_bits = 0u;
    state->current_mtime.tv_sec = 0;
    state->current_mtime.tv_nsec = 0;
    state->current_sparse = false;
    state->current_sparse_extent_index = 0u;
    state->current_sparse_extent_offset = 0u;
    state->current_sparse_logical_offset = 0u;

    if (close(fd) != 0) {
        bx_diag(diag, "%s: %s", dest_path, strerror(errno));
        free(dest_path);
        return false;
    }
    free(dest_path);
    return true;
}

static int bx_tar_extract_finish(struct bx_tar_extract_state* state,
                                 struct bx_diag_ctx* diag) {
    if (bx_tar_select_plan_report_unmatched(state->select_plan, state->matched_members, diag)) {
        state->status = 2;
    }
    if (!bx_archive_pending_dirs_apply(&state->dirs, diag)) {
        return 2;
    }
    if (state->status == 2) {
        bx_tar_report_previous_errors(diag);
    }
    return state->status;
}

static bool bx_tar_list_one_entry(struct bx_tar_list_state* state,
                                  const struct bx_tar_entry* entry,
                                  struct bx_diag_ctx* diag) {
    struct bx_tar_mapped_name clean_name = {0};
    const char* report_name;
    bool stripped_absolute;
    bool stripped_dotdot;

    if (!bx_tar_starting_file_gate_reached(&state->starting_file_reached,
                                           state->options->starting_file,
                                           entry)) {
        return true;
    }
    if (!bx_tar_select_plan_match(state->select_plan,
                                  entry->name,
                                  state->select_plan->len == 0u,
                                  state->matched_members,
                                  NULL)) {
        return true;
    }
    clean_name = bx_tar_map_member_name(entry->name,
                                        &state->name_policy,
                                        &stripped_absolute,
                                        &stripped_dotdot);
    bx_tar_warn_name_adjustments(diag,
                                 stripped_absolute,
                                 &state->warned_absolute,
                                 stripped_dotdot,
                                 &state->warned_dotdot);
    if (clean_name.text[0] == '\0') {
        bx_tar_report_empty_name(entry, diag);
        bx_tar_release_mapped_name(&clean_name);
        return true;
    }
    report_name = state->options->report_mapped_names ? clean_name.text : entry->name;
    if (!(state->options->report_block_numbers
              ? bx_tar_report_member_line_with_block(state->report_stream,
                                                     entry->header_block_index,
                                                     report_name,
                                                     entry->kind == BX_TAR_KIND_DIR,
                                                     diag)
              : bx_tar_report_member_line(state->report_stream,
                                          report_name,
                                          entry->kind == BX_TAR_KIND_DIR,
                                          diag))) {
        bx_tar_release_mapped_name(&clean_name);
        return false;
    }
    bx_tar_release_mapped_name(&clean_name);
    return true;
}

static bool bx_tar_list_stream_finish(void* user,
                                      uint64_t block_index,
                                      enum bx_tar_stream_end_kind end_kind,
                                      uint64_t total_bytes_read,
                                      struct bx_diag_ctx* diag) {
    struct bx_tar_list_state* state = user;
    state->total_bytes_read = bx_tar_reported_total_bytes_read(block_index, end_kind, total_bytes_read);
    return bx_tar_report_archive_end_if_requested(state->options,
                                                  state->report_stream,
                                                  block_index,
                                                  end_kind,
                                                  diag);
}

static int bx_tar_list_finish(struct bx_tar_list_state* state,
                              struct bx_diag_ctx* diag) {
    if (bx_tar_select_plan_report_unmatched(state->select_plan, state->matched_members, diag)) {
        bx_tar_report_previous_errors(diag);
        return 2;
    }
    return 0;
}

static bool bx_tar_extract_stream_visit(void* user,
                                        const struct bx_tar_entry* entry,
                                        struct bx_diag_ctx* diag) {
    return bx_tar_extract_one_entry(user, entry, diag);
}

static bool bx_tar_extract_stream_payload_visit(void* user,
                                                const struct bx_tar_entry* entry,
                                                const unsigned char* data,
                                                size_t len,
                                                struct bx_diag_ctx* diag) {
    return bx_tar_extract_entry_payload(user, entry, data, len, diag);
}

static bool bx_tar_extract_stream_end_visit(void* user,
                                            const struct bx_tar_entry* entry,
                                            struct bx_diag_ctx* diag) {
    return bx_tar_extract_end_entry(user, entry, diag);
}

static bool bx_tar_extract_stream_finish(void* user,
                                         uint64_t block_index,
                                         enum bx_tar_stream_end_kind end_kind,
                                         uint64_t total_bytes_read,
                                         struct bx_diag_ctx* diag) {
    struct bx_tar_extract_state* state = user;
    state->total_bytes_read = bx_tar_reported_total_bytes_read(block_index, end_kind, total_bytes_read);
    return bx_tar_report_archive_end_if_requested(state->options,
                                                  state->report_stream,
                                                  block_index,
                                                  end_kind,
                                                  diag);
}

static bool bx_tar_list_stream_visit(void* user,
                                     const struct bx_tar_entry* entry,
                                     struct bx_diag_ctx* diag) {
    return bx_tar_list_one_entry(user, entry, diag);
}

static bool bx_tar_compare_stream_visit(void* user,
                                        const struct bx_tar_entry* entry,
                                        struct bx_diag_ctx* diag) {
    return bx_tar_compare_one_entry(user, entry, diag);
}

static bool bx_tar_compare_stream_payload_visit(void* user,
                                                const struct bx_tar_entry* entry,
                                                const unsigned char* data,
                                                size_t len,
                                                struct bx_diag_ctx* diag) {
    return bx_tar_compare_entry_payload(user, entry, data, len, diag);
}

static bool bx_tar_compare_stream_end_visit(void* user,
                                            const struct bx_tar_entry* entry,
                                            struct bx_diag_ctx* diag) {
    return bx_tar_compare_end_entry(user, entry, diag);
}

static bool bx_tar_compare_stream_finish(void* user,
                                         uint64_t block_index,
                                         enum bx_tar_stream_end_kind end_kind,
                                         uint64_t total_bytes_read,
                                         struct bx_diag_ctx* diag) {
    struct bx_tar_compare_state* state = user;
    state->total_bytes_read = bx_tar_reported_total_bytes_read(block_index, end_kind, total_bytes_read);
    return bx_tar_report_archive_end_if_requested(state->options,
                                                  state->report_stream,
                                                  block_index,
                                                  end_kind,
                                                  diag);
}

static int bx_tar_process_archive_stream(const struct bx_tar_options* options,
                                         const struct bx_tar_select_plan* select_plan,
                                         struct bx_diag_ctx* diag) {
    struct bx_tar_reader_stream_options reader_options = {
        .archive_path = options->archive_path,
        .required_codec = bx_tar_input_required_codec(options),
        .skip_owner_group_names = options->owner_map.len == 0u && options->group_map.len == 0u,
        .skip_owner_group_ids = options->owner_map.len == 0u && options->group_map.len == 0u,
    };
    struct bx_tar_report_output report_output = {0};
    bool need_report_output = options->mode == BX_TAR_MODE_LIST
        || options->mode == BX_TAR_MODE_COMPARE
        || options->verbose_reports
        || (options->mode == BX_TAR_MODE_EXTRACT && options->report_block_numbers);
    FILE* report_default_stream = (options->mode == BX_TAR_MODE_LIST
                                   || options->mode == BX_TAR_MODE_COMPARE
                                   || (options->mode == BX_TAR_MODE_EXTRACT && !options->to_stdout))
        ? stdout
        : stderr;

    if (need_report_output
        && !bx_tar_report_output_init(&report_output,
                                      options->index_file_path,
                                      report_default_stream,
                                      diag)) {
        return 2;
    }

    if (options->mode == BX_TAR_MODE_LIST) {
        struct bx_tar_list_state state;
        struct bx_tar_stream_visitor_ops visitor_ops = {
            .user = &state,
            .begin_entry = bx_tar_list_stream_visit,
            .finish_archive = bx_tar_list_stream_finish,
            .stream_sparse_payload = true,
        };
        int rc;

        bx_tar_list_state_init(&state, options, select_plan, report_output.stream);
        if (!bx_tar_visit_archive_stream(&reader_options, &visitor_ops, diag)) {
            bx_tar_list_state_cleanup(&state);
            bx_tar_report_output_cleanup(&report_output);
            return 2;
        }
        if (!bx_tar_report_output_finish(&report_output, diag)) {
            bx_tar_list_state_cleanup(&state);
            return 2;
        }
        if (options->report_totals
            && !bx_tar_report_totals_line(false, state.total_bytes_read, diag)) {
            bx_tar_list_state_cleanup(&state);
            return 2;
        }
        rc = bx_tar_list_finish(&state, diag);
        bx_tar_list_state_cleanup(&state);
        return rc;
    }
    else if (options->mode == BX_TAR_MODE_COMPARE) {
        struct bx_tar_compare_state state;
        struct bx_tar_stream_visitor_ops visitor_ops = {
            .user = &state,
            .begin_entry = bx_tar_compare_stream_visit,
            .visit_payload = bx_tar_compare_stream_payload_visit,
            .end_entry = bx_tar_compare_stream_end_visit,
            .finish_archive = bx_tar_compare_stream_finish,
            .stream_sparse_payload = true,
        };
        int rc;

        bx_tar_compare_state_init(&state, options, select_plan, report_output.stream);
        if (!bx_tar_visit_archive_stream(&reader_options, &visitor_ops, diag)) {
            bx_tar_compare_state_cleanup(&state);
            bx_tar_report_output_cleanup(&report_output);
            return 2;
        }
        if (!bx_tar_report_output_finish(&report_output, diag)) {
            bx_tar_compare_state_cleanup(&state);
            return 2;
        }
        if (options->report_totals
            && !bx_tar_report_totals_line(false, state.total_bytes_read, diag)) {
            bx_tar_compare_state_cleanup(&state);
            return 2;
        }
        rc = bx_tar_compare_finish(&state, diag);
        bx_tar_compare_state_cleanup(&state);
        return rc;
    }
    else {
        struct bx_tar_extract_state state;
        struct bx_tar_stream_visitor_ops visitor_ops = {
            .user = &state,
            .begin_entry = bx_tar_extract_stream_visit,
            .visit_payload = bx_tar_extract_stream_payload_visit,
            .end_entry = bx_tar_extract_stream_end_visit,
            .finish_archive = bx_tar_extract_stream_finish,
            .stream_sparse_payload = true,
        };
        int rc;

        bx_tar_extract_state_init(&state, options, select_plan, report_output.stream);
        if (!bx_tar_visit_archive_stream(&reader_options, &visitor_ops, diag)) {
            bx_tar_extract_state_cleanup(&state);
            bx_tar_report_output_cleanup(&report_output);
            return 2;
        }
        if (!bx_tar_report_output_finish(&report_output, diag)) {
            bx_tar_extract_state_cleanup(&state);
            return 2;
        }
        if (options->report_totals
            && !bx_tar_report_totals_line(false, state.total_bytes_read, diag)) {
            bx_tar_extract_state_cleanup(&state);
            return 2;
        }
        rc = bx_tar_extract_finish(&state, diag);
        bx_tar_extract_state_cleanup(&state);
        return rc;
    }
}

static enum bx_tar_stream_kind bx_tar_stream_kind_from_entry_kind(enum bx_tar_kind kind) {
    switch (kind) {
        case BX_TAR_KIND_REG:
            return BX_TAR_STREAM_KIND_REG;
        case BX_TAR_KIND_DIR:
            return BX_TAR_STREAM_KIND_DIR;
        case BX_TAR_KIND_SYMLINK:
            return BX_TAR_STREAM_KIND_SYMLINK;
        case BX_TAR_KIND_HARDLINK:
            return BX_TAR_STREAM_KIND_HARDLINK;
        case BX_TAR_KIND_FIFO:
            return BX_TAR_STREAM_KIND_FIFO;
    }
    return BX_TAR_STREAM_KIND_REG;
}

struct bx_tar_stream_counting_sink_user {
    const struct bx_tar_stream_sink* inner;
    size_t* bytes_written;
};

static bool bx_tar_stream_counting_sink_write(void* user, const void* data, size_t len) {
    struct bx_tar_stream_counting_sink_user* counting_user = user;

    if (!counting_user->inner->write(counting_user->inner->user, data, len)) {
        return false;
    }
    *counting_user->bytes_written += len;
    return true;
}

static bool bx_tar_write_parsed_entry_sink(const struct bx_tar_stream_sink* sink,
                                           size_t* bytes_written_io,
                                           const struct bx_tar_entry* entry,
                                           struct bx_diag_ctx* diag) {
    struct bx_tar_stream_counting_sink_user counting_user = {
        .inner = sink,
        .bytes_written = bytes_written_io,
    };
    struct bx_tar_stream_sink counting_sink = {
        .user = &counting_user,
        .write = bx_tar_stream_counting_sink_write,
        .callback_owns_errors = sink->callback_owns_errors,
    };

    return bx_tar_stream_write_raw_entry(&counting_sink,
                                         entry->name,
                                         entry->linkname,
                                         entry->uname,
                                         entry->gname,
                                         bx_tar_stream_kind_from_entry_kind(entry->kind),
                                         entry->mode,
                                         entry->uid,
                                         entry->gid,
                                         entry->data,
                                         entry->data_len,
                                         entry->mtime,
                                         true,
                                         diag);
}

struct bx_tar_rewrite_stream_ctx {
    const struct bx_tar_reader_stream_options* reader_options;
    const struct bx_tar_select_plan* delete_plan;
    bool* matched_members;
    bool* had_selection_errors;
    const struct bx_archive_fs_list* appended_files;
    const struct bx_archive_name_list* source_archives;
    const struct bx_tar_options* options;
    uint64_t total_bytes_written;
};

struct bx_tar_update_record {
    char* name;
    struct timespec mtime;
};

struct bx_tar_update_record_list {
    struct bx_tar_update_record* items;
    size_t len;
    size_t cap;
};

struct bx_tar_update_scan_state {
    struct bx_tar_update_record_list* records;
};

struct bx_tar_rewrite_visit_state {
    const struct bx_tar_rewrite_stream_ctx* ctx;
    const struct bx_tar_stream_sink* sink;
    struct bx_tar_stream_options stream_options;
    size_t bytes_written;
    struct bx_tar_stream_counting_sink_user counting_user;
    struct bx_tar_stream_sink counting_sink;
    struct bx_tar_stream_live_entry current_live_entry;
    bool current_skip;
};

static ssize_t bx_tar_rewrite_find_delete_match(const struct bx_tar_select_plan* plan,
                                                const char* name) {
    size_t i;

    if (plan == NULL) {
        return -1;
    }
    for (i = 0u; i < plan->len; i++) {
        if (bx_tar_select_member_matches_name(&plan->members[i], name)) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static bool bx_tar_rewrite_stream_begin_entry(void* user,
                                              const struct bx_tar_entry* entry,
                                              struct bx_diag_ctx* diag) {
    struct bx_tar_rewrite_visit_state* state = user;
    ssize_t match_index;

    state->current_skip = false;
    match_index = bx_tar_rewrite_find_delete_match(state->ctx->delete_plan, entry->name);
    if (match_index >= 0) {
        if (state->ctx->matched_members != NULL) {
            state->ctx->matched_members[(size_t)match_index] = true;
        }
        state->current_skip = true;
        return true;
    }

    if (entry->kind == BX_TAR_KIND_REG && !entry->sparse) {
        return bx_tar_stream_start_raw_entry(&state->current_live_entry,
                                             &state->counting_sink,
                                             entry->name,
                                             entry->linkname,
                                             entry->uname,
                                             entry->gname,
                                             bx_tar_stream_kind_from_entry_kind(entry->kind),
                                             entry->mode,
                                             entry->uid,
                                             entry->gid,
                                             entry->size,
                                             entry->mtime,
                                             true,
                                             diag);
    }
    if (entry->kind == BX_TAR_KIND_REG && entry->sparse) {
        return bx_tar_stream_start_sparse_v1_entry(&state->current_live_entry,
                                                   &state->counting_sink,
                                                   entry->name,
                                                   entry->uname,
                                                   entry->gname,
                                                   entry->mode,
                                                   entry->uid,
                                                   entry->gid,
                                                   entry->extents,
                                                   entry->extent_count,
                                                   entry->size,
                                                   entry->data_len,
                                                   entry->mtime,
                                                   diag);
    }

    return bx_tar_write_parsed_entry_sink(state->sink, &state->bytes_written, entry, diag);
}

static bool bx_tar_rewrite_stream_visit_payload(void* user,
                                                const struct bx_tar_entry* entry,
                                                const unsigned char* data,
                                                size_t len,
                                                struct bx_diag_ctx* diag) {
    struct bx_tar_rewrite_visit_state* state = user;

    (void)entry;
    if (state->current_skip || !state->current_live_entry.active) {
        return true;
    }
    return bx_tar_stream_write_raw_entry_chunk(&state->current_live_entry, data, len, diag);
}

static bool bx_tar_rewrite_stream_end_entry(void* user,
                                            const struct bx_tar_entry* entry,
                                            struct bx_diag_ctx* diag) {
    struct bx_tar_rewrite_visit_state* state = user;

    (void)entry;
    if (state->current_skip) {
        state->current_skip = false;
        return true;
    }
    if (!state->current_live_entry.active) {
        return true;
    }
    return bx_tar_stream_finish_raw_entry(&state->current_live_entry, diag);
}

static bool bx_tar_rewrite_stream_visit_reader(const struct bx_tar_rewrite_stream_ctx* ctx,
                                               struct bx_tar_rewrite_visit_state* state,
                                               const struct bx_tar_reader_stream_options* reader_options,
                                               struct bx_diag_ctx* diag) {
    struct bx_tar_stream_visitor_ops visitor_ops = {
        .user = state,
        .begin_entry = bx_tar_rewrite_stream_begin_entry,
        .visit_payload = bx_tar_rewrite_stream_visit_payload,
        .end_entry = bx_tar_rewrite_stream_end_entry,
        .stream_sparse_payload = true,
    };

    (void)ctx;
    return bx_tar_visit_archive_stream(reader_options, &visitor_ops, diag);
}

static bool bx_tar_source_archive_is_unsupported_compressed(const char* path,
                                                            struct bx_diag_ctx* diag) {
    static const unsigned char lzip_magic[] = {'L', 'Z', 'I', 'P'};
    static const unsigned char compress_magic[] = {0x1f, 0x9d};
    unsigned char header[6];
    int fd;
    ssize_t nread;

    if (path == NULL || strcmp(path, "-") == 0) {
        return false;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        bx_diag(diag, "%s: %s", path, strerror(errno));
        return true;
    }
    nread = read(fd, header, sizeof(header));
    close(fd);
    if (nread < 0) {
        bx_diag(diag, "%s: %s", path, strerror(errno));
        return true;
    }

    if ((size_t)nread >= sizeof(lzip_magic) && memcmp(header, lzip_magic, sizeof(lzip_magic)) == 0) {
        bx_diag(diag, "%s: unsupported compressed archive format", path);
        return true;
    }
    if ((size_t)nread >= sizeof(compress_magic) && memcmp(header, compress_magic, sizeof(compress_magic)) == 0) {
        bx_diag(diag, "%s: unsupported compressed archive format", path);
        return true;
    }
    return false;
}

static int bx_tar_timespec_compare(struct timespec left, struct timespec right) {
    if (left.tv_sec < right.tv_sec) {
        return -1;
    }
    if (left.tv_sec > right.tv_sec) {
        return 1;
    }
    if (left.tv_nsec < right.tv_nsec) {
        return -1;
    }
    if (left.tv_nsec > right.tv_nsec) {
        return 1;
    }
    return 0;
}

static struct timespec bx_tar_entry_stat_time(const struct bx_archive_fs_entry* entry,
                                              bool use_ctime) {
    return use_ctime ? entry->st.st_ctim : entry->st.st_mtim;
}

static void bx_tar_filter_newer_entries(struct bx_archive_fs_list* list,
                                        struct timespec cutoff,
                                        bool use_ctime) {
    size_t read_index;
    size_t write_index = 0u;

    for (read_index = 0u; read_index < list->len; read_index++) {
        bool keep = bx_tar_timespec_compare(
            bx_tar_entry_stat_time(&list->entries[read_index], use_ctime),
            cutoff
        ) > 0;

        if (!keep) {
            free(list->entries[read_index].source_path);
            free(list->entries[read_index].archive_path);
            free(list->entries[read_index].link_target);
            continue;
        }
        if (write_index != read_index) {
            list->entries[write_index] = list->entries[read_index];
        }
        write_index++;
    }
    list->len = write_index;
}

static void bx_tar_update_record_list_free(struct bx_tar_update_record_list* list) {
    size_t i;

    for (i = 0u; i < list->len; i++) {
        free(list->items[i].name);
    }
    free(list->items);
    list->items = NULL;
    list->len = 0u;
    list->cap = 0u;
}

static struct bx_tar_update_record* bx_tar_update_record_list_find(struct bx_tar_update_record_list* list,
                                                                   const char* name) {
    size_t i;

    for (i = 0u; i < list->len; i++) {
        if (strcmp(list->items[i].name, name) == 0) {
            return &list->items[i];
        }
    }
    return NULL;
}

static bool bx_tar_update_record_list_note(struct bx_tar_update_record_list* list,
                                           const char* name,
                                           struct timespec mtime) {
    struct bx_tar_update_record* record = bx_tar_update_record_list_find(list, name);

    if (record != NULL) {
        if (bx_tar_timespec_compare(record->mtime, mtime) < 0) {
            record->mtime = mtime;
        }
        return true;
    }

    if (list->len == list->cap) {
        size_t next_cap = list->cap ? list->cap * 2u : 32u;
        list->items = xrealloc(list->items, next_cap * sizeof(*list->items));
        list->cap = next_cap;
    }

    record = &list->items[list->len++];
    record->name = xstrdup(name);
    record->mtime = mtime;
    return true;
}

static const struct bx_tar_update_record* bx_tar_update_record_list_lookup(
    const struct bx_tar_update_record_list* list,
    const char* name) {
    size_t i;

    for (i = 0u; i < list->len; i++) {
        if (strcmp(list->items[i].name, name) == 0) {
            return &list->items[i];
        }
    }
    return NULL;
}

static bool bx_tar_update_scan_begin_entry(void* user,
                                           const struct bx_tar_entry* entry,
                                           struct bx_diag_ctx* diag) {
    struct bx_tar_update_scan_state* state = user;

    (void)diag;
    return bx_tar_update_record_list_note(state->records, entry->name, entry->mtime);
}

static bool bx_tar_update_scan_visit_payload(void* user,
                                             const struct bx_tar_entry* entry,
                                             const unsigned char* data,
                                             size_t len,
                                             struct bx_diag_ctx* diag) {
    (void)user;
    (void)entry;
    (void)data;
    (void)len;
    (void)diag;
    return true;
}

static bool bx_tar_update_scan_end_entry(void* user,
                                         const struct bx_tar_entry* entry,
                                         struct bx_diag_ctx* diag) {
    (void)user;
    (void)entry;
    (void)diag;
    return true;
}

static bool bx_tar_collect_archived_mtimes(const struct bx_tar_reader_stream_options* reader_options,
                                           struct bx_tar_update_record_list* records,
                                           struct bx_diag_ctx* diag) {
    struct bx_tar_update_scan_state state = {
        .records = records,
    };
    struct bx_tar_stream_visitor_ops visitor_ops = {
        .user = &state,
        .begin_entry = bx_tar_update_scan_begin_entry,
        .visit_payload = bx_tar_update_scan_visit_payload,
        .end_entry = bx_tar_update_scan_end_entry,
        .stream_sparse_payload = true,
    };

    return bx_tar_visit_archive_stream(reader_options, &visitor_ops, diag);
}

static void bx_tar_filter_update_entries(struct bx_archive_fs_list* list,
                                         const struct bx_tar_update_record_list* records) {
    size_t read_index;
    size_t write_index = 0u;

    for (read_index = 0u; read_index < list->len; read_index++) {
        const struct bx_tar_update_record* record = bx_tar_update_record_list_lookup(
            records,
            list->entries[read_index].archive_path
        );
        bool keep = record == NULL
            || bx_tar_timespec_compare(record->mtime, list->entries[read_index].st.st_mtim) < 0;

        if (!keep) {
            free(list->entries[read_index].source_path);
            free(list->entries[read_index].archive_path);
            free(list->entries[read_index].link_target);
            continue;
        }
        if (write_index != read_index) {
            list->entries[write_index] = list->entries[read_index];
        }
        write_index++;
    }
    list->len = write_index;
}

static bool bx_tar_write_catenate_sources_body(const struct bx_tar_rewrite_stream_ctx* ctx,
                                               struct bx_tar_rewrite_visit_state* state,
                                               struct bx_diag_ctx* diag) {
    size_t i;

    if (ctx->source_archives == NULL) {
        return true;
    }

    for (i = 0u; i < ctx->source_archives->len; i++) {
        struct bx_tar_reader_stream_options reader_options = {
            .archive_path = ctx->source_archives->items[i],
            .required_codec = bx_tar_codec_from_suffix(ctx->source_archives->items[i]),
        };

        if (bx_tar_source_archive_is_unsupported_compressed(reader_options.archive_path, diag)) {
            return false;
        }
        if (!bx_tar_rewrite_stream_visit_reader(ctx, state, &reader_options, diag)) {
            return false;
        }
    }
    return true;
}

static bool bx_tar_write_rewrite_stream_body(const struct bx_tar_rewrite_stream_ctx* ctx,
                                             const struct bx_tar_stream_sink* sink,
                                             uint64_t* total_bytes_written_out,
                                             struct bx_diag_ctx* diag) {
    struct bx_tar_rewrite_visit_state state;

    memset(&state, 0, sizeof(state));
    *total_bytes_written_out = 0u;
    state.ctx = ctx;
    state.sink = sink;
    state.stream_options = bx_tar_make_stream_options(ctx->options);
    state.counting_user.inner = sink;
    state.counting_user.bytes_written = &state.bytes_written;
    state.counting_sink.user = &state.counting_user;
    state.counting_sink.write = bx_tar_stream_counting_sink_write;
    state.counting_sink.callback_owns_errors = sink->callback_owns_errors;

    if (ctx->reader_options != NULL
        && !bx_tar_rewrite_stream_visit_reader(ctx, &state, ctx->reader_options, diag)) {
        *total_bytes_written_out = state.bytes_written;
        return false;
    }
    if ((ctx->options->mode == BX_TAR_MODE_APPEND || ctx->options->mode == BX_TAR_MODE_UPDATE)
        && !bx_tar_stream_write_fs_list_body(ctx->appended_files,
                                             &state.stream_options,
                                             sink,
                                             &state.bytes_written,
                                             diag)) {
        *total_bytes_written_out = state.bytes_written;
        return false;
    }
    if (ctx->options->mode == BX_TAR_MODE_CATENATE
        && !bx_tar_write_catenate_sources_body(ctx, &state, diag)) {
        *total_bytes_written_out = state.bytes_written;
        return false;
    }
    if (!bx_tar_stream_write_trailer(sink, state.bytes_written, diag)) {
        *total_bytes_written_out = state.bytes_written;
        return false;
    }
    *total_bytes_written_out = bx_tar_total_archive_size_from_body(state.bytes_written);
    if (ctx->delete_plan != NULL
        && bx_tar_select_plan_report_unmatched(ctx->delete_plan, ctx->matched_members, diag)) {
        if (ctx->had_selection_errors != NULL) {
            *ctx->had_selection_errors = true;
        }
    }
    return true;
}

static bool bx_tar_rewrite_stream_produce(void* user,
                                          const struct bx_archive_codec_stream_sink* sink,
                                          struct bx_diag_ctx* diag) {
    struct bx_tar_rewrite_stream_ctx* ctx = user;
    struct bx_tar_codec_stream_sink_adapter adapter = {
        .sink = sink,
    };
    struct bx_tar_stream_sink tar_sink = {
        .user = &adapter,
        .write = bx_tar_codec_stream_sink_write,
        .callback_owns_errors = true,
    };

    return bx_tar_write_rewrite_stream_body(ctx, &tar_sink, &ctx->total_bytes_written, diag);
}

static bool bx_tar_write_rewrite_archive_direct(const struct bx_tar_rewrite_stream_ctx* ctx,
                                                const struct bx_tar_options* options,
                                                uint64_t* total_bytes_written_out,
                                                struct bx_diag_ctx* diag) {
    const struct bx_archive_codec* codec = bx_tar_output_codec(options);
    struct bx_archive_codec_stream_sink sink = {
        .user = NULL,
        .write = bx_tar_file_sink_write,
    };
    struct bx_archive_output_file output = {0};
    struct bx_tar_rewrite_stream_ctx producer_ctx = *ctx;
    bool ok;

    if (!bx_archive_output_file_open(&output, options->archive_path, diag)) {
        return false;
    }
    sink.user = output.stream;
    if (codec == bx_archive_codec_none()) {
        struct bx_tar_stream_sink tar_sink = {
            .user = output.stream,
            .write = bx_tar_file_sink_write,
        };

        ok = bx_tar_write_rewrite_stream_body(ctx, &tar_sink, total_bytes_written_out, diag);
    }
    else {
        producer_ctx.total_bytes_written = 0u;
        ok = bx_archive_codec_run_encode_stream(codec,
                                                bx_tar_rewrite_stream_produce,
                                                &producer_ctx,
                                                &sink,
                                                diag);
        *total_bytes_written_out = producer_ctx.total_bytes_written;
    }
    if (ok && !bx_archive_output_file_finish(&output, diag)) {
        ok = false;
    }
    if (!ok) {
        bx_archive_output_file_discard(&output);
    }
    return ok;
}

static bool bx_tar_write_rewrite_archive_mt_direct(const struct bx_tar_rewrite_stream_ctx* ctx,
                                                   const struct bx_tar_options* options,
                                                   size_t compress_threads,
                                                   uint64_t* total_bytes_written_out,
                                                   struct bx_diag_ctx* diag) {
    struct bx_tar_rewrite_stream_ctx producer_ctx = *ctx;
    struct bx_archive_codec_stream_sink sink = {
        .user = NULL,
        .write = bx_tar_file_sink_write,
    };
    struct bx_archive_output_file output = {0};
    const struct bx_archive_codec* codec = bx_tar_output_codec(options);
    size_t chunk_size = options->mt_chunk_size != 0u ? (size_t)options->mt_chunk_size : (1u << 20);
    size_t max_inflight = compress_threads > (SIZE_MAX / 4u) ? compress_threads : compress_threads * 4u;
    bool ok;

    if (!bx_archive_output_file_open(&output, options->archive_path, diag)) {
        return false;
    }
    sink.user = output.stream;
    producer_ctx.total_bytes_written = 0u;
    ok = bx_archive_codec_run_encode_mt_stream(codec,
                                               bx_tar_rewrite_stream_produce,
                                               &producer_ctx,
                                               &sink,
                                               &(struct bx_archive_codec_mt_options){
                                                   .thread_count = compress_threads,
                                                   .chunk_size = chunk_size,
                                                   .max_inflight_chunks = max_inflight,
                                               },
                                               diag);
    *total_bytes_written_out = producer_ctx.total_bytes_written;
    if (ok && !bx_archive_output_file_finish(&output, diag)) {
        ok = false;
    }
    if (!ok) {
        bx_archive_output_file_discard(&output);
    }
    return ok;
}

static int bx_tar_try_append_plain_in_place(const struct bx_archive_fs_list* appended_files,
                                            const struct bx_tar_options* options,
                                            uint64_t* total_bytes_written_out,
                                            struct bx_diag_ctx* diag) {
    struct stat st;
    int fd = -1;

    if (bx_tar_output_is_compressed(options) || strcmp(options->archive_path, "-") == 0) {
        return -1;
    }

    *total_bytes_written_out = 0u;

    fd = open(options->archive_path, O_RDWR);
    if (fd < 0) {
        if (errno == ENOENT) {
            return bx_tar_write_create_archive_direct(appended_files,
                                                      options,
                                                      total_bytes_written_out,
                                                      diag)
                ? 1
                : 0;
        }
        bx_diag(diag, "%s: %s", options->archive_path, strerror(errno));
        return 0;
    }
    if (fstat(fd, &st) != 0) {
        bx_diag(diag, "%s: %s", options->archive_path, strerror(errno));
        close(fd);
        return 0;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        return -1;
    }
    close(fd);
    return -1;
}

static bool bx_tar_append_target_is_existing_regular_file(const char* archive_path) {
    struct stat st;

    if (archive_path == NULL || strcmp(archive_path, "-") == 0) {
        return false;
    }
    if (stat(archive_path, &st) != 0) {
        return false;
    }
    return S_ISREG(st.st_mode);
}

static bool bx_tar_archive_path_exists(const char* archive_path) {
    struct stat st;

    if (archive_path == NULL || strcmp(archive_path, "-") == 0) {
        return false;
    }
    return stat(archive_path, &st) == 0;
}

static int bx_tar_catenate_archive(const struct bx_tar_options* options,
                                   struct bx_diag_ctx* diag) {
    struct bx_tar_reader_stream_options reader_options = {
        .archive_path = NULL,
        .required_codec = bx_tar_input_required_codec(options),
    };
    struct bx_tar_rewrite_stream_ctx rewrite_ctx = {
        .reader_options = NULL,
        .delete_plan = NULL,
        .matched_members = NULL,
        .had_selection_errors = NULL,
        .appended_files = NULL,
        .source_archives = &options->source_archives,
        .options = options,
    };
    char* snapshot_path = NULL;
    uint64_t total_bytes_written = 0u;
    int rc = 2;

    if (bx_tar_archive_path_exists(options->archive_path)) {
        if (!bx_archive_snapshot_input_path(options->archive_path, &snapshot_path, diag)) {
            goto out;
        }
        reader_options.archive_path = snapshot_path;
        rewrite_ctx.reader_options = &reader_options;
    }

    {
        size_t compress_threads = bx_tar_effective_compress_threads(options);
        bool use_mt = compress_threads > 1u
            && bx_archive_codec_supports_mt_encode(bx_tar_output_codec(options));

        if (!(use_mt
                  ? bx_tar_write_rewrite_archive_mt_direct(&rewrite_ctx,
                                                           options,
                                                           compress_threads,
                                                           &total_bytes_written,
                                                           diag)
                  : bx_tar_write_rewrite_archive_direct(&rewrite_ctx,
                                                        options,
                                                        &total_bytes_written,
                                                        diag))) {
            goto out;
        }
    }

    rc = 0;
    if (options->report_totals && !bx_tar_report_totals_line(true, total_bytes_written, diag)) {
        rc = 2;
    }
out:
    if (snapshot_path != NULL) {
        unlink(snapshot_path);
        free(snapshot_path);
    }
    return rc;
}

static int bx_tar_update_archive(const struct bx_tar_options* options,
                                 struct bx_diag_ctx* diag) {
    struct bx_archive_fs_list appended_files = {0};
    struct bx_tar_update_record_list archived_mtimes = {0};
    struct bx_tar_reader_stream_options reader_options = {
        .archive_path = NULL,
        .required_codec = bx_tar_input_required_codec(options),
    };
    struct bx_tar_rewrite_stream_ctx rewrite_ctx = {
        .reader_options = NULL,
        .delete_plan = NULL,
        .matched_members = NULL,
        .had_selection_errors = NULL,
        .appended_files = &appended_files,
        .source_archives = NULL,
        .options = options,
    };
    char* snapshot_path = NULL;
    bool had_update_errors = false;
    bool had_postwrite_errors = false;
    bool target_exists = bx_tar_archive_path_exists(options->archive_path);
    uint64_t total_bytes_written = 0u;
    int rc = 2;

    if (target_exists) {
        if (!bx_archive_snapshot_input_path(options->archive_path, &snapshot_path, diag)) {
            goto out;
        }
        reader_options.archive_path = snapshot_path;
        rewrite_ctx.reader_options = &reader_options;
        if (!bx_tar_collect_archived_mtimes(&reader_options, &archived_mtimes, diag)) {
            goto out;
        }
    }

    if (!bx_tar_create_collect_fs_entries(&appended_files,
                                          &options->create_options,
                                          options->sort_name,
                                          &had_update_errors,
                                          diag)) {
        goto out;
    }
    if (options->newer_active) {
        bx_tar_filter_newer_entries(&appended_files, options->newer_time, options->newer_use_ctime);
    }
    bx_tar_filter_update_entries(&appended_files, &archived_mtimes);

    if (had_update_errors && bx_tar_append_target_is_existing_regular_file(options->archive_path)) {
        bx_tar_report_previous_errors(diag);
        rc = 2;
        goto out;
    }

    if (!target_exists) {
        size_t compress_threads = bx_tar_effective_compress_threads(options);
        bool use_mt = compress_threads > 1u
            && bx_archive_codec_supports_mt_encode(bx_tar_output_codec(options));

        rc = (use_mt
                  ? bx_tar_write_create_archive_mt_direct(&appended_files,
                                                          options,
                                                          compress_threads,
                                                          &total_bytes_written,
                                                          diag)
                  : bx_tar_write_create_archive_direct(&appended_files,
                                                       options,
                                                       &total_bytes_written,
                                                       diag))
            ? 0
            : 2;
        goto postwrite;
    }

    {
        size_t compress_threads = bx_tar_effective_compress_threads(options);
        bool use_mt = compress_threads > 1u
            && bx_archive_codec_supports_mt_encode(bx_tar_output_codec(options));

        if (!(use_mt
                  ? bx_tar_write_rewrite_archive_mt_direct(&rewrite_ctx,
                                                           options,
                                                           compress_threads,
                                                           &total_bytes_written,
                                                           diag)
                  : bx_tar_write_rewrite_archive_direct(&rewrite_ctx,
                                                        options,
                                                        &total_bytes_written,
                                                        diag))) {
            goto out;
        }
    }
    rc = 0;

postwrite:
    if (options->report_totals
        && total_bytes_written > 0u
        && !bx_tar_report_totals_line(true, total_bytes_written, diag)) {
        rc = 2;
    }
    if (rc == 0 && options->create_options.remove_files
        && !bx_tar_create_remove_archived_sources(&appended_files, diag)) {
        had_postwrite_errors = true;
    }
    if (rc == 0 && had_update_errors) {
        had_postwrite_errors = true;
    }
    if (rc == 0 && had_postwrite_errors) {
        bx_tar_report_previous_errors(diag);
        rc = 2;
    }

out:
    bx_tar_update_record_list_free(&archived_mtimes);
    if (snapshot_path != NULL) {
        unlink(snapshot_path);
        free(snapshot_path);
    }
    bx_archive_fs_list_free(&appended_files);
    return rc;
}

static bool bx_tar_parse_touch_like_time_arg(const char* text, struct timespec* out) {
    size_t len;
    const char* seconds_text;
    size_t digits_len;
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second = 0;

    if (text == NULL || *text == '\0') {
        return false;
    }

    seconds_text = strchr(text, '.');
    len = strlen(text);
    digits_len = seconds_text == NULL ? len : (size_t)(seconds_text - text);
    if (seconds_text != NULL) {
        if (strlen(seconds_text) != 3u) {
            return false;
        }
        if (!bx_time_parse_fixed_width_int(seconds_text, 1u, 2u, &second)) {
            return false;
        }
    }

    if (digits_len == 8u) {
        if (!bx_time_current_local_year(&year)
            || !bx_time_parse_fixed_width_int(text, 0u, 2u, &month)
            || !bx_time_parse_fixed_width_int(text, 2u, 2u, &day)
            || !bx_time_parse_fixed_width_int(text, 4u, 2u, &hour)
            || !bx_time_parse_fixed_width_int(text, 6u, 2u, &minute)) {
            return false;
        }
    }
    else if (digits_len == 10u) {
        int short_year;

        if (!bx_time_parse_fixed_width_int(text, 0u, 2u, &short_year)
            || !bx_time_parse_fixed_width_int(text, 2u, 2u, &month)
            || !bx_time_parse_fixed_width_int(text, 4u, 2u, &day)
            || !bx_time_parse_fixed_width_int(text, 6u, 2u, &hour)
            || !bx_time_parse_fixed_width_int(text, 8u, 2u, &minute)) {
            return false;
        }
        year = short_year >= 69 ? 1900 + short_year : 2000 + short_year;
    }
    else if (digits_len == 12u) {
        if (!bx_time_parse_fixed_width_int(text, 0u, 4u, &year)
            || !bx_time_parse_fixed_width_int(text, 4u, 2u, &month)
            || !bx_time_parse_fixed_width_int(text, 6u, 2u, &day)
            || !bx_time_parse_fixed_width_int(text, 8u, 2u, &hour)
            || !bx_time_parse_fixed_width_int(text, 10u, 2u, &minute)) {
            return false;
        }
    }
    else {
        return false;
    }

    return bx_time_build_local_timestamp(year, month, day, hour, minute, second, 0, out);
}

static int bx_tar_rewrite_archive(const struct bx_tar_options* options,
                                  struct bx_diag_ctx* diag) {
    struct bx_archive_fs_list appended_files = {0};
    struct bx_tar_reader_stream_options reader_options = {
        .archive_path = NULL,
        .required_codec = bx_tar_input_required_codec(options),
    };
    struct bx_tar_rewrite_stream_ctx rewrite_ctx = {
        .reader_options = &reader_options,
        .delete_plan = NULL,
        .matched_members = NULL,
        .had_selection_errors = NULL,
        .appended_files = &appended_files,
        .options = options,
    };
    struct bx_tar_select_plan select_plan = {0};
    char* snapshot_path = NULL;
    bool* matched_members = NULL;
    bool had_append_errors = false;
    bool had_postwrite_errors = false;
    bool had_selection_errors = false;
    uint64_t total_bytes_written = 0u;
    int rc = 2;
    int append_fast_rc = -1;

    if (options->mode == BX_TAR_MODE_DELETE
        && !bx_tar_select_plan_build(&select_plan,
                                     &options->create_options,
                                     &had_selection_errors,
                                     diag)) {
        return 2;
    }

    if (options->mode == BX_TAR_MODE_APPEND) {
        if (!bx_tar_create_collect_fs_entries(&appended_files,
                                              &options->create_options,
                                              options->sort_name,
                                              &had_append_errors,
                                              diag)) {
            goto out;
        }
        if (options->newer_active) {
            bx_tar_filter_newer_entries(&appended_files, options->newer_time, options->newer_use_ctime);
        }
        if (had_append_errors && bx_tar_append_target_is_existing_regular_file(options->archive_path)) {
            bx_tar_report_previous_errors(diag);
            rc = 2;
            goto out;
        }
        append_fast_rc = bx_tar_try_append_plain_in_place(&appended_files,
                                                          options,
                                                          &total_bytes_written,
                                                          diag);
        if (append_fast_rc >= 0) {
            rc = append_fast_rc == 1 ? 0 : 2;
            goto postwrite;
        }
    }

    if (!bx_archive_snapshot_input_path(options->archive_path, &snapshot_path, diag)) {
        goto out;
    }
    reader_options.archive_path = snapshot_path;
    rewrite_ctx.delete_plan = options->mode == BX_TAR_MODE_DELETE ? &select_plan : NULL;
    rewrite_ctx.had_selection_errors = &had_selection_errors;

    if (options->mode == BX_TAR_MODE_DELETE) {
        matched_members = bx_tar_alloc_matched_members(&select_plan);
        rewrite_ctx.matched_members = matched_members;
    }
    {
        size_t compress_threads = bx_tar_effective_compress_threads(options);
        bool use_mt = compress_threads > 1u
            && bx_archive_codec_supports_mt_encode(bx_tar_output_codec(options));

        if (!(use_mt
                  ? bx_tar_write_rewrite_archive_mt_direct(&rewrite_ctx,
                                                           options,
                                                           compress_threads,
                                                           &total_bytes_written,
                                                           diag)
                  : bx_tar_write_rewrite_archive_direct(&rewrite_ctx,
                                                        options,
                                                        &total_bytes_written,
                                                        diag))) {
            goto out;
        }
    }
    rc = 0;
postwrite:
    if (options->report_totals
        && total_bytes_written > 0u
        && !bx_tar_report_totals_line(true, total_bytes_written, diag)) {
        rc = 2;
    }
    if (options->mode == BX_TAR_MODE_APPEND && had_append_errors) {
        had_postwrite_errors = true;
    }
    if (options->mode == BX_TAR_MODE_APPEND
        && options->create_options.remove_files
        && !bx_tar_create_remove_archived_sources(&appended_files, diag)) {
        had_postwrite_errors = true;
    }
    if (rc == 0 && options->mode == BX_TAR_MODE_DELETE && had_selection_errors) {
        had_postwrite_errors = true;
    }
    if (rc == 0 && had_postwrite_errors) {
        bx_tar_report_previous_errors(diag);
        rc = 2;
    }
out:
    free(matched_members);
    if (snapshot_path != NULL) {
        unlink(snapshot_path);
        free(snapshot_path);
    }
    bx_tar_select_plan_cleanup(&select_plan);
    bx_archive_fs_list_free(&appended_files);
    return rc;
}

static bool bx_tar_parse_time_arg(const char* text, struct timespec* out) {
    struct bx_time_epoch_parse_options epoch_options = {
        .allow_trailing_space = false,
        .normalize_negative_fraction = true,
    };

    if (bx_time_parse_epoch_literal(text, &epoch_options, out)) {
        return true;
    }
    return bx_tar_parse_touch_like_time_arg(text, out);
}

static bool bx_tar_warning_keyword_supported(const char* text) {
    return strcmp(text, "decompress-program") == 0
        || strcmp(text, "no-decompress-program") == 0;
}

static const struct bx_tar_long_option_spec* bx_tar_find_long_option(const char* arg, size_t name_len) {
    size_t i;
    for (i = 0u; bx_tar_long_options[i].name != NULL; i++) {
        if (strlen(bx_tar_long_options[i].name) == name_len
            && strncmp(arg, bx_tar_long_options[i].name, name_len) == 0) {
            return &bx_tar_long_options[i];
        }
    }
    return NULL;
}

static const struct bx_tar_short_option_spec* bx_tar_find_short_option(char ch) {
    size_t i;
    for (i = 0u; bx_tar_short_options[i].name != '\0'; i++) {
        if (bx_tar_short_options[i].name == ch) {
            return &bx_tar_short_options[i];
        }
    }
    return NULL;
}

static bool bx_tar_create_has_inputs(const struct bx_tar_options* options,
                                     int argc) {
    (void)argc;
    return bx_tar_create_options_has_inputs(&options->create_options);
}

static bool bx_tar_report_missing_mode(const struct bx_diag_ctx* diag) {
    fprintf(stderr,
            "%s: You must specify one of the '-Acdtrux', '--delete' or '--test-label' options\n",
            diag->progname);
    fprintf(stderr,
            "Try '%s --help' or '%s --usage' for more information.\n",
            diag->progname,
            diag->progname);
    return false;
}

static bool bx_tar_report_mode_conflict(const struct bx_diag_ctx* diag) {
    fprintf(stderr,
            "%s: You may not specify more than one '-Acdtrux', '--delete' or  '--test-label' option\n",
            diag->progname);
    fprintf(stderr,
            "Try '%s --help' or '%s --usage' for more information.\n",
            diag->progname,
            diag->progname);
    return false;
}

static bool bx_tar_set_mode_option(struct bx_tar_options* options,
                                   enum bx_tar_mode mode,
                                   const char* unsupported_display,
                                   struct bx_diag_ctx* diag) {
    if (options->saw_mode_option) {
        return bx_tar_report_mode_conflict(diag);
    }

    options->saw_mode_option = true;
    options->mode = mode;
    options->unsupported_mode = unsupported_display;
    return true;
}

static bool bx_tar_apply_option_effect(struct bx_tar_options* options,
                                       enum bx_tar_option_effect effect,
                                       const char* display,
                                       const char* value,
                                       struct bx_diag_ctx* diag) {
    switch (effect) {
        case BX_TAR_OPT_NOOP:
            return true;
        case BX_TAR_OPT_MODE_CATENATE:
            return bx_tar_set_mode_option(options, BX_TAR_MODE_CATENATE, NULL, diag);
        case BX_TAR_OPT_MODE_CREATE:
            return bx_tar_set_mode_option(options, BX_TAR_MODE_CREATE, NULL, diag);
        case BX_TAR_OPT_MODE_COMPARE:
            return bx_tar_set_mode_option(options, BX_TAR_MODE_COMPARE, NULL, diag);
        case BX_TAR_OPT_MODE_TEST_LABEL:
            return bx_tar_set_mode_option(options, BX_TAR_MODE_TEST_LABEL, NULL, diag);
        case BX_TAR_OPT_MODE_LIST:
            return bx_tar_set_mode_option(options, BX_TAR_MODE_LIST, NULL, diag);
        case BX_TAR_OPT_MODE_EXTRACT:
            return bx_tar_set_mode_option(options, BX_TAR_MODE_EXTRACT, NULL, diag);
        case BX_TAR_OPT_MODE_APPEND:
            return bx_tar_set_mode_option(options, BX_TAR_MODE_APPEND, NULL, diag);
        case BX_TAR_OPT_MODE_UPDATE:
            return bx_tar_set_mode_option(options, BX_TAR_MODE_UPDATE, NULL, diag);
        case BX_TAR_OPT_MODE_DELETE:
            return bx_tar_set_mode_option(options, BX_TAR_MODE_DELETE, NULL, diag);
        case BX_TAR_OPT_MODE_UNSUPPORTED:
            return bx_tar_set_mode_option(options, BX_TAR_MODE_NONE, display, diag);
        case BX_TAR_OPT_ARCHIVE_PATH:
            options->archive_path = value;
            return true;
        case BX_TAR_OPT_DIRECTORY:
            return bx_tar_create_options_add_chdir(&options->create_options, value);
        case BX_TAR_OPT_TO_STDOUT:
            options->to_stdout = true;
            return true;
        case BX_TAR_OPT_KEEP_OLD_FILES:
            options->keep_old_files = true;
            return true;
        case BX_TAR_OPT_OVERWRITE:
            options->overwrite = true;
            return true;
        case BX_TAR_OPT_UNLINK_FIRST:
            options->unlink_first = true;
            return true;
        case BX_TAR_OPT_RECURSIVE_UNLINK:
            options->recursive_unlink = true;
            return true;
        case BX_TAR_OPT_INDEX_FILE:
            free(options->index_file_path);
            options->index_file_path = xstrdup(value);
            return true;
        case BX_TAR_OPT_VERBOSE:
            options->verbose_reports = true;
            return true;
        case BX_TAR_OPT_REPORT_MAPPED_NAMES:
            options->report_mapped_names = true;
            return true;
        case BX_TAR_OPT_BLOCK_NUMBER:
            options->report_block_numbers = true;
            return true;
        case BX_TAR_OPT_TOTALS:
            options->report_totals = true;
            return true;
        case BX_TAR_OPT_EXCLUDE:
            return bx_tar_create_options_add_exclude_pattern(&options->create_options, value);
        case BX_TAR_OPT_EXCLUDE_FROM:
            return bx_tar_create_options_add_exclude_from(&options->create_options, value);
        case BX_TAR_OPT_ADD_FILE:
            return bx_tar_create_options_add_add_file(&options->create_options, value);
        case BX_TAR_OPT_FILES_FROM:
            return bx_tar_create_options_add_files_from(&options->create_options, value);
        case BX_TAR_OPT_FILES_FROM_NULL_ON:
            return bx_tar_create_options_set_files_from_null(&options->create_options, true);
        case BX_TAR_OPT_FILES_FROM_NULL_OFF:
            return bx_tar_create_options_set_files_from_null(&options->create_options, false);
        case BX_TAR_OPT_FILES_FROM_VERBATIM_ON:
            return bx_tar_create_options_set_files_from_verbatim(&options->create_options, true);
        case BX_TAR_OPT_FILES_FROM_VERBATIM_OFF:
            return bx_tar_create_options_set_files_from_verbatim(&options->create_options, false);
        case BX_TAR_OPT_UNQUOTE_ON:
            return bx_tar_create_options_set_files_from_unquote(&options->create_options, true);
        case BX_TAR_OPT_UNQUOTE_OFF:
            return bx_tar_create_options_set_files_from_unquote(&options->create_options, false);
        case BX_TAR_OPT_NO_RECURSION:
            return bx_tar_create_options_set_recurse(&options->create_options, false);
        case BX_TAR_OPT_RECURSION:
            return bx_tar_create_options_set_recurse(&options->create_options, true);
        case BX_TAR_OPT_ANCHORED_ON:
            return bx_tar_create_options_set_anchored(&options->create_options, true);
        case BX_TAR_OPT_ANCHORED_OFF:
            return bx_tar_create_options_set_anchored(&options->create_options, false);
        case BX_TAR_OPT_IGNORE_CASE_ON:
            return bx_tar_create_options_set_ignore_case(&options->create_options, true);
        case BX_TAR_OPT_IGNORE_CASE_OFF:
            return bx_tar_create_options_set_ignore_case(&options->create_options, false);
        case BX_TAR_OPT_WILDCARDS_ON:
            return bx_tar_create_options_set_wildcards(&options->create_options, true);
        case BX_TAR_OPT_WILDCARDS_OFF:
            return bx_tar_create_options_set_wildcards(&options->create_options, false);
        case BX_TAR_OPT_WILDCARDS_MATCH_SLASH_ON:
            return bx_tar_create_options_set_wildcards_match_slash(&options->create_options, true);
        case BX_TAR_OPT_WILDCARDS_MATCH_SLASH_OFF:
            return bx_tar_create_options_set_wildcards_match_slash(&options->create_options, false);
        case BX_TAR_OPT_EXCLUDE_CACHES:
            return bx_tar_create_options_set_exclude_caches(&options->create_options);
        case BX_TAR_OPT_EXCLUDE_CACHES_ALL:
            return bx_tar_create_options_set_exclude_caches_all(&options->create_options);
        case BX_TAR_OPT_EXCLUDE_CACHES_UNDER:
            return bx_tar_create_options_set_exclude_caches_under(&options->create_options);
        case BX_TAR_OPT_EXCLUDE_IGNORE:
            return bx_tar_create_options_add_exclude_ignore(&options->create_options, value);
        case BX_TAR_OPT_EXCLUDE_IGNORE_RECURSIVE:
            return bx_tar_create_options_add_exclude_ignore_recursive(&options->create_options, value);
        case BX_TAR_OPT_EXCLUDE_TAG:
            return bx_tar_create_options_add_exclude_tag(&options->create_options, value);
        case BX_TAR_OPT_EXCLUDE_TAG_ALL:
            return bx_tar_create_options_add_exclude_tag_all(&options->create_options, value);
        case BX_TAR_OPT_EXCLUDE_TAG_UNDER:
            return bx_tar_create_options_add_exclude_tag_under(&options->create_options, value);
        case BX_TAR_OPT_EXCLUDE_VCS:
            return bx_tar_create_options_set_exclude_vcs(&options->create_options);
        case BX_TAR_OPT_EXCLUDE_VCS_IGNORES:
            return bx_tar_create_options_set_exclude_vcs_ignores(&options->create_options);
        case BX_TAR_OPT_REMOVE_FILES:
            options->create_options.remove_files = true;
            return true;
        case BX_TAR_OPT_THREADS:
            return bx_thread_count_parse(diag->progname, "--threads", value, &options->threads);
        case BX_TAR_OPT_COMPRESS_THREADS:
            return bx_thread_count_parse(diag->progname, "--compress-threads", value, &options->compress_threads);
        case BX_TAR_OPT_MT_CHUNK_SIZE: {
            uintmax_t parsed = 0u;
            if (!bx_size_parse_block_size(value, &parsed) || parsed == 0u || parsed > SIZE_MAX) {
                bx_diag(diag, "invalid chunk size '%s'", value);
                return false;
            }
            options->mt_chunk_size = parsed;
            return true;
        }
        case BX_TAR_OPT_NO_MT:
            options->no_mt = true;
            return true;
        case BX_TAR_OPT_BZIP2_ON:
            bx_tar_set_codec_option(options, bx_archive_codec_bzip2());
            return true;
        case BX_TAR_OPT_GZIP_ON:
            bx_tar_set_codec_option(options, bx_archive_codec_gzip());
            return true;
        case BX_TAR_OPT_XZ_ON:
            bx_tar_set_codec_option(options, bx_archive_codec_xz());
            return true;
        case BX_TAR_OPT_ZSTD_ON:
            bx_tar_set_codec_option(options, bx_archive_codec_zstd());
            return true;
        case BX_TAR_OPT_EXTERNAL_COMPRESS_PROGRAM:
            return bx_tar_apply_external_compress_program(options, display, value);
        case BX_TAR_OPT_AUTO_COMPRESS_ON:
            options->auto_compress = true;
            return true;
        case BX_TAR_OPT_AUTO_COMPRESS_OFF:
            options->auto_compress = false;
            return true;
        case BX_TAR_OPT_ABSOLUTE_NAMES_ON:
            options->absolute_names = true;
            return true;
        case BX_TAR_OPT_TOUCH_MTIME_ON:
            options->touch_mtime = true;
            return true;
        case BX_TAR_OPT_NUMERIC_OWNER:
            options->numeric_owner = true;
            return true;
        case BX_TAR_OPT_STARTING_FILE:
            options->starting_file = value;
            return true;
        case BX_TAR_OPT_NEWER:
            if (!bx_tar_parse_time_arg(value, &options->newer_time)) {
                bx_diag(diag, "unsupported time '%s'", value);
                return false;
            }
            options->newer_active = true;
            options->newer_use_ctime = true;
            return true;
        case BX_TAR_OPT_NEWER_MTIME:
            if (!bx_tar_parse_time_arg(value, &options->newer_time)) {
                bx_diag(diag, "unsupported time '%s'", value);
                return false;
            }
            options->newer_active = true;
            options->newer_use_ctime = false;
            return true;
        case BX_TAR_OPT_STRIP_COMPONENTS: {
            char* end = NULL;
            unsigned long parsed = strtoul(value, &end, 10);
            if (value[0] == '\0' || end == NULL || *end != '\0') {
                bx_diag(diag, "invalid number of components '%s'", value);
                return false;
            }
            options->strip_components = (size_t)parsed;
            return true;
        }
        case BX_TAR_OPT_ONE_TOP_LEVEL:
            options->one_top_level = value;
            return true;
        case BX_TAR_OPT_TRANSFORM:
            return bx_tar_transform_rule_init(&options->name_transform, value, diag);
        case BX_TAR_OPT_FORMAT:
            if (strcmp(value, "ustar") != 0) {
                bx_diag(diag, "unsupported format '%s'", value);
                return false;
            }
            options->format_ustar = true;
            return true;
        case BX_TAR_OPT_SORT:
            if (strcmp(value, "name") != 0) {
                bx_diag(diag, "unsupported sort order '%s'", value);
                return false;
            }
            options->sort_name = true;
            return true;
        case BX_TAR_OPT_MTIME:
            if (!bx_tar_parse_time_arg(value, &options->mtime)) {
                bx_diag(diag, "unsupported time '%s'", value);
                return false;
            }
            options->fixed_mtime = true;
            return true;
        case BX_TAR_OPT_MODE:
            if (!bx_tar_validate_mode_arg(value, diag)) {
                return false;
            }
            free(options->mode_text);
            options->mode_text = xstrdup(value);
            return true;
        case BX_TAR_OPT_OWNER:
            options->owner = (uid_t)strtoul(value, NULL, 10);
            options->owner_set = true;
            return true;
        case BX_TAR_OPT_GROUP:
            options->group = (gid_t)strtoul(value, NULL, 10);
            options->group_set = true;
            return true;
        case BX_TAR_OPT_GROUP_MAP:
            return bx_tar_id_map_load_group(&options->group_map, value, diag);
        case BX_TAR_OPT_OWNER_MAP:
            return bx_tar_id_map_load_owner(&options->owner_map, value, diag);
        case BX_TAR_OPT_XATTRS_ON:
            options->xattrs = true;
            return true;
        case BX_TAR_OPT_XATTRS_OFF:
            options->xattrs = false;
            return true;
        case BX_TAR_OPT_ACLS_ON:
            options->acls = true;
            return true;
        case BX_TAR_OPT_ACLS_OFF:
            options->acls = false;
            return true;
        case BX_TAR_OPT_WARNING:
            if (!bx_tar_warning_keyword_supported(value)) {
                bx_diag(diag, "invalid argument '%s' for '--warning'", value);
                return false;
            }
            return true;
    }

    return true;
}

static void bx_tar_options_cleanup(struct bx_tar_options* options) {
    bx_tar_transform_rule_cleanup(&options->name_transform);
    bx_tar_create_options_cleanup(&options->create_options);
    bx_archive_name_list_free(&options->source_archives);
    bx_tar_id_map_cleanup(&options->owner_map);
    bx_tar_id_map_cleanup(&options->group_map);
    free(options->index_file_path);
    options->index_file_path = NULL;
    bx_tar_clear_unsupported_external_compress_program(options);
    free(options->mode_text);
    options->mode_text = NULL;
}

static bool bx_tar_add_operand(struct bx_tar_options* options, const char* operand) {
    if (options->mode == BX_TAR_MODE_CATENATE || options->mode == BX_TAR_MODE_TEST_LABEL) {
        return bx_archive_name_list_append(&options->source_archives, operand);
    }
    return bx_tar_create_options_add_add_file(&options->create_options, operand);
}

static int bx_tar_test_label_archive(const struct bx_tar_options* options,
                                     struct bx_diag_ctx* diag) {
    struct bx_tar_reader_stream_options reader_options = {
        .archive_path = options->archive_path,
        .required_codec = bx_tar_input_required_codec(options),
    };
    struct bx_tar_report_output report_output = {0};
    char* label = NULL;
    int rc = 0;
    size_t i;

    if (!bx_tar_read_volume_label_stream(&reader_options, &label, diag)) {
        free(label);
        return 2;
    }
    if (!bx_tar_report_output_init(&report_output, options->index_file_path, stdout, diag)) {
        free(label);
        return 2;
    }
    if (options->source_archives.len == 0u) {
        if (label != NULL) {
            if (!bx_tar_report_printf(report_output.stream, diag, "%s\n", label)) {
                bx_tar_report_output_cleanup(&report_output);
                free(label);
                return 2;
            }
        }
        if (!bx_tar_report_output_finish(&report_output, diag)) {
            free(label);
            return 2;
        }
        free(label);
        return 0;
    }

    rc = 1;
    for (i = 0u; i < options->source_archives.len; i++) {
        if (label != NULL && strcmp(label, options->source_archives.items[i]) == 0) {
            rc = 0;
            break;
        }
    }
    if (!bx_tar_report_output_finish(&report_output, diag)) {
        free(label);
        return 2;
    }
    free(label);
    return rc;
}

static bool bx_tar_report_fs_entries(FILE* stream,
                                     const struct bx_archive_fs_list* files,
                                     struct bx_diag_ctx* diag) {
    size_t i;

    for (i = 0u; i < files->len; i++) {
        if (!bx_tar_report_member_line(stream,
                                       files->entries[i].archive_path,
                                       S_ISDIR(files->entries[i].st.st_mode),
                                       diag)) {
            return false;
        }
    }
    return true;
}

struct bx_tar_create_stream_producer_ctx {
    const struct bx_tar_create_options* create_options;
    bool sort_children;
    bool had_create_errors;
};

static bool bx_tar_create_stream_entries_produce(void* user,
                                                 bx_archive_fs_visit_fn visit_fn,
                                                 void* visit_user_data,
                                                 struct bx_diag_ctx* diag) {
    struct bx_tar_create_stream_producer_ctx* ctx = user;

    return bx_tar_create_visit_fs_entries(ctx->create_options,
                                          ctx->sort_children,
                                          visit_fn,
                                          visit_user_data,
                                          &ctx->had_create_errors,
                                          diag);
}

static bool bx_tar_can_stream_create(const struct bx_tar_options* options) {
    return !options->verbose_reports
        && !options->create_options.remove_files
        && !options->newer_active;
}

static bool bx_tar_parse_options(struct bx_tar_options* options,
                                 int argc,
                                 char** argv,
                                 struct bx_diag_ctx* diag) {
    int i = 1;
    bool oldstyle = false;

    memset(options, 0, sizeof(*options));
    options->threads = -1;
    options->compress_threads = -1;

    if (i < argc && argv[i][0] != '-' && argv[i][0] != '\0') {
        oldstyle = true;
    }

    while (i < argc) {
        char* arg = argv[i];
        if (!oldstyle && strcmp(arg, "--") == 0) {
            int j;
            for (j = i + 1; j < argc; j++) {
                if (!bx_tar_add_operand(options, argv[j])) {
                    return false;
                }
            }
            break;
        }
        if (!oldstyle && arg[0] != '-') {
            if (!bx_tar_add_operand(options, arg)) {
                return false;
            }
            i++;
            continue;
        }
        if (!oldstyle && strncmp(arg, "--", 2u) == 0) {
            const struct bx_tar_long_option_spec* spec;
            const char* value = strchr(arg, '=');
            const char* parsed_value = NULL;
            size_t name_len = value ? (size_t)(value - arg) : strlen(arg);

            spec = bx_tar_find_long_option(arg, name_len);
            if (spec == NULL) {
                bx_diag(diag, "unrecognized option '%s'", arg);
                return false;
            }

            if (spec->arg_mode == BX_TAR_OPTARG_REQUIRED) {
                if (value == NULL && ++i >= argc) {
                    bx_diag(diag, "option '%s' requires an argument", spec->name);
                    return false;
                }
                parsed_value = value ? value + 1 : argv[i];
            }

            if (!bx_tar_apply_option_effect(options, spec->effect, spec->name, parsed_value, diag)) {
                return false;
            }
            i++;
            continue;
        }

        {
            const char* letters = oldstyle ? arg : arg + 1;
            size_t j;
            for (j = 0u; letters[j] != '\0'; j++) {
                char ch = letters[j];
                const char* attached = &letters[j + 1u];
                const struct bx_tar_short_option_spec* spec = bx_tar_find_short_option(ch);
                const char* parsed_value = NULL;

                if (spec == NULL) {
                    bx_diag(diag, "invalid option -- '%c'", ch);
                    return false;
                }

                if (spec->arg_mode == BX_TAR_OPTARG_REQUIRED) {
                    if (*attached != '\0') {
                        parsed_value = attached;
                        j = strlen(letters) - 1u;
                    }
                    else if (++i < argc) {
                        parsed_value = argv[i];
                    }
                    else {
                        bx_diag(diag, "option requires an argument -- '%c'", ch);
                        return false;
                    }
                }

                if (!bx_tar_apply_option_effect(options, spec->effect, spec->display, parsed_value, diag)) {
                    return false;
                }

                if (spec->arg_mode == BX_TAR_OPTARG_REQUIRED) {
                    goto next_arg;
                }
            }
        next_arg: ;
        }
        oldstyle = false;
        i++;
    }

    if (options->mode == BX_TAR_MODE_NONE) {
        if (options->unsupported_mode != NULL) {
            bx_diag(diag, "%s is not yet supported", options->unsupported_mode);
            return false;
        }
        return bx_tar_report_missing_mode(diag);
    }
    if (options->unsupported_external_compress_option != NULL) {
        bx_diag(diag,
                "external compression programs are not supported: %s %s",
                options->unsupported_external_compress_option,
                options->unsupported_external_compress_program != NULL
                    ? options->unsupported_external_compress_program
                    : "");
        return false;
    }
    if (options->archive_path == NULL) {
        bx_diag(diag, "archive file not specified; use -f");
        return false;
    }
    if ((options->mode == BX_TAR_MODE_CREATE
            || options->mode == BX_TAR_MODE_APPEND
            || options->mode == BX_TAR_MODE_UPDATE)
        && !bx_tar_create_has_inputs(options, argc)) {
        bx_diag(diag, "missing file operand");
        return false;
    }
    if (options->mode == BX_TAR_MODE_CATENATE && options->source_archives.len == 0u) {
        bx_diag(diag, "missing file operand");
        return false;
    }
    return true;
}

int bx_tar_run(int argc, char** argv) {
    struct bx_tar_options options;
    int rc = 2;
    struct bx_diag_ctx diag = {
        .progname = bx_tar_progname(argv, argc),
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (!bx_tar_parse_options(&options, argc, argv, &diag)) {
        bx_tar_options_cleanup(&options);
        return 2;
    }

    if (options.mode == BX_TAR_MODE_CREATE) {
        if (bx_tar_can_stream_create(&options)) {
            struct bx_tar_create_stream_producer_ctx stream_ctx = {
                .create_options = &options.create_options,
                .sort_children = options.sort_name,
                .had_create_errors = false,
            };
            uint64_t total_bytes_written = 0u;
            size_t compress_threads = bx_tar_effective_compress_threads(&options);
            bool use_mt = compress_threads > 1u
                && bx_archive_codec_supports_mt_encode(bx_tar_output_codec(&options));

            rc = (use_mt
                      ? bx_tar_write_create_archive_stream_mt_direct(bx_tar_create_stream_entries_produce,
                                                                     &stream_ctx,
                                                                     &options,
                                                                     compress_threads,
                                                                     &total_bytes_written,
                                                                     &diag)
                      : bx_tar_write_create_archive_stream_direct(bx_tar_create_stream_entries_produce,
                                                                  &stream_ctx,
                                                                  &options,
                                                                  &total_bytes_written,
                                                                  &diag))
                ? 0
                : 2;
            if (options.report_totals
                && total_bytes_written > 0u
                && !bx_tar_report_totals_line(true, total_bytes_written, &diag)) {
                rc = 2;
            }
            if (rc == 0 && stream_ctx.had_create_errors) {
                bx_tar_report_previous_errors(&diag);
                rc = 2;
            }
            bx_tar_options_cleanup(&options);
            return rc;
        }

        struct bx_archive_fs_list files = {0};
        struct bx_tar_report_output report_output = {0};
        bool had_create_errors = false;
        bool had_postwrite_errors = false;
        uint64_t total_bytes_written = 0u;
        size_t compress_threads = bx_tar_effective_compress_threads(&options);
        bool use_mt = compress_threads > 1u
            && bx_archive_codec_supports_mt_encode(bx_tar_output_codec(&options));

        if (!bx_tar_create_collect_fs_entries(&files,
                                              &options.create_options,
                                              options.sort_name,
                                              &had_create_errors,
                                              &diag)) {
            bx_tar_options_cleanup(&options);
            return 2;
        }
        if (options.newer_active) {
            bx_tar_filter_newer_entries(&files, options.newer_time, options.newer_use_ctime);
        }
        if (options.verbose_reports
            && !bx_tar_report_output_init(&report_output,
                                          options.index_file_path,
                                          stderr,
                                          &diag)) {
            bx_archive_fs_list_free(&files);
            bx_tar_options_cleanup(&options);
            return 2;
        }
        if (options.verbose_reports && !bx_tar_report_fs_entries(report_output.stream, &files, &diag)) {
            bx_tar_report_output_cleanup(&report_output);
            bx_archive_fs_list_free(&files);
            bx_tar_options_cleanup(&options);
            return 2;
        }
        rc = (use_mt
                  ? bx_tar_write_create_archive_mt_direct(&files,
                                                          &options,
                                                          compress_threads,
                                                          &total_bytes_written,
                                                          &diag)
                  : bx_tar_write_create_archive_direct(&files,
                                                       &options,
                                                       &total_bytes_written,
                                                       &diag))
            ? 0
            : 2;
        if (options.report_totals
            && total_bytes_written > 0u
            && !bx_tar_report_totals_line(true, total_bytes_written, &diag)) {
            rc = 2;
        }
        if (rc == 0 && options.create_options.remove_files) {
            if (!bx_tar_create_remove_archived_sources(&files, &diag)) {
                had_postwrite_errors = true;
            }
        }
        if (rc == 0 && had_create_errors) {
            had_postwrite_errors = true;
        }
        if (rc == 0 && had_postwrite_errors) {
            bx_tar_report_previous_errors(&diag);
            rc = 2;
        }
        if (!bx_tar_report_output_finish(&report_output, &diag)) {
            rc = 2;
        }
        bx_archive_fs_list_free(&files);
        bx_tar_options_cleanup(&options);
        return rc;
    }
    if (options.mode == BX_TAR_MODE_CATENATE) {
        rc = bx_tar_catenate_archive(&options, &diag);
        bx_tar_options_cleanup(&options);
        return rc;
    }
    if (options.mode == BX_TAR_MODE_TEST_LABEL) {
        rc = bx_tar_test_label_archive(&options, &diag);
        bx_tar_options_cleanup(&options);
        return rc;
    }
    if (options.mode == BX_TAR_MODE_UPDATE) {
        rc = bx_tar_update_archive(&options, &diag);
        bx_tar_options_cleanup(&options);
        return rc;
    }
    if (options.mode == BX_TAR_MODE_APPEND || options.mode == BX_TAR_MODE_DELETE) {
        rc = bx_tar_rewrite_archive(&options, &diag);
        bx_tar_options_cleanup(&options);
        return rc;
    }
    else {
        struct bx_tar_select_plan select_plan = {0};
        bool had_selection_errors = false;
        if (!bx_tar_select_plan_build(&select_plan,
                                      &options.create_options,
                                      &had_selection_errors,
                                      &diag)) {
            bx_tar_options_cleanup(&options);
            return 2;
        }
        rc = bx_tar_process_archive_stream(&options, &select_plan, &diag);
        if (rc == 0 && had_selection_errors) {
            bx_tar_report_previous_errors(&diag);
            rc = 2;
        }
        bx_tar_select_plan_cleanup(&select_plan);
        bx_tar_options_cleanup(&options);
        return rc;
    }
}
