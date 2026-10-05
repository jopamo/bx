#include <limits.h>
#include <string.h>

#include "applets/archive/tar/tar_compression.h"
#include "lib/args_common.h"
#include "lib/argv_packer.h"
#include "lib/path_ops.h"

static const struct bx_tar_compressor_alias {
    const char* name;
    const struct bx_archive_codec* (*codec)(void);
} bx_tar_compressor_aliases[] = {
    {"gzip", bx_archive_codec_gzip},
    {"gunzip", bx_archive_codec_gzip},
    {"pigz", bx_archive_codec_gzip},
    {"bzip2", bx_archive_codec_bzip2},
    {"bunzip2", bx_archive_codec_bzip2},
    {"lbzip2", bx_archive_codec_bzip2},
    {"pbzip2", bx_archive_codec_bzip2},
    {"xz", bx_archive_codec_xz},
    {"unxz", bx_archive_codec_xz},
    {"pixz", bx_archive_codec_xz},
    {"zstd", bx_archive_codec_zstd},
    {"unzstd", bx_archive_codec_zstd},
    {"zstdmt", bx_archive_codec_zstd},
    {"pzstd", bx_archive_codec_zstd},
};

static bool bx_tar_compression_direction_set(struct bx_tar_compression_spec* spec,
                                             enum bx_tar_compression_direction direction,
                                             struct bx_diag_ctx* diag) {
    if (spec->direction != BX_TAR_COMPRESSION_AUTO && spec->direction != direction) {
        bx_diag(diag, "conflicting -I compression directions");
        return false;
    }
    spec->direction = direction;
    return true;
}

bool bx_tar_compression_parse(const char* text,
                              struct bx_tar_compression_spec* spec,
                              struct bx_diag_ctx* diag) {
    struct bx_tar_compression_spec parsed = {.threads = -1};
    char** args = NULL;
    bool ok = false;
    if (bx_argv_parse_command(text, &args) != 0) {
        bx_diag(diag, "invalid -I compressor specification");
        return false;
    }
    const char* name = bx_path_basename_ptr(args[0]);
    for (size_t i = 0; i < sizeof(bx_tar_compressor_aliases) / sizeof(bx_tar_compressor_aliases[0]); i++) {
        if (strcmp(name, bx_tar_compressor_aliases[i].name) == 0) {
            parsed.codec = bx_tar_compressor_aliases[i].codec();
            break;
        }
    }
    if (parsed.codec == NULL) {
        bx_diag(diag, "unsupported -I compressor '%s'", name);
        bx_diag(diag, "supported built-in compressors: gzip, bzip2, xz, zstd");
        goto out;
    }
    for (size_t i = 1; args[i] != NULL; i++) {
        const char* arg = args[i];
        if (strcmp(arg, "-c") == 0 || strcmp(arg, "--stdout") == 0)
            continue;
        if (arg[0] == '-' && arg[1] >= '0' && arg[1] <= '9') {
            if (!bx_args_parse_int_range(arg + 1, 0, INT_MAX, &parsed.encode.level)
                || !bx_archive_codec_level_valid(parsed.codec, parsed.encode.level)) {
                bx_diag(diag, "invalid -I %s compression level '%s'", name, arg);
                goto out;
            }
            parsed.encode.level_set = true;
            continue;
        }
        if (strcmp(name, "pigz") == 0
            && (strncmp(arg, "-p", 2) == 0 || strcmp(arg, "--processes") == 0
                || strncmp(arg, "--processes=", 12) == 0)) {
            const char* value = arg[1] == 'p' ? arg + 2
                : arg + (arg[11] == '=' ? 12 : 11);
            if (strcmp(arg, "-p") == 0 || strcmp(arg, "--processes") == 0)
                value = args[++i];
            if (value == NULL || !bx_args_parse_int_range(value, 1, INT_MAX, &parsed.threads)) {
                bx_diag(diag, "invalid -I pigz process count");
                goto out;
            }
            continue;
        }
        if (parsed.codec == bx_archive_codec_xz()
            && (strncmp(arg, "-T", 2) == 0 || strcmp(arg, "--threads") == 0
                || strncmp(arg, "--threads=", 10) == 0)) {
            const char* value = arg[1] == 'T' ? arg + 2
                : arg + (arg[9] == '=' ? 10 : 9);
            if (strcmp(arg, "-T") == 0 || strcmp(arg, "--threads") == 0)
                value = args[++i];
            if (value == NULL || !bx_args_parse_int_range(value, 0, INT_MAX, &parsed.threads)) {
                bx_diag(diag, "invalid -I xz thread count");
                goto out;
            }
            continue;
        }
        enum bx_tar_compression_direction direction = BX_TAR_COMPRESSION_AUTO;
        if (strcmp(arg, "--decompress") == 0 || strcmp(arg, "--uncompress") == 0)
            direction = BX_TAR_COMPRESSION_DECODE;
        else if (strcmp(arg, "--compress") == 0)
            direction = BX_TAR_COMPRESSION_ENCODE;
        else if (arg[0] == '-' && arg[1] != '\0' && arg[1] != '-') {
            bool short_options = true;
            for (const char* ch = arg + 1; *ch != '\0'; ch++) {
                if (*ch != 'c' && *ch != 'd' && *ch != 'z') {
                    short_options = false;
                    break;
                }
            }
            if (short_options) {
                for (const char* ch = arg + 1; *ch != '\0'; ch++) {
                    if (*ch != 'c'
                        && !bx_tar_compression_direction_set(&parsed,
                            *ch == 'd' ? BX_TAR_COMPRESSION_DECODE : BX_TAR_COMPRESSION_ENCODE, diag))
                        goto out;
                }
                continue;
            }
        }
        if (direction != BX_TAR_COMPRESSION_AUTO) {
            if (!bx_tar_compression_direction_set(&parsed, direction, diag))
                goto out;
            continue;
        }
        bx_diag(diag, "unsupported -I %s option '%s'", name, arg);
        goto out;
    }
    *spec = parsed;
    ok = true;
out:
    bx_argv_free(args);
    return ok;
}
