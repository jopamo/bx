#ifndef BX_COMMON_FILE_INFO_FMT_H
#define BX_COMMON_FILE_INFO_FMT_H

#include <sys/stat.h>
#include <time.h>

char bx_file_mode_type_char(mode_t mode);
void bx_file_mode_to_string(mode_t mode, char out[11]);
void bx_file_format_ls_timestamp(time_t timestamp, char buffer[32]);

#endif
