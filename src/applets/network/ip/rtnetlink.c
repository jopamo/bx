#include "rtnetlink.h"

#ifdef __linux__
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "lib/fd_ops.h"

bool bx_rtnl_attr(void* buffer, size_t capacity, unsigned short type,
                  const void* data, size_t size) {
    struct nlmsghdr* request = buffer;
    if (request->nlmsg_len > UINT_MAX - (NLMSG_ALIGNTO - 1)) {
        errno = EMSGSIZE;
        return false;
    }
    size_t end = NLMSG_ALIGN(request->nlmsg_len);
    if (size > USHRT_MAX - RTA_LENGTH(0) || end > capacity ||
        RTA_SPACE(size) > capacity - end || RTA_SPACE(size) > UINT_MAX - end) {
        errno = EMSGSIZE;
        return false;
    }
    unsigned char* attr = (unsigned char*)buffer + end;
    struct rtattr header = {.rta_type = type, .rta_len = (unsigned short)RTA_LENGTH(size)};
    memset(attr, 0, RTA_SPACE(size));
    memcpy(attr, &header, sizeof(header));
    memcpy(attr + RTA_LENGTH(0), data, size);
    request->nlmsg_len = (unsigned int)(end + RTA_SPACE(size));
    return true;
}

static int rtnl_receive_ack(int fd, const struct nlmsghdr* request) {
    /* A single request, no multicast subscriptions and no multipart dump. */
    union {
        struct nlmsghdr alignment;
        unsigned char bytes[4096];
    } response;
    struct sockaddr_nl sender = {0};
    struct iovec vec = {.iov_base = response.bytes, .iov_len = sizeof(response.bytes)};
    struct msghdr msg = {
        .msg_name = &sender, .msg_namelen = sizeof(sender),
        .msg_iov = &vec, .msg_iovlen = 1,
    };
    ssize_t bytes;
    do {
        bytes = recvmsg(fd, &msg, 0);
    } while (bytes < 0 && errno == EINTR);
    if (bytes < 0)
        return -1;
    if (msg.msg_namelen != sizeof(sender) || sender.nl_family != AF_NETLINK ||
        sender.nl_pid != 0 || sender.nl_groups != 0 || (msg.msg_flags & MSG_TRUNC) != 0 ||
        bytes < (ssize_t)NLMSG_LENGTH(sizeof(struct nlmsgerr))) {
        errno = EPROTO;
        return -1;
    }
    struct nlmsghdr header;
    memcpy(&header, response.bytes, sizeof(header));
    if (header.nlmsg_type != NLMSG_ERROR || header.nlmsg_seq != request->nlmsg_seq ||
        header.nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr)) ||
        header.nlmsg_len > (size_t)bytes) {
        errno = EPROTO;
        return -1;
    }
    struct nlmsgerr ack;
    memcpy(&ack, response.bytes + NLMSG_HDRLEN, sizeof(ack));
    if (ack.msg.nlmsg_type != request->nlmsg_type || ack.msg.nlmsg_seq != request->nlmsg_seq ||
        ack.error > 0 || ack.error == INT_MIN) {
        errno = EPROTO;
        return -1;
    }
    if (ack.error == 0)
        return 0;
    errno = -ack.error;
    return -1;
}

int bx_rtnl_apply(void* buffer) {
    struct nlmsghdr* request = buffer;
    int fd = bx_fd_socket_cloexec(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0)
        return -1;

    struct sockaddr_nl kernel = {.nl_family = AF_NETLINK};
    struct timeval receive_timeout = {.tv_sec = 5};
    int rc = -1;
    request->nlmsg_flags |= NLM_F_REQUEST | NLM_F_ACK;
    request->nlmsg_seq = 1; /* Each call owns its socket. */
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout)) == 0) {
        ssize_t sent;
        do {
            sent = sendto(fd, buffer, request->nlmsg_len, 0,
                          (const struct sockaddr*)&kernel, sizeof(kernel));
        } while (sent < 0 && errno == EINTR);
        if (sent == (ssize_t)request->nlmsg_len)
            rc = rtnl_receive_ack(fd, request);
        else if (sent >= 0)
            errno = EIO;
    }
    int error = errno;
    bx_fd_cleanup(&fd);
    errno = error;
    return rc;
}
#else
typedef int bx_rtnetlink_linux_only;
#endif
