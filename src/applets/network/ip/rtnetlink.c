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

bool bx_rtnl_next_attr(const unsigned char** cursor, size_t* left, struct rtattr* attr,
                       const unsigned char** data) {
    if (*left >= sizeof(*attr)) {
        memcpy(attr, *cursor, sizeof(*attr));
        size_t length = RTA_ALIGN(attr->rta_len);
        if (attr->rta_len >= RTA_LENGTH(0) && length <= *left) {
            *data = *cursor + RTA_LENGTH(0);
            *cursor += length;
            *left -= length;
            return true;
        }
    }
    errno = EPROTO;
    return false;
}

static ssize_t rtnl_receive(int fd, void* buffer, size_t capacity) {
    struct sockaddr_nl sender = {0};
    struct iovec vec = {.iov_base = buffer, .iov_len = capacity};
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
    if (msg.msg_flags & MSG_TRUNC) {
        errno = EMSGSIZE;
        return -1;
    }
    if (msg.msg_namelen != sizeof(sender) || sender.nl_family != AF_NETLINK ||
        sender.nl_pid != 0 || sender.nl_groups != 0 || bytes < NLMSG_HDRLEN) {
        errno = EPROTO;
        return -1;
    }
    return bytes;
}

static int rtnl_check_ack(const struct nlmsghdr* header, const struct nlmsghdr* request) {
    if (header->nlmsg_type != NLMSG_ERROR || header->nlmsg_seq != request->nlmsg_seq ||
        header->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr))) {
        errno = EPROTO;
        return -1;
    }
    struct nlmsgerr ack;
    memcpy(&ack, (const unsigned char*)header + NLMSG_HDRLEN, sizeof(ack));
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

static int rtnl_open(void) {
    int fd = bx_fd_socket_cloexec(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0)
        return -1;
    struct timeval receive_timeout = {.tv_sec = 5};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout)) < 0) {
        int error = errno;
        bx_fd_cleanup(&fd);
        errno = error;
    }
    return fd;
}

static int rtnl_send(int fd, struct nlmsghdr* request, unsigned short flags) {
    struct sockaddr_nl kernel = {.nl_family = AF_NETLINK};
    request->nlmsg_flags |= NLM_F_REQUEST | flags;
    request->nlmsg_seq = 1; /* Each call owns its socket. */
    ssize_t sent;
    do {
        sent = sendto(fd, request, request->nlmsg_len, 0,
                      (const struct sockaddr*)&kernel, sizeof(kernel));
    } while (sent < 0 && errno == EINTR);
    if (sent == (ssize_t)request->nlmsg_len)
        return 0;
    if (sent >= 0)
        errno = EIO;
    return -1;
}

int bx_rtnl_apply(void* buffer) {
    int fd = rtnl_open();
    if (fd < 0)
        return -1;
    int rc = -1;
    if (rtnl_send(fd, buffer, NLM_F_ACK) == 0) {
        union {
            struct nlmsghdr alignment;
            unsigned char bytes[4096];
        } response;
        ssize_t bytes = rtnl_receive(fd, response.bytes, sizeof(response.bytes));
        if (bytes >= 0) {
            const struct nlmsghdr* header = &response.alignment;
            if (header->nlmsg_len > (size_t)bytes)
                errno = EPROTO;
            else
                rc = rtnl_check_ack(header, buffer);
        }
    }
    int error = errno;
    bx_fd_cleanup(&fd);
    errno = error;
    return rc;
}

static int rtnl_receive_dump(int fd, const struct nlmsghdr* request, unsigned short type,
                             unsigned int port, int (*visit)(const struct nlmsghdr*, void*),
                             void* context) {
    enum { DUMP_LIMIT = 16 * 1024 * 1024 };
    union {
        struct nlmsghdr alignment;
        unsigned char bytes[32768];
    } response;
    size_t total = 0;
    for (;;) {
        ssize_t received = rtnl_receive(fd, response.bytes, sizeof(response.bytes));
        if (received < 0)
            return -1;
        total += (size_t)received;
        if (total > DUMP_LIMIT) {
            errno = EFBIG;
            return -1;
        }
        size_t left = (size_t)received;
        const unsigned char* cursor = response.bytes;
        while (left) {
            if (left < NLMSG_HDRLEN)
                goto invalid;
            const struct nlmsghdr* header = (const void*)cursor;
            if (header->nlmsg_len < NLMSG_HDRLEN || header->nlmsg_len > left ||
                header->nlmsg_pid != port || header->nlmsg_seq != request->nlmsg_seq)
                goto invalid;
            size_t size = NLMSG_ALIGN(header->nlmsg_len);
            if (size > left)
                goto invalid;
            if (header->nlmsg_flags & NLM_F_DUMP_INTR) {
                errno = EINTR;
                return -1;
            }
            if (header->nlmsg_type == NLMSG_ERROR) {
                if (rtnl_check_ack(header, request) < 0)
                    return -1;
                goto invalid; /* The dump did not request an ACK. */
            }
            if (!(header->nlmsg_flags & NLM_F_MULTI))
                goto invalid;
            if (header->nlmsg_type == NLMSG_DONE) {
                int error;
                if (size != left || header->nlmsg_len < NLMSG_LENGTH(sizeof(error)))
                    goto invalid;
                memcpy(&error, cursor + NLMSG_HDRLEN, sizeof(error));
                if (error > 0 || error == INT_MIN)
                    goto invalid;
                if (!error)
                    return 0;
                errno = -error;
                return -1;
            }
            if (header->nlmsg_type != type)
                goto invalid;
            if (visit(header, context) < 0)
                return -1;
            left -= size;
            cursor += size;
        }
    }
invalid:
    errno = EPROTO;
    return -1;
}

int bx_rtnl_dump(void* buffer, unsigned short type,
                 int (*visit)(const struct nlmsghdr*, void*), void* context) {
    int fd = rtnl_open();
    if (fd < 0)
        return -1;
    int rc = -1;
    if (rtnl_send(fd, buffer, NLM_F_DUMP) == 0) {
        struct sockaddr_nl local;
        socklen_t size = sizeof(local);
        if (getsockname(fd, (struct sockaddr*)&local, &size) == 0) {
            if (size != sizeof(local) || local.nl_family != AF_NETLINK || !local.nl_pid)
                errno = EPROTO;
            else
                rc = rtnl_receive_dump(fd, buffer, type, local.nl_pid, visit, context);
        }
    }
    int error = errno;
    bx_fd_cleanup(&fd);
    errno = error;
    return rc;
}
#else
typedef int bx_rtnetlink_linux_only;
#endif
