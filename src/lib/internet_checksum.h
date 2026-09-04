#ifndef BX_LIB_INTERNET_CHECKSUM_H
#define BX_LIB_INTERNET_CHECKSUM_H

#include <stddef.h>
#include <stdint.h>

uint16_t bx_internet_checksum_host(const void* data, size_t length);

#endif
