#include "lib/file_info_fmt.h"

void bx_rsync_format_mode(mode_t mode, char out[12]);

void
bx_rsync_format_mode(mode_t mode, char out[12])
{
	bx_file_mode_to_string(mode, out);
	out[10] = ' ';
	out[11] = '\0';
}
