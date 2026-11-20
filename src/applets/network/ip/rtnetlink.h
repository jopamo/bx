#ifndef BX_APPLETS_NETWORK_IP_RTNETLINK_H
#define BX_APPLETS_NETWORK_IP_RTNETLINK_H

#ifdef __linux__
#include <linux/rtnetlink.h>
#include <stdbool.h>
#include <stddef.h>

/* Append one attribute to a request with capacity bytes of writable storage. */
bool bx_rtnl_attr(void* buffer, size_t capacity, unsigned short type,
                  const void* data, size_t size);
/* Send one mutation and wait for the matching kernel ACK. No retries of the
 * mutation after an ambiguous result. On failure returns -1 with errno set. */
int bx_rtnl_apply(void* buffer);
#endif

#endif
