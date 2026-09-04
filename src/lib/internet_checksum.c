#include "lib/internet_checksum.h"

uint16_t bx_internet_checksum_host(const void* data, size_t length) {
    const unsigned char* bytes = data;
    uint32_t sum = 0;

    while (length >= 2u) {
        sum += ((uint32_t)bytes[0] << 8) | (uint32_t)bytes[1];
        bytes += 2;
        length -= 2u;
    }

    if (length != 0u) {
        sum += (uint32_t)bytes[0] << 8;
    }

    while ((sum >> 16) != 0u) {
        sum = (sum & UINT32_C(0xffff)) + (sum >> 16);
    }

    return (uint16_t)~sum;
}
