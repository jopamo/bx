#ifndef BX_LIB_SOCKADDR_FORMAT_H
#define BX_LIB_SOCKADDR_FORMAT_H

#include <stddef.h>
#include <sys/socket.h>

int bx_sockaddr_format_numeric(
    const struct sockaddr* address,
    socklen_t address_len,
    char* host,
    size_t host_len,
    char* service,
    size_t service_len);

#endif
