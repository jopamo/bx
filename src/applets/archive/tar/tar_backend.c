#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
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
#include "applets/archive/archive_temp.h"
#include "applets/archive/tar/tar_backend.h"
#include "applets/archive/tar/tar_create.h"
#include "applets/archive/tar/tar_names.h"
#include "applets/archive/tar/tar_report.h"
#include "applets/archive/tar/tar_reader.h"
#include "applets/archive/tar/tar_select.h"
#include "applets/archive/tar/tar_stream.h"
#include "bx/libbx.h"
#include "lib/args_common.h"
#include "lib/cli_common.h"
#include "lib/copy_data.h"
#include "lib/dir_path.h"
#include "lib/fd_ops.h"
#include "lib/id_parse.h"
#include "lib/inode_ledger.h"
#include "lib/mode_parse.h"
#include "lib/path_ops.h"
#include "lib/remove_ops.h"
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
    BX_TAR_MODE_CREATE,
    BX_TAR_MODE_LIST,
    BX_TAR_MODE_EXTRACT,
};

enum bx_tar_old_file_mode {
    BX_TAR_OLD_FILES_DEFAULT = 0,
    BX_TAR_OLD_FILES_OVERWRITE,
    BX_TAR_OLD_FILES_UNLINK_FIRST,
    BX_TAR_OLD_FILES_KEEP,
    BX_TAR_OLD_FILES_SKIP,
    BX_TAR_OLD_FILES_KEEP_NEWER,
};

enum bx_tar_owner_policy {
    BX_TAR_OWNER_DEFAULT = 0,
    BX_TAR_OWNER_FORCE,
    BX_TAR_OWNER_DISABLE,
};

enum bx_tar_permission_policy {
    BX_TAR_PERMISSIONS_DEFAULT = 0,
    BX_TAR_PERMISSIONS_FORCE,
    BX_TAR_PERMISSIONS_DISABLE,
};

struct bx_tar_options {
    enum bx_tar_mode mode;
    const char* invalid_mode_option[BX_TAR_MODE_EXTRACT + 1];
    const char* gzip_output_option;
    const char* filesystem_option;
    bool mt_chunk_size_set;
    bool saw_mode_option;
    const char* archive_path;
    bool to_stdout;
    enum bx_tar_old_file_mode old_file_mode;
    const char* old_file_mode_name;
    bool recursive_unlink;
    bool verbose_reports;
    unsigned int verbose_count;
    bool report_mapped_names;
    bool report_block_numbers;
    bool report_totals;
    char* index_file_path;
    const struct bx_archive_codec* codec;
    bool auto_compress;
    bool absolute_names;
    bool touch_mtime;
    bool sort_name;
    bool sparse;
    bool sparse_selectors;
    const char* starting_file;
    bool format_ustar;
    bool numeric_owner;
    bool owner_set;
    bool group_set;
    uid_t owner;
    gid_t group;
    bool fixed_mtime;
    struct timespec mtime;
    enum bx_tar_owner_policy owner_policy;
    enum bx_tar_permission_policy permission_policy;
    struct bx_tar_metadata_options metadata;
    bool preserve_all;
    const char* preservation_conflict;
    bool no_mt;
    enum bx_archive_codec_seek_mode seek_mode;
    char* mode_text;
    bool newer_active;
    bool newer_use_ctime;
    struct timespec newer_time;
    size_t strip_components;
    int threads;
    int compress_threads;
    uintmax_t mt_chunk_size;
    const char* one_top_level;
    struct bx_tar_create_options create_options;
    uintmax_t occurrence;
};

enum bx_tar_option_arg_mode {
    BX_TAR_OPTARG_NONE = 0,
    BX_TAR_OPTARG_REQUIRED,
    BX_TAR_OPTARG_OPTIONAL,
};

enum bx_tar_option_effect {
    BX_TAR_OPT_INVALID = 0,
    BX_TAR_OPT_SPARSE,
    BX_TAR_OPT_SPARSE_VERSION,
    BX_TAR_OPT_HOLE_DETECTION,
    BX_TAR_OPT_MODE_CREATE,
    BX_TAR_OPT_MODE_LIST,
    BX_TAR_OPT_MODE_EXTRACT,
    BX_TAR_OPT_ARCHIVE_PATH,
    BX_TAR_OPT_DIRECTORY,
    BX_TAR_OPT_TO_STDOUT,
    BX_TAR_OPT_KEEP_OLD_FILES,
    BX_TAR_OPT_OVERWRITE,
    BX_TAR_OPT_UNLINK_FIRST,
    BX_TAR_OPT_SKIP_OLD_FILES,
    BX_TAR_OPT_KEEP_NEWER_FILES,
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
    BX_TAR_OPT_THREADS,
    BX_TAR_OPT_COMPRESS_THREADS,
    BX_TAR_OPT_MT_CHUNK_SIZE,
    BX_TAR_OPT_NO_MT,
    BX_TAR_OPT_BZIP2_ON,
    BX_TAR_OPT_GZIP_ON,
    BX_TAR_OPT_XZ_ON,
    BX_TAR_OPT_ZSTD_ON,
    BX_TAR_OPT_SEEK_ON,
    BX_TAR_OPT_SEEK_OFF,
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
    BX_TAR_OPT_FORMAT,
    BX_TAR_OPT_SORT,
    BX_TAR_OPT_MTIME,
    BX_TAR_OPT_MODE,
    BX_TAR_OPT_OWNER,
    BX_TAR_OPT_GROUP,
    BX_TAR_OPT_OWNER_RESTORE_ON,
    BX_TAR_OPT_OWNER_RESTORE_OFF,
    BX_TAR_OPT_PERMISSIONS_ON,
    BX_TAR_OPT_PERMISSIONS_OFF,
    BX_TAR_OPT_XATTRS_ON,
    BX_TAR_OPT_XATTRS_OFF,
    BX_TAR_OPT_XATTRS_INCLUDE,
    BX_TAR_OPT_XATTRS_EXCLUDE,
    BX_TAR_OPT_SELINUX_ON,
    BX_TAR_OPT_SELINUX_OFF,
    BX_TAR_OPT_ACLS_ON,
    BX_TAR_OPT_ACLS_OFF,
    BX_TAR_OPT_FILE_FLAGS,
    BX_TAR_OPT_PRESERVE_ALL,
    BX_TAR_OPT_IGNORE_FAILED_READ,
    BX_TAR_OPT_ONE_FILE_SYSTEM,
    BX_TAR_OPT_OCCURRENCE,
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
    {"--create", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_CREATE},
    {"--list", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_LIST},
    {"--extract", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_EXTRACT},
    {"--get", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_EXTRACT},
    {"--hole-detection", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_HOLE_DETECTION},
    {"--ignore-failed-read", BX_TAR_OPTARG_NONE, BX_TAR_OPT_IGNORE_FAILED_READ},
    {"--no-seek", BX_TAR_OPTARG_NONE, BX_TAR_OPT_SEEK_OFF},
    {"--seek", BX_TAR_OPTARG_NONE, BX_TAR_OPT_SEEK_ON},
    {"--occurrence", BX_TAR_OPTARG_OPTIONAL, BX_TAR_OPT_OCCURRENCE},
    {"--sparse-version", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_SPARSE_VERSION},
    {"--sparse", BX_TAR_OPTARG_NONE, BX_TAR_OPT_SPARSE},
    {"--add-file", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_ADD_FILE},
    {"--directory", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_DIRECTORY},
    {"--exclude", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_EXCLUDE},
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
    {"--keep-newer-files", BX_TAR_OPTARG_NONE, BX_TAR_OPT_KEEP_NEWER_FILES},
    {"--one-top-level", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_ONE_TOP_LEVEL},
    {"--overwrite", BX_TAR_OPTARG_NONE, BX_TAR_OPT_OVERWRITE},
    {"--recursive-unlink", BX_TAR_OPTARG_NONE, BX_TAR_OPT_RECURSIVE_UNLINK},
    {"--threads", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_THREADS},
    {"--compress-threads", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_COMPRESS_THREADS},
    {"--mt-chunk-size", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_MT_CHUNK_SIZE},
    {"--no-mt", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NO_MT},
    {"--skip-old-files", BX_TAR_OPTARG_NONE, BX_TAR_OPT_SKIP_OLD_FILES},
    {"--unlink-first", BX_TAR_OPTARG_NONE, BX_TAR_OPT_UNLINK_FIRST},
    {"--to-stdout", BX_TAR_OPTARG_NONE, BX_TAR_OPT_TO_STDOUT},
    {"--group", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_GROUP},
    {"--numeric-owner", BX_TAR_OPTARG_NONE, BX_TAR_OPT_NUMERIC_OWNER},
    {"--owner", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_OWNER},
    {"--mode", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_MODE},
    {"--mtime", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_MTIME},
    {"--touch", BX_TAR_OPTARG_NONE, BX_TAR_OPT_TOUCH_MTIME_ON},
    {"--no-same-owner", BX_TAR_OPTARG_NONE, BX_TAR_OPT_OWNER_RESTORE_OFF},
    {"--no-same-permissions", BX_TAR_OPTARG_NONE, BX_TAR_OPT_PERMISSIONS_OFF},
    {"--preserve-permissions", BX_TAR_OPTARG_NONE, BX_TAR_OPT_PERMISSIONS_ON},
    {"--same-permissions", BX_TAR_OPTARG_NONE, BX_TAR_OPT_PERMISSIONS_ON},
    {"--same-owner", BX_TAR_OPTARG_NONE, BX_TAR_OPT_OWNER_RESTORE_ON},
    {"--sort", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_SORT},
    {"--acls", BX_TAR_OPTARG_NONE, BX_TAR_OPT_ACLS_ON},
    {"--no-acls", BX_TAR_OPTARG_NONE, BX_TAR_OPT_ACLS_OFF},
    {"--file-flags", BX_TAR_OPTARG_NONE, BX_TAR_OPT_FILE_FLAGS},
    {"--preserve-all", BX_TAR_OPTARG_NONE, BX_TAR_OPT_PRESERVE_ALL},
    {"--no-selinux", BX_TAR_OPTARG_NONE, BX_TAR_OPT_SELINUX_OFF},
    {"--no-xattrs", BX_TAR_OPTARG_NONE, BX_TAR_OPT_XATTRS_OFF},
    {"--selinux", BX_TAR_OPTARG_NONE, BX_TAR_OPT_SELINUX_ON},
    {"--xattrs", BX_TAR_OPTARG_NONE, BX_TAR_OPT_XATTRS_ON},
    {"--xattrs-exclude", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_XATTRS_EXCLUDE},
    {"--xattrs-include", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_XATTRS_INCLUDE},
    {"--file", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_ARCHIVE_PATH},
    {"--format", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_FORMAT},
    {"--auto-compress", BX_TAR_OPTARG_NONE, BX_TAR_OPT_AUTO_COMPRESS_ON},
    {"--bzip2", BX_TAR_OPTARG_NONE, BX_TAR_OPT_BZIP2_ON},
    {"--xz", BX_TAR_OPTARG_NONE, BX_TAR_OPT_XZ_ON},
    {"--zstd", BX_TAR_OPTARG_NONE, BX_TAR_OPT_ZSTD_ON},
    {"--gzip", BX_TAR_OPTARG_NONE, BX_TAR_OPT_GZIP_ON},
    {"--gunzip", BX_TAR_OPTARG_NONE, BX_TAR_OPT_GZIP_ON},
    {"--ungzip", BX_TAR_OPTARG_NONE, BX_TAR_OPT_GZIP_ON},
    {"--no-auto-compress", BX_TAR_OPTARG_NONE, BX_TAR_OPT_AUTO_COMPRESS_OFF},
    {"--starting-file", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_STARTING_FILE},
    {"--newer-mtime", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NEWER_MTIME},
    {"--newer", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NEWER},
    {"--after-date", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NEWER},
    {"--one-file-system", BX_TAR_OPTARG_NONE, BX_TAR_OPT_ONE_FILE_SYSTEM},
    {"--absolute-names", BX_TAR_OPTARG_NONE, BX_TAR_OPT_ABSOLUTE_NAMES_ON},
    {"--strip-components", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_STRIP_COMPONENTS},
    {"--index-file", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_INDEX_FILE},
    {"--block-number", BX_TAR_OPTARG_NONE, BX_TAR_OPT_BLOCK_NUMBER},
    {"--show-transformed-names", BX_TAR_OPTARG_NONE, BX_TAR_OPT_REPORT_MAPPED_NAMES},
    {"--show-stored-names", BX_TAR_OPTARG_NONE, BX_TAR_OPT_REPORT_MAPPED_NAMES},
    {"--totals", BX_TAR_OPTARG_NONE, BX_TAR_OPT_TOTALS},
    {"--verbose", BX_TAR_OPTARG_NONE, BX_TAR_OPT_VERBOSE},
    {NULL, BX_TAR_OPTARG_NONE, BX_TAR_OPT_INVALID},
};

static const struct bx_tar_short_option_spec bx_tar_short_options[] = {
    {'c', "-c", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_CREATE},
    {'t', "-t", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_LIST},
    {'x', "-x", BX_TAR_OPTARG_NONE, BX_TAR_OPT_MODE_EXTRACT},
    {'n', "-n", BX_TAR_OPTARG_NONE, BX_TAR_OPT_SEEK_ON},
    {'S', "-S", BX_TAR_OPTARG_NONE, BX_TAR_OPT_SPARSE},
    {'C', "-C", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_DIRECTORY},
    {'X', "-X", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_EXCLUDE_FROM},
    {'T', "-T", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_FILES_FROM},
    {'k', "-k", BX_TAR_OPTARG_NONE, BX_TAR_OPT_KEEP_OLD_FILES},
    {'O', "-O", BX_TAR_OPTARG_NONE, BX_TAR_OPT_TO_STDOUT},
    {'m', "-m", BX_TAR_OPTARG_NONE, BX_TAR_OPT_TOUCH_MTIME_ON},
    {'p', "-p", BX_TAR_OPTARG_NONE, BX_TAR_OPT_PERMISSIONS_ON},
    {'H', "-H", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_FORMAT},
    {'a', "-a", BX_TAR_OPTARG_NONE, BX_TAR_OPT_AUTO_COMPRESS_ON},
    {'j', "-j", BX_TAR_OPTARG_NONE, BX_TAR_OPT_BZIP2_ON},
    {'J', "-J", BX_TAR_OPTARG_NONE, BX_TAR_OPT_XZ_ON},
    {'z', "-z", BX_TAR_OPTARG_NONE, BX_TAR_OPT_GZIP_ON},
    {'K', "-K", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_STARTING_FILE},
    {'N', "-N", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_NEWER},
    {'P', "-P", BX_TAR_OPTARG_NONE, BX_TAR_OPT_ABSOLUTE_NAMES_ON},
    {'v', "-v", BX_TAR_OPTARG_NONE, BX_TAR_OPT_VERBOSE},
    {'o', "-o", BX_TAR_OPTARG_NONE, BX_TAR_OPT_OWNER_RESTORE_OFF},
    {'f', "-f", BX_TAR_OPTARG_REQUIRED, BX_TAR_OPT_ARCHIVE_PATH},
    {'\0', NULL, BX_TAR_OPTARG_NONE, BX_TAR_OPT_INVALID},
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
        .metadata = &options->metadata,
        .format_ustar = options->format_ustar,
        .sparse = options->sparse,
        .numeric_owner = options->numeric_owner,
        .owner_set = options->owner_set,
        .group_set = options->group_set,
        .fixed_mtime = options->fixed_mtime,
        .mode_text = options->mode_text,
        .owner = options->owner,
        .group = options->group,
        .mtime = options->mtime,
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

static void bx_tar_set_codec_option(struct bx_tar_options* options,
                                    const struct bx_archive_codec* codec) {
    options->codec = codec;
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

struct bx_tar_pending_link {
    size_t boundary_prefix;
    char* path;
    char* target;
    char* leaf;
    dev_t parent_dev;
    ino_t parent_ino;
    struct bx_file_restore restore;
    struct timespec mtime;
    bool omit_mtime;
    size_t bytes;
    uint64_t sequence;
};

struct bx_tar_extract_state {
    const struct bx_tar_options* options;
    bool restore_owner;
    bool preserve_permissions;
    mode_t umask_value;
    FILE* report_stream;
    const struct bx_tar_select_plan* select_plan;
    struct bx_archive_pending_metadata dirs;
    struct bx_inode_ledger restored;
    struct bx_tar_pending_link* links;
    size_t link_count;
    size_t link_bytes;
    uint64_t sequence;
    size_t link_checks;
    int root_fd;
    int parent_fd;
    char* leaf;
    struct bx_tar_name_policy name_policy;
    bool warned_absolute;
    bool warned_dotdot;
    bool starting_file_reached;
    bool* matched_members;
    uintmax_t* occurrence_counts;
    int status;
    struct bx_fd_staged_file current_file;
    char* current_leaf;
    char* current_dest_path;
    size_t boundary_prefix;
    mode_t current_mode_bits;
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
    uintmax_t* occurrence_counts;
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

static uintmax_t* bx_tar_alloc_occurrence_counts(const struct bx_tar_select_plan* select_plan,
                                                 uintmax_t occurrence) {
    uintmax_t* occurrence_counts;

    if (select_plan->len == 0u || occurrence == 0u) {
        return NULL;
    }
    occurrence_counts = xmalloc(select_plan->len * sizeof(*occurrence_counts));
    memset(occurrence_counts, 0, select_plan->len * sizeof(*occurrence_counts));
    return occurrence_counts;
}

static bool bx_tar_validate_occurrence_selection(const struct bx_tar_options* options,
                                                 const struct bx_tar_select_plan* select_plan,
                                                 struct bx_diag_ctx* diag) {
    if (options->occurrence == 0u || select_plan->len > 0u) {
        return true;
    }
    bx_diag(diag, "--occurrence is meaningless without a file list");
    return false;
}

static int bx_tar_timespec_compare(struct timespec left, struct timespec right);

static void bx_tar_extract_state_init(struct bx_tar_extract_state* state,
                                      const struct bx_tar_options* options,
                                      const struct bx_tar_select_plan* select_plan,
                                      FILE* report_stream) {
    memset(state, 0, sizeof(*state));
    state->options = options;
    state->restore_owner = options->owner_policy == BX_TAR_OWNER_FORCE
        || (options->owner_policy == BX_TAR_OWNER_DEFAULT && geteuid() == 0);
    state->preserve_permissions = options->permission_policy == BX_TAR_PERMISSIONS_FORCE
        || (options->permission_policy == BX_TAR_PERMISSIONS_DEFAULT && geteuid() == 0);
    state->umask_value = state->preserve_permissions ? 0u : bx_mode_current_umask();
    state->report_stream = report_stream;
    state->select_plan = select_plan;
    state->starting_file_reached = options->starting_file == NULL;
    state->matched_members = bx_tar_alloc_matched_members(select_plan);
    state->occurrence_counts = bx_tar_alloc_occurrence_counts(select_plan, options->occurrence);
    state->name_policy = (struct bx_tar_name_policy){
        .absolute_names = options->absolute_names,
        .strip_components = options->strip_components,
        .one_top_level = options->one_top_level,
    };
    state->current_file = (struct bx_fd_staged_file)BX_FD_STAGED_FILE_INIT;
    state->root_fd = -1;
    state->parent_fd = -1;
    state->dirs.path_policy = BX_DIR_PATH_ALLOW_EXTERNAL | BX_DIR_PATH_REPLACE_NON_DIRS | BX_DIR_PATH_NO_MOUNT_CROSSING;
}

static void bx_tar_extract_state_cleanup(struct bx_tar_extract_state* state) {
    bx_fd_cleanup(&state->root_fd);
    bx_fd_staged_file_discard(&state->current_file);
    free(state->current_leaf);
    free(state->current_dest_path);
    state->current_dest_path = NULL;
    free(state->matched_members);
    free(state->occurrence_counts);
    bx_fd_cleanup(&state->parent_fd);
    free(state->leaf);
    bx_archive_pending_metadata_free(&state->dirs);
    bx_inode_ledger_free(&state->restored);
    for (size_t i = 0; i < state->link_count; i++) {
        free(state->links[i].path);
        free(state->links[i].target);
        free(state->links[i].leaf);
        bx_file_metadata_free(&state->links[i].restore.metadata);
    }
    free(state->links);
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
    };
    state->matched_members = bx_tar_alloc_matched_members(select_plan);
    state->occurrence_counts = bx_tar_alloc_occurrence_counts(select_plan, options->occurrence);
}

static void bx_tar_list_state_cleanup(struct bx_tar_list_state* state) {
    free(state->matched_members);
    free(state->occurrence_counts);
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

static void bx_tar_extract_clear_current_stream(struct bx_tar_extract_state* state) {
    bx_fd_staged_file_discard(&state->current_file);
    free(state->current_leaf);
    state->current_leaf = NULL;
    state->current_stream_mode = BX_TAR_EXTRACT_STREAM_NONE;
    state->current_mode_bits = 0u;
    state->current_sparse = false;
    state->current_sparse_extent_index = 0u;
    state->current_sparse_extent_offset = 0u;
    state->current_sparse_logical_offset = 0u;
    free(state->current_dest_path);
    state->current_dest_path = NULL;
}

static void bx_tar_extract_entry_ids(const struct bx_tar_extract_state* state,
                                     const struct bx_tar_entry* entry,
                                     uid_t* owner_out,
                                     gid_t* group_out,
                                     bool* owner_restore_out,
                                     bool* group_restore_out) {
    *owner_out = entry->uid;
    *group_out = entry->gid;
    *owner_restore_out = state->restore_owner && !entry->omit_uid;
    *group_restore_out = state->restore_owner && !entry->omit_gid;
}

static mode_t bx_tar_extract_mode(const struct bx_tar_extract_state* state,
                                  mode_t archive_mode) {
    mode_t mode = archive_mode & (state->preserve_permissions ? 07777u : 0777u);

    return state->preserve_permissions ? mode : mode & ~state->umask_value;
}

enum bx_tar_existing_target_action {
    BX_TAR_EXISTING_TARGET_PROCEED = 0,
    BX_TAR_EXISTING_TARGET_SKIP,
    BX_TAR_EXISTING_TARGET_ERROR,
};

static enum bx_tar_existing_target_action bx_tar_extract_existing_target_action(
    struct bx_tar_extract_state* state,
    const struct bx_tar_entry* entry,
    const char* dest_path,
    struct bx_diag_ctx* diag) {
    struct stat st;

    if (entry->kind == BX_TAR_KIND_DIR) {
        return BX_TAR_EXISTING_TARGET_PROCEED;
    }
    if (state->options->recursive_unlink
        || state->options->old_file_mode == BX_TAR_OLD_FILES_DEFAULT
        || state->options->old_file_mode == BX_TAR_OLD_FILES_OVERWRITE
        || state->options->old_file_mode == BX_TAR_OLD_FILES_UNLINK_FIRST) {
        return BX_TAR_EXISTING_TARGET_PROCEED;
    }
    if (fstatat(state->parent_fd, state->leaf, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            return BX_TAR_EXISTING_TARGET_PROCEED;
        }
        bx_diag(diag, "%s: %s", dest_path, strerror(errno));
        return BX_TAR_EXISTING_TARGET_ERROR;
    }

    if (state->options->old_file_mode == BX_TAR_OLD_FILES_KEEP) {
        fprintf(stderr, "%s: %s: Cannot open: File exists\n", diag->progname, entry->name);
        state->status = 2;
        return BX_TAR_EXISTING_TARGET_SKIP;
    }
    if (state->options->old_file_mode == BX_TAR_OLD_FILES_SKIP) {
        return BX_TAR_EXISTING_TARGET_SKIP;
    }
    if (state->options->old_file_mode == BX_TAR_OLD_FILES_KEEP_NEWER && !S_ISDIR(st.st_mode) && !entry->omit_mtime && bx_tar_timespec_compare(st.st_mtim, entry->mtime) >= 0) {
        fprintf(stderr,
                "%s: Current '%s' is newer or same age\n",
                diag->progname,
                entry->name);
        return BX_TAR_EXISTING_TARGET_SKIP;
    }
    return BX_TAR_EXISTING_TARGET_PROCEED;
}

static bool bx_tar_extract_remove_empty_dir_default(struct bx_tar_extract_state* state,
                                                    const char* path,
                                                    struct bx_diag_ctx* diag) {
    if (unlinkat(state->parent_fd, state->leaf, AT_REMOVEDIR) == 0) {
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

    if (fstatat(state->parent_fd, state->leaf, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            return true;
        }
        bx_diag(diag, "%s: %s", dest_path, strerror(errno));
        return false;
    }

    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        if (unlinkat(state->parent_fd, state->leaf, 0) != 0) {
            bx_diag(diag, "%s: %s", dest_path, strerror(errno));
            return false;
        }
        return true;
    }

    if (state->options->recursive_unlink) {
        return bx_remove_recursive_at(state->parent_fd, state->leaf, dest_path, &st, diag);
    }
    if (state->options->old_file_mode == BX_TAR_OLD_FILES_OVERWRITE) {
        bx_diag(diag, "%s: %s", dest_path, strerror(EISDIR));
        return false;
    }
    if (state->options->old_file_mode == BX_TAR_OLD_FILES_UNLINK_FIRST) {
        if (unlinkat(state->parent_fd, state->leaf, AT_REMOVEDIR) != 0) {
            bx_diag(diag, "%s: %s", dest_path, strerror(errno));
            return false;
        }
        return true;
    }

    (void)entry;
    return bx_tar_extract_remove_empty_dir_default(state, dest_path, diag);
}

static bool bx_tar_extract_prepare_final_dir_target(struct bx_tar_extract_state* state,
                                                    const char* dest_path,
                                                    bool* mkdir_needed_out,
                                                    struct bx_diag_ctx* diag) {
    struct stat st;

    *mkdir_needed_out = false;
    if (fstatat(state->parent_fd, state->leaf, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            *mkdir_needed_out = true;
            return true;
        }
        bx_diag(diag, "%s: %s", dest_path, strerror(errno));
        return false;
    }

    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        if (!bx_remove_recursive_at(state->parent_fd, state->leaf, dest_path, &st, diag)) {
            return false;
        }
        *mkdir_needed_out = true;
        return true;
    }

    if (state->options->recursive_unlink && strcmp(state->leaf, ".") != 0) {
        if (!bx_remove_recursive_at(state->parent_fd, state->leaf, dest_path, &st, diag)) {
            return false;
        }
        *mkdir_needed_out = true;
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
            else if (state->current_stream_mode == BX_TAR_EXTRACT_STREAM_FILE && lseek(state->current_file.fd, (off_t)extent->offset, SEEK_SET) < 0) {
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
        else if (!bx_archive_write_regular_payload(state->current_file.fd, cursor, chunk, false, diag)) {
            return false;
        }

        cursor += chunk;
        len -= chunk;
        state->current_sparse_extent_offset += chunk;
    }

    return true;
}

static bool bx_tar_extract_select_metadata(struct bx_tar_extract_state* state,
                                           const struct bx_tar_entry* entry,
                                           mode_t mode,
                                           struct bx_file_restore* restore,
                                           const char* path,
                                           struct bx_diag_ctx* diag) {
    restore->mode = mode;
    restore->set_mode = entry->kind != BX_TAR_KIND_SYMLINK;
    restore->mtime = entry->mtime;
    restore->set_mtime = !state->options->touch_mtime && !entry->omit_mtime && entry->kind != BX_TAR_KIND_HARDLINK;
    bx_tar_extract_entry_ids(state, entry, &restore->uid, &restore->gid,
                              &restore->set_owner, &restore->set_group);
    bx_tar_metadata_select(&restore->metadata, &entry->metadata, &state->options->metadata);
    restore->flags_present = state->options->metadata.file_flags && entry->metadata.file_flags != NULL;
    if (restore->flags_present && !bx_tar_metadata_decode_flags(entry->metadata.file_flags, &restore->flags_set, &restore->flags_clear)) {
        bx_diag(diag, "%s: invalid or unsupported file flags: %s", path, strerror(errno));
        return false;
    }
    return true;
}

static bool bx_tar_extract_metadata(struct bx_tar_extract_state* state,
                                     const struct bx_tar_entry* entry, int fd,
                                     const char* path, mode_t mode,
                                     struct bx_diag_ctx* diag) {
    struct bx_file_restore selected = {0};
    if (!bx_tar_extract_select_metadata(state, entry, mode, &selected, path, diag)) {
        bx_file_metadata_free(&selected.metadata);
        return false;
    }
    struct stat status;
    if (fstat(fd, &status) != 0) {
        bx_file_metadata_free(&selected.metadata);
        bx_diag(diag, "%s: %s", path, strerror(errno));
        return false;
    }
    bool deferred = state->options->metadata.file_flags && S_ISREG(status.st_mode);
    struct bx_file_restore immediate = selected;
    if (deferred) {
        immediate.flags_set = immediate.flags_clear = 0u;
        immediate.mode = 0600u;
    }
    bool ok = bx_archive_restore_fd(&immediate, fd, path, S_ISLNK(status.st_mode), S_ISDIR(status.st_mode), diag);
    /* Final snapshots must remain reopenable until links and restrictive flags
     * are complete. ACL replay can have changed the temporary access mask. */
    if (ok && deferred && fchmod(fd, 0600u) != 0) {
        bx_diag(diag, "%s: %s", path, strerror(errno));
        ok = false;
    }
    if (ok && deferred) {
        ok = bx_archive_pending_metadata_record_fd(&state->dirs, fd, path, state->boundary_prefix, &selected, state->sequence, state->sequence);
        if (!ok)
            bx_diag(diag, "%s: cannot defer metadata: %s", path, strerror(errno));
    }
    bx_file_metadata_free(&selected.metadata);
    return ok;
}

static void bx_tar_extract_drop_link(struct bx_tar_extract_state* state, size_t index) {
    struct bx_tar_pending_link* link = &state->links[index];
    state->link_bytes -= link->bytes;
    free(link->path);
    free(link->target);
    free(link->leaf);
    bx_file_metadata_free(&link->restore.metadata);
    memmove(link, link + 1, (--state->link_count - index) * sizeof(*link));
}

static bool bx_tar_extract_cancel_links(struct bx_tar_extract_state* state, struct bx_diag_ctx* diag) {
    if (!state->link_count)
        return true;
    struct stat parent;
    if (fstat(state->parent_fd, &parent) != 0) {
        bx_diag(diag, "%s: %s", state->leaf, strerror(errno));
        return false;
    }
    for (size_t i = 0; i < state->link_count;) {
        struct bx_tar_pending_link* link = &state->links[i];
        if (link->parent_dev == parent.st_dev && link->parent_ino == parent.st_ino && strcmp(link->leaf, state->leaf) == 0)
            bx_tar_extract_drop_link(state, i);
        else
            i++;
    }
    return true;
}

static bool bx_tar_extract_record_inode(struct bx_tar_extract_state* state, int fd, const char* path, struct bx_diag_ctx* diag) {
    struct stat status;
    if (fstat(fd, &status) == 0 && bx_inode_ledger_record(&state->restored, &status, state->sequence, state->sequence, 1048576u))
        return true;
    bx_diag(diag, "%s: cannot record restored inode (limit 1048576): %s", path, strerror(errno));
    return false;
}

static bool bx_tar_extract_queue_link(struct bx_tar_extract_state* state, const struct bx_tar_entry* entry, const char* path, const char* target, mode_t mode, struct bx_diag_ctx* diag) {
    struct bx_tar_pending_link link = {0};
    struct stat parent;
    if (fstat(state->parent_fd, &parent) != 0) {
        bx_diag(diag, "%s: %s", path, strerror(errno));
        return false;
    }
    if (!bx_tar_extract_select_metadata(state, entry, mode, &link.restore, path, diag)) {
        bx_file_metadata_free(&link.restore.metadata);
        return false;
    }
    size_t bytes = sizeof(link) + strlen(path) + strlen(target) + strlen(state->leaf) + 3u;
    const struct bx_file_metadata* metadata = &link.restore.metadata;
    size_t values = bx_file_metadata_value_bytes(metadata);
    const size_t limit = 16u * 1024u * 1024u;
    bool oversized = bytes > limit || metadata->len > limit / sizeof(*metadata->xattrs);
    if (!oversized) {
        size_t slots = metadata->len * sizeof(*metadata->xattrs);
        oversized = slots > limit - bytes || values > limit - bytes - slots;
        if (!oversized)
            bytes += slots + values;
    }
    if (state->link_count >= 4096u || oversized || bytes > 16u * 1024u * 1024u - state->link_bytes) {
        bx_file_metadata_free(&link.restore.metadata);
        bx_diag(diag, "%s: unresolved hard-link limit exceeded (4096 links, 16 MiB)", path);
        return false;
    }
    link.path = xstrdup(path);
    link.target = xstrdup(target);
    link.leaf = xstrdup(state->leaf);
    link.parent_dev = parent.st_dev;
    link.parent_ino = parent.st_ino;
    link.mtime = entry->mtime;
    link.omit_mtime = entry->omit_mtime;
    link.bytes = bytes;
    link.sequence = state->sequence;
    link.boundary_prefix = state->boundary_prefix;
    state->links = xrealloc(state->links, (state->link_count + 1u) * sizeof(*state->links));
    state->links[state->link_count++] = link;
    state->link_bytes += bytes;
    return true;
}

/* Resolver failures that a later archive member can repair remain pending. */
static bool bx_tar_link_missing(int error) {
    return error == ENOENT || error == ENOTDIR || error == ELOOP;
}

static bool bx_tar_extract_resolve_links(struct bx_tar_extract_state* state, struct bx_diag_ctx* diag) {
    if (!state->restored.len)
        return true;
    bool progress;
    do {
        progress = false;
        for (size_t i = 0; i < state->link_count;) {
            if (++state->link_checks > 1048576u) {
                bx_diag(diag, "hard-link resolution work limit exceeded (1048576 checks)");
                return false;
            }
            if (bx_archive_temp_pending_signal()) {
                bx_diag(diag, "hard-link resolution interrupted");
                return false;
            }
            struct bx_tar_pending_link* link = &state->links[i];
            char* leaf = NULL;
            int parent = bx_dir_path_open_destination_parent_from(state->root_fd, link->path, link->boundary_prefix, state->dirs.path_policy, false, 0, &leaf);
            struct stat parent_status;
            bool stale = parent < 0 && bx_tar_link_missing(errno);
            if (parent >= 0 && fstat(parent, &parent_status) == 0)
                stale = parent_status.st_dev != link->parent_dev || parent_status.st_ino != link->parent_ino;
            else if (!stale) {
                bx_diag(diag, "%s: %s", link->path, strerror(errno));
                bx_fd_cleanup(&parent);
                free(leaf);
                return false;
            }
            if (stale) {
                bx_fd_cleanup(&parent);
                free(leaf);
                bx_tar_extract_drop_link(state, i);
                progress = true;
                continue;
            }
            char* source_leaf = NULL;
            int source_parent = bx_dir_path_open_destination_parent_from(state->root_fd, link->target, link->boundary_prefix, state->dirs.path_policy, false, 0, &source_leaf);
            int source = source_parent < 0 ? -1 : bx_fd_openat_cloexec(source_parent, source_leaf, O_PATH | O_NOFOLLOW, 0);
            int error = errno;
            struct stat expected;
            bool waiting = source < 0 && bx_tar_link_missing(error);
            bool ok = source >= 0 && fstat(source, &expected) == 0;
            uint64_t sequence = 0, origin = 0;
            if (ok && !bx_inode_ledger_lookup(&state->restored, &expected, &sequence, &origin))
                waiting = true;
            if (waiting) {
                bx_fd_cleanup(&source);
                bx_fd_cleanup(&source_parent);
                free(source_leaf);
                bx_fd_cleanup(&parent);
                free(leaf);
                i++;
                continue;
            }
            if (!ok) {
                if (source >= 0)
                    error = errno;
                bx_diag(diag, "%s: %s", link->target, strerror(error));
            }
            if (ok) {
                struct bx_tar_entry entry = {.kind = BX_TAR_KIND_HARDLINK, .name = link->path, .mtime = link->mtime, .omit_mtime = link->omit_mtime};
                int saved_parent = state->parent_fd;
                char* saved_leaf = state->leaf;
                state->parent_fd = parent;
                state->leaf = leaf;
                enum bx_tar_existing_target_action action = bx_tar_extract_existing_target_action(state, &entry, link->path, diag);
                if (action == BX_TAR_EXISTING_TARGET_ERROR)
                    ok = false;
                else if (action == BX_TAR_EXISTING_TARGET_PROCEED) {
                    struct stat destination;
                    bool already_linked = fstatat(parent, leaf, &destination, AT_SYMLINK_NOFOLLOW) == 0 && destination.st_dev == expected.st_dev && destination.st_ino == expected.st_ino &&
                                          (destination.st_mode & S_IFMT) == (expected.st_mode & S_IFMT);
                    ok = already_linked || bx_tar_extract_prepare_final_non_dir_target(state, &entry, link->path, diag);
                    if (ok && !already_linked && bx_fd_linkat_child(source_parent, source_leaf, parent, leaf, 0) != 0) {
                        bx_diag(diag, "%s: %s", link->path, strerror(errno));
                        ok = false;
                    }
                    if (ok) {
                        int fd = bx_fd_openat_metadata(parent, leaf);
                        struct stat linked;
                        ok = fd >= 0 && bx_fd_fstat_expected(fd, &expected, &linked) == 0;
                        if (!ok)
                            bx_diag(diag, "%s: %s", link->path, strerror(errno));
                        if (ok && (link->sequence > sequence || (state->options->metadata.file_flags && S_ISREG(linked.st_mode) && link->sequence >= origin))) {
                            if (state->options->metadata.file_flags && S_ISREG(linked.st_mode)) {
                                ok = bx_archive_pending_metadata_record_fd(&state->dirs, fd, link->path, link->boundary_prefix, &link->restore, link->sequence, origin);
                                if (!ok)
                                    bx_diag(diag, "%s: cannot defer metadata: %s", link->path, strerror(errno));
                            }
                            else
                                ok = bx_archive_restore_fd(&link->restore, fd, link->path, S_ISLNK(linked.st_mode), false, diag);
                        }
                        if (ok && link->sequence > sequence)
                            ok = bx_inode_ledger_record(&state->restored, &linked, link->sequence, origin, 1048576u);
                        if (fd >= 0 && !bx_fd_close(&fd, link->path, diag))
                            ok = false;
                    }
                }
                state->parent_fd = saved_parent;
                state->leaf = saved_leaf;
            }
            bx_fd_cleanup(&parent);
            free(leaf);
            bx_fd_cleanup(&source);
            bx_fd_cleanup(&source_parent);
            free(source_leaf);
            if (!ok)
                return false;
            bx_tar_extract_drop_link(state, i);
            progress = true;
        }
    } while (progress && state->link_count);
    return true;
}

static bool bx_tar_extract_one_entry_impl(struct bx_tar_extract_state* state,
                                     const struct bx_tar_entry* entry,
                                     struct bx_diag_ctx* diag) {
    struct bx_tar_mapped_name clean_name = {0};
    char* dest_path = NULL;
    const char* extract_dir = NULL;
    const char* report_name;
    bool stripped_absolute;
    bool stripped_dotdot;
    mode_t extract_mode = 0;

    if (state->sequence == UINT64_MAX) {
        bx_diag(diag, "archive member sequence overflow");
        return false;
    }
    state->sequence++;

    if (!bx_tar_starting_file_gate_reached(&state->starting_file_reached,
                                           state->options->starting_file,
                                           entry)) {
        bx_tar_extract_clear_current_stream(state);
        return true;
    }

    if (!bx_tar_select_plan_match_occurrence(state->select_plan,
                                             entry->name,
                                             state->select_plan->len == 0u,
                                             state->matched_members,
                                             state->options->occurrence,
                                             state->occurrence_counts,
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
    if (entry->kind == BX_TAR_KIND_DIR && clean_name.text[0] == '\0'
        && state->name_policy.strip_components == 0u
        && state->name_policy.one_top_level == NULL
        && (strcmp(entry->name, ".") == 0 || strcmp(entry->name, "./") == 0)) {
        bx_tar_release_mapped_name(&clean_name);
        clean_name.text = ".";
    }
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

    extract_mode = bx_tar_extract_mode(state, entry->mode);

    dest_path = extract_dir ? bx_path_join(extract_dir, clean_name.text) : xstrdup(clean_name.text);
    state->boundary_prefix = extract_dir ? strlen(extract_dir) : 0;
    if (state->boundary_prefix && extract_dir[state->boundary_prefix - 1] != '/')
        state->boundary_prefix++;
    state->parent_fd = bx_dir_path_open_destination_parent_from(state->root_fd, dest_path, state->boundary_prefix, state->dirs.path_policy, true, 0777u, &state->leaf);
    if (state->parent_fd < 0) {
        bx_diag(diag, "%s: %s", dest_path, strerror(errno));
        bx_tar_release_mapped_name(&clean_name);
        free(dest_path);
        return false;
    }

    {
        enum bx_tar_existing_target_action existing_action =
            bx_tar_extract_existing_target_action(state, entry, dest_path, diag);

        if (existing_action == BX_TAR_EXISTING_TARGET_ERROR) {
            bx_tar_release_mapped_name(&clean_name);
            free(dest_path);
            bx_tar_extract_clear_current_stream(state);
            return false;
        }
        if (existing_action == BX_TAR_EXISTING_TARGET_SKIP) {
            bx_tar_release_mapped_name(&clean_name);
            free(dest_path);
            bx_tar_extract_clear_current_stream(state);
            return true;
        }
    }
    if (!bx_tar_extract_cancel_links(state, diag)) {
        bx_tar_release_mapped_name(&clean_name);
        free(dest_path);
        return false;
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

    if (entry->kind == BX_TAR_KIND_DIR) {
        bool mkdir_needed = false;

        bool created = mkdirat(state->parent_fd, state->leaf, 0777u) == 0;
        if (!created) {
            if (errno != EEXIST) {
                bx_diag(diag, "%s: %s", dest_path, strerror(errno));
                free(dest_path);
                return false;
            }
            if (!bx_tar_extract_prepare_final_dir_target(state, dest_path, &mkdir_needed, diag)) {
                free(dest_path);
                return false;
            }
            if (mkdir_needed && mkdirat(state->parent_fd, state->leaf, 0777u) != 0) {
                bx_diag(diag, "%s: %s", dest_path, strerror(errno));
                free(dest_path);
                return false;
            }
        }
        if (created || mkdir_needed
            || (state->options->old_file_mode != BX_TAR_OLD_FILES_KEEP
                && state->options->old_file_mode != BX_TAR_OLD_FILES_SKIP)) {
            int fd = bx_fd_openat_cloexec(state->parent_fd, state->leaf,
                                           O_RDONLY | O_DIRECTORY | O_NOFOLLOW, 0);
            struct bx_file_restore restore = {0};
            bool selected = bx_tar_extract_select_metadata(state, entry, extract_mode, &restore, dest_path, diag);
            bool recorded = selected && fd >= 0 && bx_archive_pending_metadata_record_fd(&state->dirs, fd, dest_path, state->boundary_prefix, &restore, state->sequence, 0u);
            int error = errno;
            bx_file_metadata_free(&restore.metadata);
            bx_fd_cleanup(&fd);
            if (!recorded) {
                errno = error;
                if (selected)
                    bx_diag(diag, "%s: cannot defer metadata: %s", dest_path, strerror(errno));
                free(dest_path);
                return false;
            }
        }
        free(dest_path);
        bx_tar_extract_clear_current_stream(state);
        return true;
    }

    if (entry->kind == BX_TAR_KIND_REG) {
        bx_tar_extract_clear_current_stream(state);
        if (bx_fd_staged_file_begin(&state->current_file, state->parent_fd, state->leaf, 0600u) != 0) {
            bx_diag(diag, "%s: %s", dest_path, strerror(errno));
            free(dest_path);
            return false;
        }
        state->current_leaf = xstrdup(state->leaf);
        state->current_dest_path = dest_path;
        state->current_mode_bits = extract_mode;
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
        if (symlinkat(entry->linkname, state->parent_fd, state->leaf) != 0) {
            bx_diag(diag, "%s: %s", dest_path, strerror(errno));
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
        bool ok = bx_tar_extract_queue_link(state, entry, dest_path, target, extract_mode, diag);
        free(target);
        free(dest_path);
        bx_tar_extract_clear_current_stream(state);
        return ok && bx_tar_extract_resolve_links(state, diag);
    }
    else if (entry->kind == BX_TAR_KIND_FIFO) {
        if (!bx_tar_extract_prepare_final_non_dir_target(state, entry, dest_path, diag)) {
            free(dest_path);
            state->status = 2;
            return true;
        }
        if (mkfifoat(state->parent_fd, state->leaf, extract_mode) != 0) {
            bx_diag(diag, "%s: %s", dest_path, strerror(errno));
            free(dest_path);
            return false;
        }
    }
    else if (entry->kind == BX_TAR_KIND_CHAR || entry->kind == BX_TAR_KIND_BLOCK) {
        if (!bx_tar_extract_prepare_final_non_dir_target(state, entry, dest_path, diag)) {
            free(dest_path);
            state->status = 2;
            return true;
        }
        mode_t type = entry->kind == BX_TAR_KIND_CHAR ? S_IFCHR : S_IFBLK;
        if (bx_fd_mknodat(state->parent_fd, state->leaf, type | extract_mode, entry->rdev) != 0) {
            bx_diag(diag, "%s: cannot create device node: %s", dest_path, strerror(errno));
            free(dest_path);
            return false;
        }
    }

    int fd = bx_fd_openat_metadata(state->parent_fd, state->leaf);
    if (fd < 0) {
        bx_diag(diag, "%s: %s", dest_path, strerror(errno));
        free(dest_path);
        return false;
    }
    if (entry->kind == BX_TAR_KIND_CHAR || entry->kind == BX_TAR_KIND_BLOCK) {
        struct stat status;
        mode_t type = entry->kind == BX_TAR_KIND_CHAR ? S_IFCHR : S_IFBLK;
        int rc = fstat(fd, &status);
        if (rc != 0 || (status.st_mode & S_IFMT) != type || status.st_rdev != entry->rdev) {
            bx_diag(diag, "%s: cannot verify device node: %s", dest_path, strerror(rc != 0 ? errno : ESTALE));
            close(fd);
            free(dest_path);
            return false;
        }
    }
    bool ok = bx_tar_extract_metadata(state, entry, fd, dest_path, extract_mode, diag) && bx_tar_extract_record_inode(state, fd, dest_path, diag);
    if (!bx_fd_close(&fd, dest_path, diag))
        ok = false;
    free(dest_path);
    bx_tar_extract_clear_current_stream(state);
    return ok && bx_tar_extract_resolve_links(state, diag);
}

static bool bx_tar_extract_one_entry(struct bx_tar_extract_state* state,
                                     const struct bx_tar_entry* entry,
                                     struct bx_diag_ctx* diag) {
    bool ok = bx_tar_extract_one_entry_impl(state, entry, diag);
    bx_fd_cleanup(&state->parent_fd);
    free(state->leaf);
    state->leaf = NULL;
    return ok;
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
    return bx_archive_write_regular_payload(state->current_file.fd, data, len, false, diag);
}

static bool bx_tar_extract_end_entry(struct bx_tar_extract_state* state,
                                     const struct bx_tar_entry* entry,
                                     struct bx_diag_ctx* diag) {
    char* dest_path = state->current_dest_path;
    int fd = state->current_file.fd;
    mode_t mode = state->current_mode_bits;

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
    if (!bx_tar_extract_metadata(state, entry, fd, dest_path, mode, diag)) {
        bx_tar_extract_clear_current_stream(state);
        return false;
    }
    struct stat candidate;
    if (fstat(fd, &candidate) != 0) {
        bx_diag(diag, "%s: %s", dest_path, strerror(errno));
        bx_tar_extract_clear_current_stream(state);
        return false;
    }
    if (state->restored.len >= 1048576u && !bx_inode_ledger_lookup(&state->restored, &candidate, NULL, NULL)) {
        bx_diag(diag, "%s: cannot record restored inode: %s", dest_path, strerror(E2BIG));
        bx_tar_extract_clear_current_stream(state);
        return false;
    }

    if (bx_archive_temp_pending_signal()) {
        bx_diag(diag, "extraction interrupted");
        bx_tar_extract_clear_current_stream(state);
        return false;
    }
    struct stat existing;
    int found = fstatat(state->current_file.parent_fd, state->current_leaf, &existing, AT_SYMLINK_NOFOLLOW);
    if (found != 0 && errno != ENOENT) {
        bx_diag(diag, "%s: %s", dest_path, strerror(errno));
        bx_tar_extract_clear_current_stream(state);
        return false;
    }
    if (found == 0 && S_ISDIR(existing.st_mode)) {
        if (!state->options->recursive_unlink && state->options->old_file_mode == BX_TAR_OLD_FILES_OVERWRITE) {
            bx_diag(diag, "%s: %s", dest_path, strerror(EISDIR));
            state->status = 2;
            bx_tar_extract_clear_current_stream(state);
            return true;
        }
        bool removed = state->options->recursive_unlink ? bx_remove_recursive_at(state->current_file.parent_fd, state->current_leaf, dest_path, &existing, diag)
                                                        : bx_fd_unlinkat_child(state->current_file.parent_fd, state->current_leaf, AT_REMOVEDIR) == 0;
        if (!removed) {
            int error = errno == ENOTEMPTY || errno == EEXIST ? EEXIST : errno;
            bx_diag(diag, "%s: %s", dest_path, strerror(error));
            state->status = 2;
            bx_tar_extract_clear_current_stream(state);
            return true;
        }
    }
    if (bx_fd_staged_file_publish(&state->current_file, state->current_leaf) != 0) {
        bx_diag(diag, "%s: %s", dest_path, strerror(errno));
        bx_tar_extract_clear_current_stream(state);
        return false;
    }
    bool recorded = bx_inode_ledger_record(&state->restored, &candidate, state->sequence, state->sequence, 1048576u);
    if (!recorded)
        bx_diag(diag, "%s: cannot record restored inode: %s", dest_path, strerror(errno));
    bx_tar_extract_clear_current_stream(state);
    return recorded && bx_tar_extract_resolve_links(state, diag);
}

static int bx_tar_extract_finish(struct bx_tar_extract_state* state,
                                 struct bx_diag_ctx* diag) {
    if (bx_tar_select_plan_report_unmatched_occurrence(state->select_plan,
                                                       state->matched_members,
                                                       state->options->occurrence,
                                                       state->occurrence_counts,
                                                       diag)) {
        state->status = 2;
    }
    if (!bx_tar_extract_resolve_links(state, diag))
        return 2;
    if (state->link_count) {
        for (size_t i = 0; i < state->link_count; i++)
            bx_diag(diag, "%s: unresolved hard link to '%s' (target not restored)", state->links[i].path, state->links[i].target);
        return 2;
    }
    if (!bx_archive_pending_metadata_apply(&state->dirs, state->root_fd, diag)) {
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
    if (!bx_tar_select_plan_match_occurrence(state->select_plan,
                                             entry->name,
                                             state->select_plan->len == 0u,
                                             state->matched_members,
                                             state->options->occurrence,
                                             state->occurrence_counts,
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
    if (state->options->verbose_count >= 2u
        && !bx_tar_report_metadata_line(state->report_stream, entry, diag)) {
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
    if (bx_tar_select_plan_report_unmatched_occurrence(state->select_plan,
                                                       state->matched_members,
                                                       state->options->occurrence,
                                                       state->occurrence_counts,
                                                       diag)) {
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

static int bx_tar_process_archive_stream(const struct bx_tar_options* options,
                                         const struct bx_tar_select_plan* select_plan,
                                         struct bx_diag_ctx* diag) {
    struct bx_tar_reader_stream_options reader_options = {
        .archive_path = options->archive_path,
        .required_codec = bx_tar_input_required_codec(options),
        .seek_mode = options->seek_mode,
        .skip_owner_group_names = true,
        .skip_owner_group_ids = options->mode == BX_TAR_MODE_LIST && options->verbose_count < 2u,
    };
    struct bx_tar_report_output report_output = {0};
    bool need_report_output = options->mode == BX_TAR_MODE_LIST
        || options->verbose_reports
        || (options->mode == BX_TAR_MODE_EXTRACT && options->report_block_numbers);
    FILE* report_default_stream = (options->mode == BX_TAR_MODE_LIST
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
        if (!options->to_stdout) {
            char* cwd = bx_path_getcwd_dup();
            if (cwd == NULL) {
                bx_diag(diag, ".: %s", strerror(errno));
                bx_tar_extract_state_cleanup(&state);
                bx_tar_report_output_cleanup(&report_output);
                return 2;
            }
            state.dirs.root_depth = bx_dir_path_depth(cwd, 0);
            free(cwd);
            state.root_fd = bx_fd_open_cloexec(".", O_PATH | O_DIRECTORY, 0);
            if (state.root_fd < 0) {
                bx_diag(diag, ".: %s", strerror(errno));
                bx_tar_extract_state_cleanup(&state);
                bx_tar_report_output_cleanup(&report_output);
                return 2;
            }
        }
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

static const char* bx_tar_occurrence_mode_option(enum bx_tar_mode mode) {
    switch (mode) {
        case BX_TAR_MODE_CREATE:
            return "-c";
        case BX_TAR_MODE_NONE:
        case BX_TAR_MODE_LIST:
        case BX_TAR_MODE_EXTRACT:
            return NULL;
    }
    return NULL;
}

static bool bx_tar_report_missing_mode(const struct bx_diag_ctx* diag) {
    fprintf(stderr,
            "%s: You must specify one of the '-ctx' options\n",
            diag->progname);
    fprintf(stderr,
            "Try '%s --help' or '%s --usage' for more information.\n",
            diag->progname,
            diag->progname);
    return false;
}

static bool bx_tar_report_mode_conflict(const struct bx_diag_ctx* diag) {
    fprintf(stderr,
            "%s: You may not specify more than one '-ctx' option\n",
            diag->progname);
    fprintf(stderr,
            "Try '%s --help' or '%s --usage' for more information.\n",
            diag->progname,
            diag->progname);
    return false;
}

static bool bx_tar_set_mode_option(struct bx_tar_options* options,
                                   enum bx_tar_mode mode,
                                   struct bx_diag_ctx* diag) {
    if (options->saw_mode_option) {
        return bx_tar_report_mode_conflict(diag);
    }

    options->saw_mode_option = true;
    options->mode = mode;
    return true;
}

static bool bx_tar_parse_owner_option(const char* value,
                                      uid_t* owner_out,
                                      struct bx_diag_ctx* diag) {
    uintmax_t parsed = 0u;
    if (!bx_id_parse_numeric(value, (uintmax_t)((uid_t)-1), &parsed)) {
        bx_diag(diag, "invalid owner '%s'", value);
        return false;
    }

    *owner_out = (uid_t)parsed;
    return true;
}

static bool bx_tar_parse_group_option(const char* value,
                                      gid_t* group_out,
                                      struct bx_diag_ctx* diag) {
    uintmax_t parsed = 0u;
    if (!bx_id_parse_numeric(value, (uintmax_t)((gid_t)-1), &parsed)) {
        bx_diag(diag, "invalid group '%s'", value);
        return false;
    }

    *group_out = (gid_t)parsed;
    return true;
}

static bool bx_tar_set_old_file_mode(struct bx_tar_options* options,
                                     enum bx_tar_old_file_mode mode,
                                     const char* name,
                                     struct bx_diag_ctx* diag) {
    if (options->old_file_mode != BX_TAR_OLD_FILES_DEFAULT
        && options->old_file_mode != mode) {
        bx_diag(diag,
                "'%s' cannot be used with '%s'",
                name,
                options->old_file_mode_name);
        return false;
    }
    options->old_file_mode = mode;
    options->old_file_mode_name = name;
    return true;
}

static unsigned int bx_tar_option_modes(enum bx_tar_option_effect effect) {
    const unsigned int create = 1u << BX_TAR_MODE_CREATE;
    const unsigned int list = 1u << BX_TAR_MODE_LIST;
    const unsigned int extract = 1u << BX_TAR_MODE_EXTRACT;

    switch (effect) {
        case BX_TAR_OPT_FORMAT:
        case BX_TAR_OPT_SORT:
        case BX_TAR_OPT_MTIME:
        case BX_TAR_OPT_MODE:
        case BX_TAR_OPT_OWNER:
        case BX_TAR_OPT_GROUP:
        case BX_TAR_OPT_NUMERIC_OWNER:
        case BX_TAR_OPT_NEWER:
        case BX_TAR_OPT_NEWER_MTIME:
        case BX_TAR_OPT_IGNORE_FAILED_READ:
        case BX_TAR_OPT_EXCLUDE_CACHES:
        case BX_TAR_OPT_EXCLUDE_CACHES_ALL:
        case BX_TAR_OPT_EXCLUDE_CACHES_UNDER:
        case BX_TAR_OPT_EXCLUDE_IGNORE:
        case BX_TAR_OPT_EXCLUDE_IGNORE_RECURSIVE:
        case BX_TAR_OPT_EXCLUDE_TAG:
        case BX_TAR_OPT_EXCLUDE_TAG_ALL:
        case BX_TAR_OPT_EXCLUDE_TAG_UNDER:
        case BX_TAR_OPT_EXCLUDE_VCS_IGNORES:
        case BX_TAR_OPT_THREADS:
        case BX_TAR_OPT_COMPRESS_THREADS:
        case BX_TAR_OPT_MT_CHUNK_SIZE:
        case BX_TAR_OPT_NO_MT:
            return create;
        case BX_TAR_OPT_TO_STDOUT:
        case BX_TAR_OPT_TOUCH_MTIME_ON:
        case BX_TAR_OPT_OWNER_RESTORE_ON:
        case BX_TAR_OPT_OWNER_RESTORE_OFF:
        case BX_TAR_OPT_PERMISSIONS_ON:
        case BX_TAR_OPT_PERMISSIONS_OFF:
        case BX_TAR_OPT_KEEP_OLD_FILES:
        case BX_TAR_OPT_SKIP_OLD_FILES:
        case BX_TAR_OPT_KEEP_NEWER_FILES:
        case BX_TAR_OPT_OVERWRITE:
        case BX_TAR_OPT_UNLINK_FIRST:
        case BX_TAR_OPT_RECURSIVE_UNLINK:
            return extract;
        case BX_TAR_OPT_SEEK_ON:
        case BX_TAR_OPT_SEEK_OFF:
        case BX_TAR_OPT_STARTING_FILE:
        case BX_TAR_OPT_STRIP_COMPONENTS:
        case BX_TAR_OPT_ONE_TOP_LEVEL:
        case BX_TAR_OPT_REPORT_MAPPED_NAMES:
        case BX_TAR_OPT_BLOCK_NUMBER:
            return list | extract;
        case BX_TAR_OPT_XATTRS_ON:
        case BX_TAR_OPT_XATTRS_OFF:
        case BX_TAR_OPT_XATTRS_INCLUDE:
        case BX_TAR_OPT_XATTRS_EXCLUDE:
        case BX_TAR_OPT_ACLS_ON:
        case BX_TAR_OPT_ACLS_OFF:
        case BX_TAR_OPT_SELINUX_ON:
        case BX_TAR_OPT_SELINUX_OFF:
            return create | extract;
        default:
            return create | list | extract;
    }
}

static bool bx_tar_apply_option_effect(struct bx_tar_options* options,
                                       enum bx_tar_option_effect effect,
                                       const char* display,
                                       const char* value,
                                       struct bx_diag_ctx* diag) {
    unsigned int modes = bx_tar_option_modes(effect);
    for (unsigned int mode = BX_TAR_MODE_CREATE; mode <= BX_TAR_MODE_EXTRACT; mode++) {
        if (!(modes & (1u << mode)) && options->invalid_mode_option[mode] == NULL)
            options->invalid_mode_option[mode] = display;
    }
    if ((modes == (1u << BX_TAR_MODE_EXTRACT) && effect != BX_TAR_OPT_TO_STDOUT) || modes == ((1u << BX_TAR_MODE_CREATE) | (1u << BX_TAR_MODE_EXTRACT))) {
        if (options->filesystem_option == NULL)
            options->filesystem_option = display;
    }
    if (effect == BX_TAR_OPT_THREADS || effect == BX_TAR_OPT_COMPRESS_THREADS || effect == BX_TAR_OPT_MT_CHUNK_SIZE || effect == BX_TAR_OPT_NO_MT) {
        if (options->gzip_output_option == NULL)
            options->gzip_output_option = display;
        if (effect == BX_TAR_OPT_MT_CHUNK_SIZE)
            options->mt_chunk_size_set = true;
    }
    switch (effect) {
        case BX_TAR_OPT_OWNER_RESTORE_OFF:
        case BX_TAR_OPT_PERMISSIONS_OFF:
        case BX_TAR_OPT_XATTRS_OFF:
        case BX_TAR_OPT_ACLS_OFF:
        case BX_TAR_OPT_SELINUX_OFF:
        case BX_TAR_OPT_XATTRS_INCLUDE:
        case BX_TAR_OPT_XATTRS_EXCLUDE:
        case BX_TAR_OPT_TOUCH_MTIME_ON:
        case BX_TAR_OPT_IGNORE_FAILED_READ:
        case BX_TAR_OPT_TO_STDOUT:
        case BX_TAR_OPT_OWNER:
        case BX_TAR_OPT_GROUP:
        case BX_TAR_OPT_MODE:
        case BX_TAR_OPT_MTIME:
        case BX_TAR_OPT_KEEP_OLD_FILES:
        case BX_TAR_OPT_SKIP_OLD_FILES:
        case BX_TAR_OPT_KEEP_NEWER_FILES:
            if (!options->preservation_conflict)
                options->preservation_conflict = display;
            break;
        case BX_TAR_OPT_FORMAT:
            if (!options->preservation_conflict && strcmp(value, "ustar") == 0)
                options->preservation_conflict = display;
            break;
        default:
            break;
    }
    switch (effect) {
        case BX_TAR_OPT_INVALID:
            return false;
        case BX_TAR_OPT_SPARSE:
            options->sparse = true;
            return true;
        case BX_TAR_OPT_SPARSE_VERSION:
        case BX_TAR_OPT_HOLE_DETECTION:
            if (strcmp(value, effect == BX_TAR_OPT_SPARSE_VERSION ? "1.0" : "seek") != 0) {
                bx_diag(diag, "%s: unsupported value '%s'", display, value);
                return false;
            }
            options->sparse_selectors = true;
            return true;
        case BX_TAR_OPT_MODE_CREATE:
            return bx_tar_set_mode_option(options, BX_TAR_MODE_CREATE, diag);
        case BX_TAR_OPT_MODE_LIST:
            if (!bx_tar_set_mode_option(options, BX_TAR_MODE_LIST, diag)) {
                return false;
            }
            if (options->verbose_count < 3u) {
                options->verbose_count++;
            }
            return true;
        case BX_TAR_OPT_MODE_EXTRACT:
            return bx_tar_set_mode_option(options, BX_TAR_MODE_EXTRACT, diag);
        case BX_TAR_OPT_ARCHIVE_PATH:
            options->archive_path = value;
            return true;
        case BX_TAR_OPT_DIRECTORY:
            return bx_tar_create_options_add_chdir(&options->create_options, value);
        case BX_TAR_OPT_TO_STDOUT:
            options->to_stdout = true;
            return true;
        case BX_TAR_OPT_KEEP_OLD_FILES:
            return bx_tar_set_old_file_mode(options,
                                            BX_TAR_OLD_FILES_KEEP,
                                            "--keep-old-files",
                                            diag);
        case BX_TAR_OPT_OVERWRITE:
            return bx_tar_set_old_file_mode(options,
                                            BX_TAR_OLD_FILES_OVERWRITE,
                                            "--overwrite",
                                            diag);
        case BX_TAR_OPT_UNLINK_FIRST:
            return bx_tar_set_old_file_mode(options,
                                            BX_TAR_OLD_FILES_UNLINK_FIRST,
                                            "--unlink-first",
                                            diag);
        case BX_TAR_OPT_SKIP_OLD_FILES:
            return bx_tar_set_old_file_mode(options,
                                            BX_TAR_OLD_FILES_SKIP,
                                            "--skip-old-files",
                                            diag);
        case BX_TAR_OPT_KEEP_NEWER_FILES:
            return bx_tar_set_old_file_mode(options,
                                            BX_TAR_OLD_FILES_KEEP_NEWER,
                                            "--keep-newer-files",
                                            diag);
        case BX_TAR_OPT_RECURSIVE_UNLINK:
            options->recursive_unlink = true;
            return true;
        case BX_TAR_OPT_INDEX_FILE:
            free(options->index_file_path);
            options->index_file_path = xstrdup(value);
            return true;
        case BX_TAR_OPT_VERBOSE:
            options->verbose_reports = true;
            if (options->verbose_count < 3u) {
                options->verbose_count++;
            }
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
        case BX_TAR_OPT_IGNORE_FAILED_READ:
            options->create_options.ignore_failed_read = true;
            return true;
        case BX_TAR_OPT_OCCURRENCE: {
            uintmax_t parsed = 1u;

            if (value != NULL
                && (!bx_size_parse_uint(value, &parsed)
                    || parsed > (uintmax_t)INTMAX_MAX)) {
                bx_diag(diag, "%s: Invalid number", value != NULL ? value : "");
                return false;
            }
            options->occurrence = parsed;
            return true;
        }
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
        case BX_TAR_OPT_SEEK_ON:
            options->seek_mode = BX_ARCHIVE_CODEC_SEEK_FORCE;
            return true;
        case BX_TAR_OPT_SEEK_OFF:
            options->seek_mode = BX_ARCHIVE_CODEC_SEEK_DISABLE;
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
            size_t parsed = 0u;
            if (!bx_args_parse_size_range(value, 0u, (size_t)-1, &parsed)) {
                bx_diag(diag, "invalid number of components '%s'", value);
                return false;
            }
            options->strip_components = parsed;
            return true;
        }
        case BX_TAR_OPT_ONE_TOP_LEVEL:
            options->one_top_level = value;
            return true;
        case BX_TAR_OPT_FORMAT:
            if (strcmp(value, "ustar") != 0 && strcmp(value, "pax") != 0
                && strcmp(value, "posix") != 0) {
                bx_diag(diag, "unsupported format '%s'", value);
                return false;
            }
            options->format_ustar = strcmp(value, "ustar") == 0;
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
            if (!bx_tar_parse_owner_option(value, &options->owner, diag)) {
                return false;
            }
            options->owner_set = true;
            return true;
        case BX_TAR_OPT_GROUP:
            if (!bx_tar_parse_group_option(value, &options->group, diag)) {
                return false;
            }
            options->group_set = true;
            return true;
        case BX_TAR_OPT_OWNER_RESTORE_ON:
            options->owner_policy = BX_TAR_OWNER_FORCE;
            return true;
        case BX_TAR_OPT_OWNER_RESTORE_OFF:
            options->owner_policy = BX_TAR_OWNER_DISABLE;
            return true;
        case BX_TAR_OPT_PERMISSIONS_ON:
            options->permission_policy = BX_TAR_PERMISSIONS_FORCE;
            return true;
        case BX_TAR_OPT_PERMISSIONS_OFF:
            options->permission_policy = BX_TAR_PERMISSIONS_DISABLE;
            return true;
        case BX_TAR_OPT_XATTRS_ON:
            options->format_ustar = false;
            options->metadata.xattrs = true;
            return true;
        case BX_TAR_OPT_ONE_FILE_SYSTEM:
            options->create_options.one_file_system = true;
            return true;
        case BX_TAR_OPT_XATTRS_OFF:
            options->metadata.xattrs = false;
            return true;
        case BX_TAR_OPT_ACLS_ON:
            options->format_ustar = false;
            options->metadata.acls = true;
            return true;
        case BX_TAR_OPT_ACLS_OFF:
            options->metadata.acls = false;
            return true;
        case BX_TAR_OPT_FILE_FLAGS:
            options->metadata.file_flags = true;
            return true;
        case BX_TAR_OPT_PRESERVE_ALL:
            options->preserve_all = true;
            return true;
        case BX_TAR_OPT_SELINUX_ON:
            options->format_ustar = false;
            options->metadata.selinux = true;
            return true;
        case BX_TAR_OPT_SELINUX_OFF:
            options->metadata.selinux = false;
            return true;
        case BX_TAR_OPT_XATTRS_INCLUDE:
        case BX_TAR_OPT_XATTRS_EXCLUDE:
            options->format_ustar = false;
            options->metadata.xattrs = true;
            return bx_archive_name_list_append(effect == BX_TAR_OPT_XATTRS_INCLUDE
                                                  ? &options->metadata.include
                                                  : &options->metadata.exclude, value);
    }

    return true;
}

static void bx_tar_options_cleanup(struct bx_tar_options* options) {
    bx_archive_name_list_free(&options->metadata.include);
    bx_archive_name_list_free(&options->metadata.exclude);
    bx_tar_create_options_cleanup(&options->create_options);
    free(options->index_file_path);
    options->index_file_path = NULL;
    free(options->mode_text);
    options->mode_text = NULL;
}

static bool bx_tar_add_operand(struct bx_tar_options* options, const char* operand) {
    return bx_tar_create_options_add_add_file(&options->create_options, operand);
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
};

static bool bx_tar_create_stream_entries_produce(void* user,
                                                 bx_archive_fs_visit_fn visit_fn,
                                                 void* visit_user_data,
                                                 struct bx_diag_ctx* diag) {
    struct bx_tar_create_stream_producer_ctx* ctx = user;
    bool had_create_errors = false;

    if (!bx_tar_create_visit_fs_entries(ctx->create_options,
                                          ctx->sort_children,
                                          visit_fn,
                                          visit_user_data,
                                          &had_create_errors,
                                          diag)) {
        return false;
    }
    /* Skipped required inputs invalidate the producer, before any writer commits. */
    if (had_create_errors) {
        bx_tar_report_previous_errors(diag);
        return false;
    }
    return true;
}

static bool bx_tar_can_stream_create(const struct bx_tar_options* options) {
    return !options->verbose_reports
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
            if (spec->arg_mode == BX_TAR_OPTARG_NONE && value != NULL) {
                bx_diag(diag, "option '%s' doesn't allow an argument", spec->name);
                return false;
            }

            if (spec->arg_mode == BX_TAR_OPTARG_REQUIRED) {
                if (value == NULL && ++i >= argc) {
                    bx_diag(diag, "option '%s' requires an argument", spec->name);
                    return false;
                }
                parsed_value = value ? value + 1 : argv[i];
            }
            else if (spec->arg_mode == BX_TAR_OPTARG_OPTIONAL && value != NULL) {
                parsed_value = value + 1;
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

    if (options->preserve_all) {
        if (options->preservation_conflict) {
            bx_diag(diag, "--preserve-all cannot be combined with %s", options->preservation_conflict);
            return false;
        }
        if (options->mode != BX_TAR_MODE_CREATE && options->mode != BX_TAR_MODE_EXTRACT) {
            bx_diag(diag, "--preserve-all requires --create or --extract");
            return false;
        }
        options->numeric_owner = true;
        options->owner_policy = BX_TAR_OWNER_FORCE;
        options->permission_policy = BX_TAR_PERMISSIONS_FORCE;
        options->metadata.xattrs = options->metadata.acls = options->metadata.selinux = options->metadata.file_flags = true;
        if (!bx_archive_name_list_append(&options->metadata.include, "*"))
            return false;
    }
    if (options->sparse_selectors && !options->sparse) {
        bx_diag(diag, "sparse selectors require --sparse");
        return false;
    }
    if (options->sparse && options->mode != BX_TAR_MODE_CREATE) {
        bx_diag(diag, "--sparse is supported only for ordinary creation");
        return false;
    }
    if (options->format_ustar && (options->sparse || options->metadata.xattrs || options->metadata.acls || options->metadata.selinux || options->metadata.file_flags)) {
        bx_diag(diag, "metadata requires pax format");
        return false;
    }
    if (options->mode == BX_TAR_MODE_NONE) {
        return bx_tar_report_missing_mode(diag);
    }
    if (options->metadata.file_flags && options->mode != BX_TAR_MODE_CREATE && options->mode != BX_TAR_MODE_EXTRACT) {
        bx_diag(diag, "--file-flags requires --create or --extract");
        return false;
    }
    if (options->metadata.file_flags && options->to_stdout) {
        bx_diag(diag, "--file-flags cannot be combined with --to-stdout");
        return false;
    }
    if (options->create_options.one_file_system
        && options->mode != BX_TAR_MODE_CREATE) {
        bx_diag(diag, "--one-file-system requires creating an archive from files");
        return false;
    }
    if (options->archive_path == NULL) {
        bx_diag(diag, "archive file not specified; use -f");
        return false;
    }
    if (options->occurrence > 0u) {
        const char* mode_option = bx_tar_occurrence_mode_option(options->mode);

        if (mode_option != NULL) {
            bx_diag(diag, "'--occurrence' cannot be used with '%s'", mode_option);
            return false;
        }
    }
    if (options->mode == BX_TAR_MODE_CREATE
        && !bx_tar_create_has_inputs(options, argc)) {
        bx_diag(diag, "missing file operand");
        return false;
    }
    const char* mode_name = options->mode == BX_TAR_MODE_CREATE ? "create" : options->mode == BX_TAR_MODE_LIST ? "list" : "extract";
    if (options->invalid_mode_option[options->mode] != NULL) {
        bx_diag(diag, "%s is not supported with --%s", options->invalid_mode_option[options->mode], mode_name);
        return false;
    }
    if (options->to_stdout && options->filesystem_option != NULL) {
        bx_diag(diag, "%s cannot be combined with --to-stdout", options->filesystem_option);
        return false;
    }
    if (options->gzip_output_option != NULL && bx_tar_output_codec(options) != bx_archive_codec_gzip()) {
        bx_diag(diag, "%s requires gzip output", options->gzip_output_option);
        return false;
    }
    if (options->mt_chunk_size_set && bx_tar_effective_compress_threads(options) <= 1u) {
        bx_diag(diag, "--mt-chunk-size requires multithreaded gzip output");
        return false;
    }
    if (options->mode != BX_TAR_MODE_LIST && !options->verbose_reports) {
        if (options->index_file_path != NULL && !(options->mode == BX_TAR_MODE_EXTRACT && options->report_block_numbers)) {
            bx_diag(diag, "--index-file requires --verbose with --%s", mode_name);
            return false;
        }
        if (options->report_mapped_names) {
            bx_diag(diag, "--show-transformed-names requires --verbose with --extract");
            return false;
        }
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
            bx_tar_options_cleanup(&options);
            return rc;
        }

        struct bx_archive_fs_list files = {0};
        struct bx_tar_report_output report_output = {0};
        bool had_create_errors = false;
        uint64_t total_bytes_written = 0u;
        size_t compress_threads = bx_tar_effective_compress_threads(&options);
        bool use_mt = compress_threads > 1u
            && bx_archive_codec_supports_mt_encode(bx_tar_output_codec(&options));

        if (!bx_tar_create_collect_fs_entries(&files,
                                              &options.create_options,
                                              options.sort_name,
                                              &had_create_errors,
                                              &diag)
            || had_create_errors) {
            if (had_create_errors) {
                bx_tar_report_previous_errors(&diag);
            }
            bx_archive_fs_list_free(&files);
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
        if (!bx_tar_report_output_finish(&report_output, &diag)) {
            rc = 2;
        }
        bx_archive_fs_list_free(&files);
        bx_tar_options_cleanup(&options);
        return rc;
    }
    {
        struct bx_tar_select_plan select_plan = {0};
        bool had_selection_errors = false;
        if (!bx_tar_select_plan_build(&select_plan,
                                      &options.create_options,
                                      &had_selection_errors,
                                      &diag)) {
            bx_tar_options_cleanup(&options);
            return 2;
        }
        if (!bx_tar_validate_occurrence_selection(&options, &select_plan, &diag)) {
            bx_tar_select_plan_cleanup(&select_plan);
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
