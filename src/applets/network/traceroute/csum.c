/*
    Copyright (c)  2006, 2007		Dmitry Butskoy
                                        <dmitry@butskoy.name>
    License:  GPL v2 or any later

    See COPYING for the status of this software.
*/

#include <stdint.h>
#include <stddef.h>
#include <netinet/in.h>  // For htons()

#include "lib/internet_checksum.h"

uint16_t in_csum(const void* ptr, size_t len);

uint16_t in_csum(const void* ptr, size_t len) {
    uint16_t result = htons(bx_internet_checksum_host(ptr, len));
    return result != 0 ? result : UINT16_MAX;
}
