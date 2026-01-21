#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "applets/archive/archive_common.h"
#include "applets/archive/archive_gzip.h"
#include "applets/archive/archive_fs.h"
#include "applets/archive/tar/tar_backend.h"
#include "applets/archive/tar/tar_create.h"
#include "applets/archive/tar/tar_names.h"
#include "applets/archive/tar/tar_reader.h"
#include "applets/archive/tar/tar_select.h"
#include "applets/archive/tar/tar_stream.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/copy_data.h"
#include "lib/path_ops.h"
#include "lib/size_parse.h"
#include "lib/thread_count.h"
#include "lib/xreadwrite.h"

enum bx_tar_mode {
    BX_TAR_MODE_NONE = 0,
    BX_TAR_MODE_CREATE,
    BX_TAR_MODE_LIST,
    BX_TAR_MODE_EXTRACT,
    BX_TAR_MODE_APPEND,
    BX_TAR_MODE_DELETE,
};

struct bx_tar_options {
    enum bx_tar_mode mode;
    const char* unsupported_mode;
    bool saw_mode_option;
    const char* archive_path;
    bool to_stdout;
    bool keep_old_files;
    bool gzip;
    bool auto_compress;
    bool absolute_names;
    bool touch_mtime;
    bool sort_name;
    bool format_ustar;
    bool owner_set;
    bool group_set;
    uid_t owner;
    gid_t group;
    bool fixed_mtime;
    struct timespec mtime;
    bool xattrs;
    bool acls;
    bool no_mt;
    size_t strip_components;
    int threads;
    int compress_threads;
    uintmax_t mt_chunk_size;
    const char* one_top_level;
    struct bx_tar_transform_rule name_transform;
    struct bx_tar_create_options create_options;
};

enum bx_tar_option_arg_mode {
    BX_TAR_OPTARG_NONE = 0,
    BX_TAR_OPTARG_REQUIRED,
};

enum bx_tar_option_effect {
    BX_TAR_OPT_NOOP = 0,
    BX_TAR_OPT_MODE_CREATE,
    BX_TAR_OPT_MODE_LIST,
    BX_TAR_OPT_MODE_EXTRACT,
    BX_TAR_OPT_MODE_APPEND,
    BX_TAR_OPT_MODE_DELETE,
    BX_TAR_OPT_MODE_UNSUPPORTED,
    BX_TAR_OPT_ARCHIVE_PATH,
    BX_TAR_OPT_DIRECTORY,
    BX_TAR_OPT_TO_STDOUT,
    BX_TAR_OPT_KEEP_OLD_FILES,
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
    BX_TAR_OPT_REMOVE_FILES,
    BX_TAR_OPT_THREADS,
    BX_TAR_OPT_COMPRESS_THREADS,
    BX_TAR_OPT_MT_CHUNK_SIZE,
    BX_TAR_OPT_NO_MT,
    BX_TAR_OPT_GZIP_ON,
    BX_TAR_OPT_AUTO_COMPRESS_ON,
    BX_TAR_OPT_AUTO_COMPRESS_OFF,
    BX_TAR_OPT_ABSOLUTE_NAMES_ON,
    BX_TAR_OPT_TOUCH_MTIME_ON,
    BX_TAR_OPT_STRIP_COMPONENTS,
    BX_TAR_OPT_ONE_TOP_LEVEL,
    BX_TAR_OPT_TRANSFORM,
    BX_TAR_OPT_FORMAT,
    BX_TAR_OPT_SORT,
    BX_TAR_OPT_MTIME,
    BX_TAR_OPT_OWNER,
    BX_TAR_OPT_GROUP,
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
    {"--catenate", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_UNSUPPORTED},
    {"--concatenate", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_UNSUPPORTED},
    {"--create", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_CREATE},
    {"--delete", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_DELETE},
    {"--diff", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_UNSUPPORTED},
    {"--compare", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_UNSUPPORTED},
    {"--append", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_APPEND},
    {"--test-label", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_UNSUPPORTED},
    {"--list", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_LIST},
    {"--update", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_UNSUPPORTED},
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
    {"--exclude-caches", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--exclude-caches-all", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--exclude-caches-under", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--exclude-ignore", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--exclude-ignore-recursive", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--exclude-tag", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--exclude-tag-all", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--exclude-tag-under", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--exclude-vcs", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--exclude-vcs-ignores", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
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
    {"--anchored", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--ignore-case", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--no-anchored", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--no-ignore-case", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--no-wildcards", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--no-wildcards-match-slash", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--wildcards", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--wildcards-match-slash", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--keep-old-files", BX_TAR_OPTARG_NONE, BX_TAR_OPT_KEEP_OLD_FILES},
    {"--keep-directory-symlink", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--keep-newer-files", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--no-overwrite-dir", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--one-top-level", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_ONE_TOP_LEVEL},
    {"--overwrite", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--overwrite-dir", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--recursive-unlink", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--remove-files", BX_TAR_OPTARG_NONE, BX_TAR_OPT_REMOVE_FILES},
    {"--threads", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_THREADS},
    {"--compress-threads", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_COMPRESS_THREADS},
    {"--mt-chunk-size", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_MT_CHUNK_SIZE},
    {"--no-mt", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NO_MT},
    {"--skip-old-files", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--unlink-first", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
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
    {"--numeric-owner", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--owner", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_OWNER},
    {"--group-map", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--owner-map", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--mode", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
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
    {"--use-compress-program", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--bzip2", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--xz", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--lzip", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--lzma", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--lzop", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--zstd", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
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
    {"--starting-file", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--newer-mtime", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--newer", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--after-date", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--one-file-system", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--absolute-names", BX_TAR_OPTARG_NONE, BX_TAR_OPT_ABSOLUTE_NAMES_ON},
    {"--strip-components", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_STRIP_COMPONENTS},
    {"--transform", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_TRANSFORM},
    {"--xform", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_TRANSFORM},
    {"--checkpoint", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--checkpoint-action", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--full-time", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--index-file", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--check-links", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--no-quote-chars", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--quote-chars", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--quoting-style", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--block-number", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--show-defaults", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--show-omitted-dirs", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--show-snapshot-field-ranges", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--show-transformed-names", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--show-stored-names", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--totals", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {"--utc", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--verbose", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--warning", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_WARNING},
    {"--interactive", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--confirmation", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {"--restrict", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {NULL, BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
};

static const struct bx_tar_short_option_spec bx_tar_short_options[] = {
    {'A', "-A", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_UNSUPPORTED},
    {'c', "-c", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_CREATE},
    {'d', "-d", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_UNSUPPORTED},
    {'r', "-r", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_APPEND},
    {'t', "-t", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_LIST},
    {'u', "-u", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_UNSUPPORTED},
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
    {'I', "-I", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {'j', "-j", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {'J', "-J", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {'Z', "-Z", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {'z', "-z", BX_TAR_OPTARG_NONE, BX_TAR_OPT_GZIP_ON},
    {'h', "-h", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {'K', "-K", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {'N', "-N", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NOOP},
    {'l', "-l", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {'P', "-P", BX_TAR_OPTARG_NONE, BX_TAR_OPT_ABSOLUTE_NAMES_ON},
    {'s', "-s", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {'R', "-R", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
    {'v', "-v", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NOOP},
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

static struct bx_tar_stream_options
bx_tar_make_stream_options(const struct bx_tar_options* options) {
    return (struct bx_tar_stream_options){
        .format_ustar = options->format_ustar,
        .owner_set = options->owner_set,
        .group_set = options->group_set,
        .fixed_mtime = options->fixed_mtime,
        .owner = options->owner,
        .group = options->group,
        .mtime = options->mtime,
    };
}

static bool bx_tar_output_uses_gzip(const struct bx_tar_options* options) {
    return options->gzip
        || (options->auto_compress
            && options->archive_path != NULL
            && bx_archive_path_has_gzip_suffix(options->archive_path));
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

static bool bx_tar_file_sink_write(void* user, const void* data, size_t len) {
    FILE* stream = user;
    return fwrite(data, 1u, len, stream) == len;
}

struct bx_tar_gzip_stream_create_ctx {
    const struct bx_archive_fs_list* files;
    const struct bx_tar_options* options;
};

struct bx_tar_gzip_stream_sink_adapter {
    const struct bx_archive_gzip_stream_sink* sink;
};

static bool bx_tar_gzip_stream_sink_write(void* user, const void* data, size_t len) {
    const struct bx_tar_gzip_stream_sink_adapter* adapter = user;
    return adapter->sink->write(adapter->sink->user, data, len);
}

static bool bx_tar_gzip_stream_produce(void* user,
                                       const struct bx_archive_gzip_stream_sink* sink,
                                       struct bx_diag_ctx* diag) {
    const struct bx_tar_gzip_stream_create_ctx* ctx = user;
    struct bx_tar_gzip_stream_sink_adapter adapter = {
        .sink = sink,
    };
    struct bx_tar_stream_options stream_options = bx_tar_make_stream_options(ctx->options);
    struct bx_tar_stream_sink tar_sink = {
        .user = &adapter,
        .write = bx_tar_gzip_stream_sink_write,
        .callback_owns_errors = true,
    };

    return bx_tar_stream_encode_fs_list(ctx->files, &stream_options, &tar_sink, diag);
}

static bool bx_tar_write_create_archive_direct(const struct bx_archive_fs_list* files,
                                               const struct bx_tar_options* options,
                                               struct bx_diag_ctx* diag) {
    struct bx_tar_stream_options stream_options = bx_tar_make_stream_options(options);
    struct bx_tar_stream_sink sink = {
        .user = NULL,
        .write = bx_tar_file_sink_write,
    };
    struct bx_archive_output_file output = {0};
    bool ok;

    if (!bx_archive_output_file_open(&output, options->archive_path, diag)) {
        return false;
    }
    sink.user = output.stream;
    ok = bx_tar_stream_encode_fs_list(files, &stream_options, &sink, diag);
    if (ok) {
        ok = bx_archive_output_file_finish(&output, diag);
    }
    else {
        bx_archive_output_file_discard(&output);
    }
    return ok;
}

static bool bx_tar_write_create_archive_gzip_direct(const struct bx_archive_fs_list* files,
                                                    const struct bx_tar_options* options,
                                                    struct bx_diag_ctx* diag) {
    struct bx_tar_gzip_stream_create_ctx create_ctx = {
        .files = files,
        .options = options,
    };
    struct bx_archive_gzip_stream_sink sink = {
        .user = NULL,
        .write = bx_tar_file_sink_write,
    };
    struct bx_archive_output_file output = {0};
    bool ok;

    if (!bx_archive_output_file_open(&output, options->archive_path, diag)) {
        return false;
    }
    sink.user = output.stream;
    ok = bx_archive_run_gzip_filter_stream(bx_tar_gzip_stream_produce, &create_ctx, &sink, diag);
    if (ok) {
        ok = bx_archive_output_file_finish(&output, diag);
    }
    else {
        bx_archive_output_file_discard(&output);
    }
    return ok;
}

static bool bx_tar_write_create_archive_gzip_mt_direct(const struct bx_archive_fs_list* files,
                                                       const struct bx_tar_options* options,
                                                       size_t compress_threads,
                                                       struct bx_diag_ctx* diag) {
    struct bx_tar_gzip_stream_create_ctx create_ctx = {
        .files = files,
        .options = options,
    };
    struct bx_archive_gzip_stream_sink sink = {
        .user = NULL,
        .write = bx_tar_file_sink_write,
    };
    struct bx_archive_output_file output = {0};
    size_t chunk_size = options->mt_chunk_size != 0u ? (size_t)options->mt_chunk_size : (1u << 20);
    size_t max_inflight = compress_threads > (SIZE_MAX / 4u) ? compress_threads : compress_threads * 4u;
    bool ok;

    if (!bx_archive_output_file_open(&output, options->archive_path, diag)) {
        return false;
    }
    sink.user = output.stream;
    ok = bx_archive_run_gzip_filter_mt_stream(bx_tar_gzip_stream_produce,
                                              &create_ctx,
                                              &sink,
                                              compress_threads,
                                              chunk_size,
                                              max_inflight,
                                              diag);
    if (ok) {
        ok = bx_archive_output_file_finish(&output, diag);
    }
    else {
        bx_archive_output_file_discard(&output);
    }
    return ok;
}

struct bx_tar_extract_state {
    const struct bx_tar_options* options;
    const struct bx_tar_select_plan* select_plan;
    struct bx_archive_pending_dirs dirs;
    struct bx_tar_name_policy name_policy;
    bool warned_absolute;
    bool warned_dotdot;
    bool* matched_members;
    int status;
    int current_fd;
    char* current_dest_path;
    mode_t current_mode_bits;
    struct timespec current_mtime;
    bool current_sparse;
    size_t current_sparse_extent_index;
    size_t current_sparse_extent_offset;
    size_t current_sparse_logical_offset;
    enum {
        BX_TAR_EXTRACT_STREAM_NONE = 0,
        BX_TAR_EXTRACT_STREAM_STDOUT,
        BX_TAR_EXTRACT_STREAM_FILE,
    } current_stream_mode;
};

struct bx_tar_list_state {
    const struct bx_tar_select_plan* select_plan;
    bool* matched_members;
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

static void bx_tar_extract_state_init(struct bx_tar_extract_state* state,
                                      const struct bx_tar_options* options,
                                      const struct bx_tar_select_plan* select_plan) {
    memset(state, 0, sizeof(*state));
    state->options = options;
    state->select_plan = select_plan;
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
    bx_archive_pending_dirs_free(&state->dirs);
}

static void bx_tar_list_state_init(struct bx_tar_list_state* state,
                                   const struct bx_tar_select_plan* select_plan) {
    state->select_plan = select_plan;
    state->matched_members = bx_tar_alloc_matched_members(select_plan);
}

static void bx_tar_list_state_cleanup(struct bx_tar_list_state* state) {
    free(state->matched_members);
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
    state->current_sparse = false;
    state->current_sparse_extent_index = 0u;
    state->current_sparse_extent_offset = 0u;
    state->current_sparse_logical_offset = 0u;
    free(state->current_dest_path);
    state->current_dest_path = NULL;
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
    char* clean_name;
    char* dest_path = NULL;
    const char* extract_dir = NULL;
    bool stripped_absolute;
    bool stripped_dotdot;

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
    if (stripped_absolute && !state->warned_absolute) {
        fprintf(stderr, "%s: Removing leading '/' from member names\n", diag->progname);
        state->warned_absolute = true;
    }
    if (stripped_dotdot && !state->warned_dotdot) {
        fprintf(stderr, "%s: Removing leading '../' from member names\n", diag->progname);
        state->warned_dotdot = true;
    }
    if (clean_name[0] == '\0') {
        free(clean_name);
        bx_tar_extract_clear_current_stream(state);
        return true;
    }

    if (state->options->to_stdout) {
        bool ok = true;
        bx_tar_extract_clear_current_stream(state);
        if (entry->kind == BX_TAR_KIND_REG) {
            state->current_stream_mode = BX_TAR_EXTRACT_STREAM_STDOUT;
            state->current_sparse = entry->sparse;
        }
        free(clean_name);
        return ok;
    }

    dest_path = extract_dir ? bx_path_join(extract_dir, clean_name) : xstrdup(clean_name);
    free(clean_name);

    if (state->options->keep_old_files
        && access(dest_path, F_OK) == 0
        && entry->kind != BX_TAR_KIND_DIR) {
        fprintf(stderr, "%s: %s: Cannot open: File exists\n", diag->progname, entry->name);
        state->status = 2;
        free(dest_path);
        bx_tar_extract_clear_current_stream(state);
        return true;
    }

    if (entry->kind == BX_TAR_KIND_DIR) {
        if (mkdir(dest_path, 0777u) != 0 && errno != EEXIST) {
            bx_diag(diag, "%s: %s", dest_path, strerror(errno));
            free(dest_path);
            return false;
        }
        bx_archive_pending_dirs_record(&state->dirs,
                                       dest_path,
                                       entry->mode,
                                       !state->options->touch_mtime,
                                       entry->mtime);
        free(dest_path);
        bx_tar_extract_clear_current_stream(state);
        return true;
    }

    if (!bx_archive_ensure_parent_dirs(dest_path, diag)) {
        free(dest_path);
        return false;
    }

    if (entry->kind == BX_TAR_KIND_REG) {
        int fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, entry->mode & 07777u);
        if (fd < 0) {
            bx_diag(diag, "%s: %s", dest_path, strerror(errno));
            free(dest_path);
            return false;
        }
        bx_tar_extract_clear_current_stream(state);
        state->current_fd = fd;
        state->current_dest_path = dest_path;
        state->current_mode_bits = entry->mode;
        state->current_mtime = entry->mtime;
        state->current_stream_mode = BX_TAR_EXTRACT_STREAM_FILE;
        state->current_sparse = entry->sparse;
        return true;
    }
    else if (entry->kind == BX_TAR_KIND_SYMLINK) {
        unlink(dest_path);
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
    }
    else if (entry->kind == BX_TAR_KIND_HARDLINK) {
        bool target_stripped_absolute = false;
        bool target_stripped_dotdot = false;
        char* mapped_target = bx_tar_map_member_name(entry->linkname,
                                                     &state->name_policy,
                                                     &target_stripped_absolute,
                                                     &target_stripped_dotdot);
        char* target = extract_dir ? bx_path_join(extract_dir, mapped_target) : xstrdup(mapped_target);
        (void)target_stripped_absolute;
        (void)target_stripped_dotdot;
        free(mapped_target);
        if (!bx_archive_ensure_parent_dirs(dest_path, diag)) {
            free(dest_path);
            free(target);
            return false;
        }
        unlink(dest_path);
        if (link(target, dest_path) != 0) {
            bx_diag(diag, "%s: %s", dest_path, strerror(errno));
            free(dest_path);
            free(target);
            return false;
        }
        free(target);
    }
    else if (entry->kind == BX_TAR_KIND_FIFO) {
        unlink(dest_path);
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
    if (chmod(dest_path, mode & 07777u) != 0) {
        bx_diag(diag, "%s: %s", dest_path, strerror(errno));
        free(dest_path);
        return false;
    }
    if (!state->options->touch_mtime
        && !bx_archive_set_path_mtime(dest_path, mtime, false, diag)) {
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
    if (!bx_tar_select_plan_match(state->select_plan,
                                  entry->name,
                                  state->select_plan->len == 0u,
                                  state->matched_members,
                                  NULL)) {
        return true;
    }
    if (entry->kind == BX_TAR_KIND_DIR) {
        if (printf("%s/\n", entry->name) < 0) {
            bx_diag(diag, "write error: %s", strerror(errno));
            return false;
        }
    }
    else if (printf("%s\n", entry->name) < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
    return true;
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

static bool bx_tar_list_stream_visit(void* user,
                                     const struct bx_tar_entry* entry,
                                     struct bx_diag_ctx* diag) {
    return bx_tar_list_one_entry(user, entry, diag);
}

static int bx_tar_process_archive_stream(const struct bx_tar_options* options,
                                         const struct bx_tar_select_plan* select_plan,
                                         struct bx_diag_ctx* diag) {
    struct bx_tar_reader_stream_options reader_options = {
        .archive_path = options->archive_path,
        .require_gzip = options->gzip
            || (options->auto_compress
                && options->archive_path != NULL
                && bx_archive_path_has_gzip_suffix(options->archive_path)),
    };

    if (options->mode == BX_TAR_MODE_LIST) {
        struct bx_tar_list_state state;
        struct bx_tar_stream_visitor_ops visitor_ops = {
            .user = &state,
            .begin_entry = bx_tar_list_stream_visit,
            .stream_sparse_payload = true,
        };
        int rc;

        bx_tar_list_state_init(&state, select_plan);
        if (!bx_tar_visit_archive_stream(&reader_options, &visitor_ops, diag)) {
            bx_tar_list_state_cleanup(&state);
            return 2;
        }
        rc = bx_tar_list_finish(&state, diag);
        bx_tar_list_state_cleanup(&state);
        return rc;
    }
    else {
        struct bx_tar_extract_state state;
        struct bx_tar_stream_visitor_ops visitor_ops = {
            .user = &state,
            .begin_entry = bx_tar_extract_stream_visit,
            .visit_payload = bx_tar_extract_stream_payload_visit,
            .end_entry = bx_tar_extract_stream_end_visit,
            .stream_sparse_payload = true,
        };
        int rc;

        bx_tar_extract_state_init(&state, options, select_plan);
        if (!bx_tar_visit_archive_stream(&reader_options, &visitor_ops, diag)) {
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
    const struct bx_tar_options* options;
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

static bool bx_tar_write_rewrite_stream_body(const struct bx_tar_rewrite_stream_ctx* ctx,
                                             const struct bx_tar_stream_sink* sink,
                                             struct bx_diag_ctx* diag) {
    struct bx_tar_rewrite_visit_state state;
    struct bx_tar_stream_visitor_ops visitor_ops = {
        .user = &state,
        .begin_entry = bx_tar_rewrite_stream_begin_entry,
        .visit_payload = bx_tar_rewrite_stream_visit_payload,
        .end_entry = bx_tar_rewrite_stream_end_entry,
        .stream_sparse_payload = true,
    };

    memset(&state, 0, sizeof(state));
    state.ctx = ctx;
    state.sink = sink;
    state.stream_options = bx_tar_make_stream_options(ctx->options);
    state.counting_user.inner = sink;
    state.counting_user.bytes_written = &state.bytes_written;
    state.counting_sink.user = &state.counting_user;
    state.counting_sink.write = bx_tar_stream_counting_sink_write;
    state.counting_sink.callback_owns_errors = sink->callback_owns_errors;

    if (!bx_tar_visit_archive_stream(ctx->reader_options, &visitor_ops, diag)) {
        return false;
    }
    if (ctx->options->mode == BX_TAR_MODE_APPEND
        && !bx_tar_stream_write_fs_list_body(ctx->appended_files,
                                             &state.stream_options,
                                             sink,
                                             &state.bytes_written,
                                             diag)) {
        return false;
    }
    if (!bx_tar_stream_write_trailer(sink, state.bytes_written, diag)) {
        return false;
    }
    if (ctx->delete_plan != NULL
        && bx_tar_select_plan_report_unmatched(ctx->delete_plan, ctx->matched_members, diag)) {
        if (ctx->had_selection_errors != NULL) {
            *ctx->had_selection_errors = true;
        }
    }
    return true;
}

static bool bx_tar_rewrite_stream_produce(void* user,
                                          const struct bx_archive_gzip_stream_sink* sink,
                                          struct bx_diag_ctx* diag) {
    const struct bx_tar_rewrite_stream_ctx* ctx = user;
    struct bx_tar_gzip_stream_sink_adapter adapter = {
        .sink = sink,
    };
    struct bx_tar_stream_sink tar_sink = {
        .user = &adapter,
        .write = bx_tar_gzip_stream_sink_write,
        .callback_owns_errors = true,
    };

    return bx_tar_write_rewrite_stream_body(ctx, &tar_sink, diag);
}

static bool bx_tar_write_rewrite_archive_direct(const struct bx_tar_rewrite_stream_ctx* ctx,
                                                const struct bx_tar_options* options,
                                                struct bx_diag_ctx* diag) {
    struct bx_tar_stream_sink sink = {
        .user = NULL,
        .write = bx_tar_file_sink_write,
    };
    struct bx_archive_output_file output = {0};
    bool ok;

    if (!bx_archive_output_file_open(&output, options->archive_path, diag)) {
        return false;
    }
    sink.user = output.stream;
    ok = bx_tar_write_rewrite_stream_body(ctx, &sink, diag);
    if (ok) {
        ok = bx_archive_output_file_finish(&output, diag);
    }
    else {
        bx_archive_output_file_discard(&output);
    }
    return ok;
}

static bool bx_tar_write_rewrite_archive_gzip_direct(const struct bx_tar_rewrite_stream_ctx* ctx,
                                                     const struct bx_tar_options* options,
                                                     struct bx_diag_ctx* diag) {
    struct bx_tar_rewrite_stream_ctx producer_ctx = *ctx;
    struct bx_archive_gzip_stream_sink sink = {
        .user = NULL,
        .write = bx_tar_file_sink_write,
    };
    struct bx_archive_output_file output = {0};
    bool ok;

    if (!bx_archive_output_file_open(&output, options->archive_path, diag)) {
        return false;
    }
    sink.user = output.stream;
    ok = bx_archive_run_gzip_filter_stream(bx_tar_rewrite_stream_produce, &producer_ctx, &sink, diag);
    if (ok) {
        ok = bx_archive_output_file_finish(&output, diag);
    }
    else {
        bx_archive_output_file_discard(&output);
    }
    return ok;
}

static bool bx_tar_write_rewrite_archive_gzip_mt_direct(const struct bx_tar_rewrite_stream_ctx* ctx,
                                                        const struct bx_tar_options* options,
                                                        size_t compress_threads,
                                                        struct bx_diag_ctx* diag) {
    struct bx_tar_rewrite_stream_ctx producer_ctx = *ctx;
    struct bx_archive_gzip_stream_sink sink = {
        .user = NULL,
        .write = bx_tar_file_sink_write,
    };
    struct bx_archive_output_file output = {0};
    size_t chunk_size = options->mt_chunk_size != 0u ? (size_t)options->mt_chunk_size : (1u << 20);
    size_t max_inflight = compress_threads > (SIZE_MAX / 4u) ? compress_threads : compress_threads * 4u;
    bool ok;

    if (!bx_archive_output_file_open(&output, options->archive_path, diag)) {
        return false;
    }
    sink.user = output.stream;
    ok = bx_archive_run_gzip_filter_mt_stream(bx_tar_rewrite_stream_produce,
                                              &producer_ctx,
                                              &sink,
                                              compress_threads,
                                              chunk_size,
                                              max_inflight,
                                              diag);
    if (ok) {
        ok = bx_archive_output_file_finish(&output, diag);
    }
    else {
        bx_archive_output_file_discard(&output);
    }
    return ok;
}

static size_t bx_tar_round_up_bytes(size_t value, size_t align) {
    size_t rem = value % align;

    if (rem == 0u) {
        return value;
    }
    return value + (align - rem);
}

static bool bx_tar_block_is_zero(const unsigned char* block) {
    size_t i;

    for (i = 0u; i < BX_TAR_BLOCK_SIZE; i++) {
        if (block[i] != 0u) {
            return false;
        }
    }
    return true;
}

static bool bx_tar_parse_octal_field(const unsigned char* field,
                                     size_t len,
                                     size_t* value_out) {
    size_t value = 0u;
    size_t i = 0u;

    while (i < len && (field[i] == ' ' || field[i] == '\0')) {
        i++;
    }
    for (; i < len; i++) {
        unsigned char ch = field[i];

        if (ch == '\0' || ch == ' ') {
            break;
        }
        if (ch < '0' || ch > '7') {
            return false;
        }
        value = (value << 3) + (size_t)(ch - '0');
    }
    *value_out = value;
    return true;
}

static bool bx_tar_fd_read_exact(int fd,
                                 void* buffer,
                                 size_t len,
                                 bool* eof_out,
                                 struct bx_diag_ctx* diag) {
    size_t total = 0u;

    while (total < len) {
        ssize_t nread = bx_xread(fd, (unsigned char*)buffer + total, len - total);

        if (nread < 0) {
            bx_diag(diag, "read error: %s", strerror(errno));
            return false;
        }
        if (nread == 0) {
            if (total == 0u) {
                *eof_out = true;
                return true;
            }
            bx_diag(diag, "truncated archive");
            return false;
        }
        total += (size_t)nread;
    }

    *eof_out = false;
    return true;
}

static bool bx_tar_find_plain_archive_end_fd(int fd,
                                             off_t file_size,
                                             off_t* end_out,
                                             struct bx_diag_ctx* diag) {
    unsigned char header[BX_TAR_BLOCK_SIZE];
    off_t offset = 0;
    bool have_header = false;

    if (lseek(fd, 0, SEEK_SET) < 0) {
        bx_diag(diag, "read error: %s", strerror(errno));
        return false;
    }

    while (true) {
        size_t size = 0u;
        off_t payload_padded;
        bool eof = false;

        if (!have_header) {
            if (offset == file_size) {
                *end_out = offset;
                return true;
            }
            if (!bx_tar_fd_read_exact(fd, header, sizeof(header), &eof, diag)) {
                return false;
            }
            if (eof) {
                *end_out = offset;
                return true;
            }
        }
        have_header = false;

        if (bx_tar_block_is_zero(header)) {
            off_t zero_offset = offset;

            offset += BX_TAR_BLOCK_SIZE;
            if (offset == file_size) {
                *end_out = zero_offset;
                return true;
            }
            if (!bx_tar_fd_read_exact(fd, header, sizeof(header), &eof, diag)) {
                return false;
            }
            if (eof || bx_tar_block_is_zero(header)) {
                *end_out = zero_offset;
                return true;
            }
            have_header = true;
            continue;
        }

        if (!bx_tar_parse_octal_field(header + 124, 12u, &size)) {
            bx_diag(diag, "invalid tar header");
            return false;
        }
        payload_padded = (off_t)bx_tar_round_up_bytes(size, BX_TAR_BLOCK_SIZE);
        if (payload_padded < 0 || file_size - offset < (off_t)BX_TAR_BLOCK_SIZE + payload_padded) {
            bx_diag(diag, "truncated archive");
            return false;
        }
        offset += (off_t)BX_TAR_BLOCK_SIZE + payload_padded;
        if (lseek(fd, offset, SEEK_SET) < 0) {
            bx_diag(diag, "read error: %s", strerror(errno));
            return false;
        }
    }
}

struct bx_tar_fd_sink {
    int fd;
};

static bool bx_tar_fd_sink_write(void* user, const void* data, size_t len) {
    const struct bx_tar_fd_sink* sink = user;
    return bx_xwrite_all(sink->fd, data, len);
}

static int bx_tar_try_append_plain_in_place(const struct bx_archive_fs_list* appended_files,
                                            const struct bx_tar_options* options,
                                            struct bx_diag_ctx* diag) {
    struct stat st;
    int fd = -1;
    off_t archive_end = 0;
    struct bx_tar_stream_options stream_options;
    struct bx_tar_fd_sink fd_sink;
    struct bx_tar_stream_sink sink = {
        .user = &fd_sink,
        .write = bx_tar_fd_sink_write,
        .callback_owns_errors = false,
    };
    size_t bytes_written = 0u;
    bool ok = false;

    if (bx_tar_output_uses_gzip(options) || strcmp(options->archive_path, "-") == 0) {
        return -1;
    }

    fd = open(options->archive_path, O_RDWR);
    if (fd < 0) {
        if (errno == ENOENT) {
            return bx_tar_write_create_archive_direct(appended_files, options, diag) ? 1 : 0;
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
    if (!bx_tar_find_plain_archive_end_fd(fd, st.st_size, &archive_end, diag)) {
        close(fd);
        return 0;
    }
    if (appended_files->len == 0u) {
        close(fd);
        return 1;
    }
    if (ftruncate(fd, archive_end) != 0) {
        bx_diag(diag, "%s: %s", options->archive_path, strerror(errno));
        close(fd);
        return 0;
    }
    if (lseek(fd, archive_end, SEEK_SET) < 0) {
        bx_diag(diag, "%s: %s", options->archive_path, strerror(errno));
        close(fd);
        return 0;
    }

    stream_options = bx_tar_make_stream_options(options);
    fd_sink.fd = fd;
    bytes_written = (size_t)archive_end;
    ok = bx_tar_stream_write_fs_list_body(appended_files,
                                          &stream_options,
                                          &sink,
                                          &bytes_written,
                                          diag)
        && bx_tar_stream_write_trailer(&sink, bytes_written, diag);
    if (!ok) {
        close(fd);
        return 0;
    }
    if (close(fd) != 0) {
        bx_diag(diag, "%s: %s", options->archive_path, strerror(errno));
        return 0;
    }
    return 1;
}

static int bx_tar_rewrite_archive(const struct bx_tar_options* options,
                                  struct bx_diag_ctx* diag) {
    struct bx_archive_fs_list appended_files = {0};
    struct bx_tar_reader_stream_options reader_options = {
        .archive_path = NULL,
        .require_gzip = options->gzip
            || (options->auto_compress
                && options->archive_path != NULL
                && bx_archive_path_has_gzip_suffix(options->archive_path)),
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
        append_fast_rc = bx_tar_try_append_plain_in_place(&appended_files, options, diag);
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
    if (bx_tar_output_uses_gzip(options)) {
        size_t compress_threads = bx_tar_effective_compress_threads(options);

        if (!(compress_threads > 1u
                  ? bx_tar_write_rewrite_archive_gzip_mt_direct(&rewrite_ctx,
                                                                options,
                                                                compress_threads,
                                                                diag)
                  : bx_tar_write_rewrite_archive_gzip_direct(&rewrite_ctx, options, diag))) {
            goto out;
        }
    }
    else if (!bx_tar_write_rewrite_archive_direct(&rewrite_ctx, options, diag)) {
        goto out;
    }
    rc = 0;
postwrite:
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
    if (text[0] != '@') {
        return false;
    }
    out->tv_sec = (time_t)strtoll(text + 1, NULL, 10);
    out->tv_nsec = 0;
    return true;
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
        case BX_TAR_OPT_MODE_CREATE:
            return bx_tar_set_mode_option(options, BX_TAR_MODE_CREATE, NULL, diag);
        case BX_TAR_OPT_MODE_LIST:
            return bx_tar_set_mode_option(options, BX_TAR_MODE_LIST, NULL, diag);
        case BX_TAR_OPT_MODE_EXTRACT:
            return bx_tar_set_mode_option(options, BX_TAR_MODE_EXTRACT, NULL, diag);
        case BX_TAR_OPT_MODE_APPEND:
            return bx_tar_set_mode_option(options, BX_TAR_MODE_APPEND, NULL, diag);
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
        case BX_TAR_OPT_GZIP_ON:
            options->gzip = true;
            return true;
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
        case BX_TAR_OPT_OWNER:
            options->owner = (uid_t)strtoul(value, NULL, 10);
            options->owner_set = true;
            return true;
        case BX_TAR_OPT_GROUP:
            options->group = (gid_t)strtoul(value, NULL, 10);
            options->group_set = true;
            return true;
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
                if (!bx_tar_create_options_add_add_file(&options->create_options, argv[j])) {
                    return false;
                }
            }
            break;
        }
        if (!oldstyle && arg[0] != '-') {
            if (!bx_tar_create_options_add_add_file(&options->create_options, arg)) {
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
    if (options->archive_path == NULL) {
        bx_diag(diag, "archive file not specified; use -f");
        return false;
    }
    if ((options->mode == BX_TAR_MODE_CREATE || options->mode == BX_TAR_MODE_APPEND)
        && !bx_tar_create_has_inputs(options, argc)) {
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
        struct bx_archive_fs_list files = {0};
        bool had_create_errors = false;
        bool had_postwrite_errors = false;
        bool gzip_output = bx_tar_output_uses_gzip(&options);
        size_t compress_threads = bx_tar_effective_compress_threads(&options);

        if (!bx_tar_create_collect_fs_entries(&files,
                                              &options.create_options,
                                              options.sort_name,
                                              &had_create_errors,
                                              &diag)) {
            bx_tar_options_cleanup(&options);
            return 2;
        }
        if (gzip_output && compress_threads > 1u) {
            rc = bx_tar_write_create_archive_gzip_mt_direct(&files, &options, compress_threads, &diag) ? 0 : 2;
        }
        else if (gzip_output) {
            rc = bx_tar_write_create_archive_gzip_direct(&files, &options, &diag) ? 0 : 2;
        }
        else {
            rc = bx_tar_write_create_archive_direct(&files, &options, &diag) ? 0 : 2;
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
        bx_archive_fs_list_free(&files);
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
