#ifndef BX_APPLETS_ARCHIVE_TAR_TAR_COMPRESSION_H
#define BX_APPLETS_ARCHIVE_TAR_TAR_COMPRESSION_H

#include "applets/archive/archive_codec.h"

enum bx_tar_compression_direction {
    BX_TAR_COMPRESSION_AUTO,
    BX_TAR_COMPRESSION_ENCODE,
    BX_TAR_COMPRESSION_DECODE,
};

struct bx_tar_compression_spec {
    const struct bx_archive_codec* codec;
    struct bx_archive_codec_encode_options encode;
    int threads;
    enum bx_tar_compression_direction direction;
};

bool bx_tar_compression_parse(const char* text,
                              struct bx_tar_compression_spec* spec,
                              struct bx_diag_ctx* diag);

#endif
