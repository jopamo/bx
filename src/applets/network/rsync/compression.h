#ifndef BX_RSYNC_COMPRESSION_H
#define BX_RSYNC_COMPRESSION_H

#include <stddef.h>
#include <zlib.h>

static inline int
bx_rsync_match_history(z_stream *stream, const Bytef *data, size_t len,
    int (*set_dictionary)(z_stream *, const Bytef *, uInt))
{
	/*
	 * Protocol 29 repeats the start of a match in 65535-byte chunks.
	 * Only the last full chunk and its remainder affect the 32 KiB window.
	 */
	if (len > 65535) {
		int res = set_dictionary(stream, data + 65535 - 32768, 32768);
		if (res != Z_OK)
			return res;
		len %= 65535;
	}
	if (len == 0)
		return Z_OK;
	if (len > 32768) {
		data += len - 32768;
		len = 32768;
	}
	return set_dictionary(stream, data, (uInt)len);
}

#endif
