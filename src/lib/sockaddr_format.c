#include "lib/sockaddr_format.h"

#include <netdb.h>

static int bx_sockaddr_size_to_socklen(size_t size, socklen_t* result) {
    socklen_t converted = (socklen_t)size;
    if ((size_t)converted != size) {
        return -1;
    }
    *result = converted;
    return 0;
}

int bx_sockaddr_format_numeric(
    const struct sockaddr* address,
    socklen_t address_len,
    char* host,
    size_t host_len,
    char* service,
    size_t service_len) {
    socklen_t host_size;
    socklen_t service_size;

    if (address == NULL ||
        (host == NULL && host_len != 0u) ||
        (service == NULL && service_len != 0u) ||
        (host == NULL && service == NULL) ||
        bx_sockaddr_size_to_socklen(host_len, &host_size) != 0 ||
        bx_sockaddr_size_to_socklen(service_len, &service_size) != 0) {
        return EAI_FAIL;
    }

    return getnameinfo(
        address,
        address_len,
        host,
        host_size,
        service,
        service_size,
        NI_NUMERICHOST | NI_NUMERICSERV);
}
