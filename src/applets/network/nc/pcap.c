#include "pcap.h"
#include "netcat.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "lib/fd_ops.h"
#include "lib/poll_deadline.h"

#define PCAP_MAX_PACKET_SIZE 65535
#define PCAP_MAX_PAYLOAD_SIZE 65000
#define PCAP_RING_CAPACITY 128

struct pcap_hdr_s {
    uint32_t magic_number;
    uint16_t version_major;
    uint16_t version_minor;
    int32_t thiszone;
    uint32_t sigfigs;
    uint32_t snaplen;
    uint32_t network;
};

struct pcaprec_hdr_s {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t incl_len;
    uint32_t orig_len;
};

struct pcap_ring_entry {
    struct pcaprec_hdr_s rec;
    time_t capture_time;
    size_t incl_len;
    size_t full_packet_len;
    unsigned char packet[PCAP_MAX_PACKET_SIZE];
};

struct pcap_counter_shard {
    unsigned long long packets_seen;
    unsigned long long packets_written;
    unsigned long long packets_filtered;
    unsigned long long packets_truncated;
    unsigned long long packets_dropped_queue;
    unsigned long long packets_dropped_io;
    unsigned long long rotations;
    unsigned long long bytes_captured;
    unsigned long long bytes_original;
};

struct pcap_writer_file_state {
    unsigned int rotation_index;
    unsigned long long current_file_bytes;
    unsigned long long packets_current_file;
    time_t current_file_opened_at;
};

static int pcap_file_fd = -1;
static int pcap_cancel_fd = -1;
static int pcap_fd = -1;
static int pcap_family = AF_UNSPEC;
static int pcap_socktype = SOCK_STREAM;
static struct sockaddr_storage local_addr, remote_addr;
static uint32_t seq_local = 1000, seq_remote = 2000;
static char pcap_base_path[PATH_MAX];
static struct pcap_writer_file_state pcap_writer_file;
static struct pcap_counter_shard pcap_capture_counter_shard;
static struct pcap_counter_shard pcap_writer_counter_shard;

static struct pcap_ring_entry pcap_ring[PCAP_RING_CAPACITY];
static size_t pcap_ring_head;
static size_t pcap_ring_tail;
static size_t pcap_ring_count;

static pthread_mutex_t pcap_ring_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pcap_ring_cond = PTHREAD_COND_INITIALIZER;
static pthread_t pcap_writer_thread;
static int pcap_writer_running;
static int pcap_writer_error; /* Published to the relay by pthread_join(). */
static atomic_int pcap_writer_stop;

static void pcap_reset_state(void) {
    pcap_file_fd = -1;
    pcap_cancel_fd = -1;
    pcap_fd = -1;
    pcap_family = AF_UNSPEC;
    pcap_socktype = SOCK_STREAM;
    memset(&local_addr, 0, sizeof(local_addr));
    memset(&remote_addr, 0, sizeof(remote_addr));
    seq_local = 1000;
    seq_remote = 2000;
    pcap_base_path[0] = '\0';
    memset(&pcap_writer_file, 0, sizeof(pcap_writer_file));
    memset(&pcap_capture_counter_shard, 0, sizeof(pcap_capture_counter_shard));
    memset(&pcap_writer_counter_shard, 0, sizeof(pcap_writer_counter_shard));
    pcap_ring_head = 0;
    pcap_ring_tail = 0;
    pcap_ring_count = 0;
    pcap_writer_running = 0;
    pcap_writer_error = 0;
    atomic_store(&pcap_writer_stop, 0);
}

static int pcap_write_all_fd(int fd, const void* data, size_t len, struct bx_poll_deadline* deadline, int stop_sensitive) {
    const unsigned char* p = data;
    size_t off = 0;

    while (off < len) {
        struct pollfd fds[2] = {{.fd = fd, .events = POLLOUT}, {.fd = pcap_cancel_fd, .events = POLLIN}};
        int ready = bx_poll_deadline_wait(deadline, fds, stop_sensitive ? 2 : 1);
        if (ready <= 0) {
            if (ready == 0)
                errno = ETIMEDOUT;
            return -1;
        }
        /* Drain ready output after shutdown, but do not wait for space. */
        if (stop_sensitive && fds[1].revents && !fds[0].revents) {
            errno = atomic_load(&pcap_writer_stop) ? ECANCELED : EIO;
            return -1;
        }
        ssize_t n = write(fd, p + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n == -1 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (stop_sensitive && atomic_load(&pcap_writer_stop)) {
                errno = ECANCELED;
                return -1;
            }
            continue;
        }
        if (n == 0)
            errno = EIO;
        return -1;
    }

    return 0;
}

static int pcap_write_global_header(int fd, struct bx_poll_deadline* deadline, int stop_sensitive) {
    struct pcap_hdr_s hdr;

    memset(&hdr, 0, sizeof(hdr));
    hdr.magic_number = 0xa1b2c3d4;
    hdr.version_major = 2;
    hdr.version_minor = 4;
    hdr.snaplen = pcap_snaplen;
    hdr.network = 101; /* LINKTYPE_RAW (IP) */

    if (pcap_write_all_fd(fd, &hdr, sizeof(hdr), deadline, stop_sensitive) == -1)
        return -1;

    pcap_writer_file.current_file_bytes = sizeof(hdr);
    pcap_writer_file.packets_current_file = 0;
    pcap_writer_file.current_file_opened_at = time(NULL);
    return 0;
}

static int pcap_open_default_file(struct bx_poll_deadline* deadline) {
    char tmpl[] = "/tmp/nc-pcap-XXXXXX.pcap";
    int fd, saved_errno;

    fd = mkstemps(tmpl, 5);
    if (fd == -1)
        return -1;
    if (nc_strlcpy(pcap_base_path, tmpl, sizeof(pcap_base_path)) >= sizeof(pcap_base_path)) {
        errno = ENAMETOOLONG;
        goto fail;
    }
    if (bx_fd_set_cloexec(fd, true) == -1 || bx_fd_set_nonblocking(fd, true) == -1)
        goto fail;
    if (pcap_write_global_header(fd, deadline, 0) == -1)
        goto fail;

    pcap_file_fd = fd;
    return 0;
fail:
    saved_errno = errno;
    close(fd);
    unlink(tmpl);
    errno = saved_errno;
    return -1;
}

static int pcap_build_path(unsigned int index, char* out, size_t outsz) {
    int n;

    if (out == NULL || outsz == 0 || pcap_base_path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    if (index == 0) {
        if (nc_strlcpy(out, pcap_base_path, outsz) >= outsz) {
            errno = ENAMETOOLONG;
            return -1;
        }
        return 0;
    }

    n = snprintf(out, outsz, "%s.%u", pcap_base_path, index);
    if (n < 0 || (size_t)n >= outsz) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int pcap_open_file_for_index(unsigned int index, struct bx_poll_deadline* deadline, int stop_sensitive) {
    char path[PATH_MAX];
    struct stat sb;
    int flags;
    int fd;

    if (pcap_build_path(index, path, sizeof(path)) == -1)
        return -1;

    flags = O_WRONLY | O_CREAT | O_CLOEXEC | O_NONBLOCK;
    if (stat(path, &sb) == -1 || !S_ISFIFO(sb.st_mode))
        flags |= O_TRUNC;

    fd = bx_fd_open_cloexec(path, flags, 0600);
    if (fd == -1)
        return -1;

    if (pcap_write_global_header(fd, deadline, stop_sensitive) == -1) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    pcap_file_fd = fd;
    return 0;
}

static int pcap_maybe_rotate(size_t next_record_size, time_t now, struct bx_poll_deadline* deadline) {
    int rotate = 0;

    if (pcap_file_fd == -1) {
        errno = EBADF;
        return -1;
    }
    if (pcap_writer_file.packets_current_file == 0)
        return 0;

    if (pcap_rotate_size > 0 &&
        pcap_writer_file.current_file_bytes + next_record_size > pcap_rotate_size)
        rotate = 1;
    if (pcap_rotate_seconds > 0 &&
        now - pcap_writer_file.current_file_opened_at >= (time_t)pcap_rotate_seconds)
        rotate = 1;

    if (!rotate)
        return 0;

    int fd = pcap_file_fd;
    pcap_file_fd = -1;
    if (close(fd) == -1)
        return -1;

    pcap_writer_file.rotation_index++;
    if (pcap_open_file_for_index(pcap_writer_file.rotation_index, deadline, 1) == -1)
        return -1;

    pcap_writer_counter_shard.rotations++;
    return 0;
}

static void pcap_counter_shard_add(struct pcap_counter_shard* totals,
                                   const struct pcap_counter_shard* shard) {
    totals->packets_seen += shard->packets_seen;
    totals->packets_written += shard->packets_written;
    totals->packets_filtered += shard->packets_filtered;
    totals->packets_truncated += shard->packets_truncated;
    totals->packets_dropped_queue += shard->packets_dropped_queue;
    totals->packets_dropped_io += shard->packets_dropped_io;
    totals->rotations += shard->rotations;
    totals->bytes_captured += shard->bytes_captured;
    totals->bytes_original += shard->bytes_original;
}

static void pcap_counter_reduce(struct pcap_counter_shard* totals) {
    memset(totals, 0, sizeof(*totals));
    pcap_counter_shard_add(totals, &pcap_capture_counter_shard);
    pcap_counter_shard_add(totals, &pcap_writer_counter_shard);
}

static void pcap_emit_summary(void) {
    struct pcap_counter_shard counters;
    unsigned long long pcap_packets_dropped;

    if (pcap_base_path[0] == '\0')
        return;

    pcap_counter_reduce(&counters);
    pcap_packets_dropped =
        counters.packets_dropped_queue + counters.packets_dropped_io;

    if (jflag) {
        json_event_begin(stderr, "info", "pcap_summary", NULL, uflag ? "udp" : "tcp",
                         "disabled", NULL, NULL, NULL, NULL);
        fprintf(stderr,
                ",\"message\":\"PCAP summary\",\"capture_mode\":\"app\",\"path\":\"%s\","
                "\"packets_seen\":%llu,\"packets_written\":%llu,\"packets_filtered\":%llu,"
                "\"packets_truncated\":%llu,\"packets_dropped\":%llu,\"rotations\":%llu,"
                "\"bytes_captured\":%llu,\"bytes_original\":%llu}\n",
                pcap_base_path, counters.packets_seen, counters.packets_written, counters.packets_filtered,
                counters.packets_truncated, pcap_packets_dropped, counters.rotations, counters.bytes_captured,
                counters.bytes_original);
    }
    else if (vflag >= 2) {
        fprintf(stderr,
                "PCAP summary: path=%s mode=app packets_seen=%llu packets_written=%llu packets_filtered=%llu "
                "packets_truncated=%llu packets_dropped=%llu rotations=%llu bytes_captured=%llu bytes_original=%llu\n",
                pcap_base_path, counters.packets_seen, counters.packets_written, counters.packets_filtered,
                counters.packets_truncated, pcap_packets_dropped, counters.rotations, counters.bytes_captured,
                counters.bytes_original);
    }
}

static void pcap_disable_capture(void) {
    if (pcap_file_fd != -1) {
        close(pcap_file_fd);
        pcap_file_fd = -1;
    }
    if (pcap_cancel_fd != -1)
        close(pcap_cancel_fd);
    pcap_reset_state();
}

static void pcap_drop_queued_locked(void) {
    pcap_writer_counter_shard.packets_dropped_queue += pcap_ring_count;
    pcap_ring_head = 0;
    pcap_ring_tail = 0;
    pcap_ring_count = 0;
}

static void* pcap_writer_main(void* arg) {
    (void)arg;

    for (;;) {
        struct pcap_ring_entry entry;
        struct bx_poll_deadline deadline;

        pthread_mutex_lock(&pcap_ring_lock);
        while (pcap_ring_count == 0 && !atomic_load(&pcap_writer_stop))
            pthread_cond_wait(&pcap_ring_cond, &pcap_ring_lock);

        if (pcap_ring_count == 0 && atomic_load(&pcap_writer_stop)) {
            pthread_mutex_unlock(&pcap_ring_lock);
            break;
        }

        entry = pcap_ring[pcap_ring_head];
        pcap_ring_head = (pcap_ring_head + 1) % PCAP_RING_CAPACITY;
        pcap_ring_count--;
        pthread_mutex_unlock(&pcap_ring_lock);

        bx_poll_deadline_init(&deadline, timeout);
        if (pcap_maybe_rotate(sizeof(entry.rec) + entry.incl_len, entry.capture_time, &deadline) == -1 || pcap_write_all_fd(pcap_file_fd, &entry.rec, sizeof(entry.rec), &deadline, 1) == -1 ||
            pcap_write_all_fd(pcap_file_fd, entry.packet, entry.incl_len, &deadline, 1) == -1) {
            pcap_writer_error = errno;
            pcap_writer_counter_shard.packets_dropped_io++;
            atomic_store(&pcap_writer_stop, 1);
            pthread_mutex_lock(&pcap_ring_lock);
            pcap_drop_queued_locked();
            pthread_mutex_unlock(&pcap_ring_lock);
            break;
        }

        pcap_writer_counter_shard.packets_written++;
        pcap_writer_counter_shard.bytes_captured += entry.incl_len;
        pcap_writer_counter_shard.bytes_original += entry.full_packet_len;
        pcap_writer_file.current_file_bytes += sizeof(entry.rec) + entry.incl_len;
        pcap_writer_file.packets_current_file++;
    }

    return NULL;
}

int pcap_open(int fd, const char* path) {
    struct bx_poll_deadline deadline;
    socklen_t len;
    int error;

    if (!path) {
        errno = EINVAL;
        return -1;
    }

    pcap_reset_state();

    len = sizeof(local_addr);
    if (getsockname(fd, (struct sockaddr*)&local_addr, &len) == -1)
        goto fail;
    len = sizeof(remote_addr);
    if (getpeername(fd, (struct sockaddr*)&remote_addr, &len) == -1)
        goto fail;
    pcap_family = local_addr.ss_family;
    if ((pcap_family != AF_INET && pcap_family != AF_INET6) || remote_addr.ss_family != pcap_family) {
        errno = EAFNOSUPPORT;
        goto fail;
    }

    len = sizeof(pcap_socktype);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &pcap_socktype, &len) == -1)
        goto fail;
    if (pcap_socktype != SOCK_STREAM && pcap_socktype != SOCK_DGRAM) {
        errno = EPROTOTYPE;
        goto fail;
    }

    pcap_cancel_fd = bx_fd_eventfd_cloexec(0, EFD_NONBLOCK);
    if (pcap_cancel_fd == -1)
        goto fail;
    bx_poll_deadline_init(&deadline, timeout);
    if (path[0] == '\0') {
        if (pcap_open_default_file(&deadline) == -1)
            goto fail;
    }
    else {
        if (nc_strlcpy(pcap_base_path, path, sizeof(pcap_base_path)) >= sizeof(pcap_base_path)) {
            errno = ENAMETOOLONG;
            goto fail;
        }
        if (pcap_open_file_for_index(0, &deadline, 0) == -1)
            goto fail;
    }

    pcap_fd = fd;
    error = pthread_create(&pcap_writer_thread, NULL, pcap_writer_main, NULL);
    if (error != 0) {
        errno = error;
        goto fail;
    }
    pcap_writer_running = 1;
    if (path[0] == '\0' && vflag >= 2)
        fprintf(stderr, "PCAP default path: %s\n", pcap_base_path);
    return 0;
fail:
    error = errno;
    if (path[0] == '\0' && pcap_file_fd != -1)
        unlink(pcap_base_path);
    pcap_disable_capture();
    errno = error;
    return -1;
}

void pcap_log(int fd, const unsigned char* buf, size_t len, int direction) {
    struct pcaprec_hdr_s rec;
    struct timeval tv;
    unsigned char packet[PCAP_MAX_PACKET_SIZE];
    size_t offset = 0;
    size_t full_payload_len;
    size_t full_packet_len;
    size_t incl_len;
    time_t now;

    if (!pcap_writer_running || fd != pcap_fd)
        return;

    pcap_capture_counter_shard.packets_seen++;

    if (direction == 1) {
        if (!(pcap_filter & PCAP_FILTER_OUT)) {
            pcap_capture_counter_shard.packets_filtered++;
            return;
        }
    }
    else {
        if (!(pcap_filter & PCAP_FILTER_IN)) {
            pcap_capture_counter_shard.packets_filtered++;
            return;
        }
    }

    if (gettimeofday(&tv, NULL) == -1)
        return;
    now = tv.tv_sec;

    if (pcap_family == AF_INET) {
        struct iphdr* ip = (struct iphdr*)packet;

        memset(ip, 0, sizeof(struct iphdr));
        ip->version = 4;
        ip->ihl = 5;
        ip->ttl = 64;
        ip->protocol = (pcap_socktype == SOCK_STREAM) ? IPPROTO_TCP : IPPROTO_UDP;
        ip->saddr = (direction == 1) ? ((struct sockaddr_in*)&local_addr)->sin_addr.s_addr
                                     : ((struct sockaddr_in*)&remote_addr)->sin_addr.s_addr;
        ip->daddr = (direction == 1) ? ((struct sockaddr_in*)&remote_addr)->sin_addr.s_addr
                                     : ((struct sockaddr_in*)&local_addr)->sin_addr.s_addr;
        offset = sizeof(struct iphdr);

        if (pcap_socktype == SOCK_STREAM) {
            struct tcphdr* tcp = (struct tcphdr*)(packet + offset);

            memset(tcp, 0, sizeof(struct tcphdr));
            tcp->source = (direction == 1) ? ((struct sockaddr_in*)&local_addr)->sin_port
                                           : ((struct sockaddr_in*)&remote_addr)->sin_port;
            tcp->dest = (direction == 1) ? ((struct sockaddr_in*)&remote_addr)->sin_port
                                         : ((struct sockaddr_in*)&local_addr)->sin_port;
            tcp->seq = htonl((direction == 1) ? seq_local : seq_remote);
            tcp->ack_seq = htonl((direction == 1) ? seq_remote : seq_local);
            tcp->doff = 5;
            tcp->ack = 1;
            tcp->psh = 1;
            tcp->window = htons(65535);
            offset += sizeof(struct tcphdr);
        }
        else {
            struct udphdr* udp = (struct udphdr*)(packet + offset);

            memset(udp, 0, sizeof(struct udphdr));
            udp->source = (direction == 1) ? ((struct sockaddr_in*)&local_addr)->sin_port
                                           : ((struct sockaddr_in*)&remote_addr)->sin_port;
            udp->dest = (direction == 1) ? ((struct sockaddr_in*)&remote_addr)->sin_port
                                         : ((struct sockaddr_in*)&local_addr)->sin_port;
            offset += sizeof(struct udphdr);
        }
    }
    else if (pcap_family == AF_INET6) {
        struct ip6_hdr* ip6 = (struct ip6_hdr*)packet;

        memset(ip6, 0, sizeof(struct ip6_hdr));
        ip6->ip6_vfc = 0x60;
        ip6->ip6_nxt = (pcap_socktype == SOCK_STREAM) ? IPPROTO_TCP : IPPROTO_UDP;
        ip6->ip6_hlim = 64;
        memcpy(&ip6->ip6_src,
               (direction == 1) ? &((struct sockaddr_in6*)&local_addr)->sin6_addr
                                : &((struct sockaddr_in6*)&remote_addr)->sin6_addr,
               16);
        memcpy(&ip6->ip6_dst,
               (direction == 1) ? &((struct sockaddr_in6*)&remote_addr)->sin6_addr
                                : &((struct sockaddr_in6*)&local_addr)->sin6_addr,
               16);
        offset = sizeof(struct ip6_hdr);

        if (pcap_socktype == SOCK_STREAM) {
            struct tcphdr* tcp = (struct tcphdr*)(packet + offset);

            memset(tcp, 0, sizeof(struct tcphdr));
            tcp->source = (direction == 1) ? ((struct sockaddr_in6*)&local_addr)->sin6_port
                                           : ((struct sockaddr_in6*)&remote_addr)->sin6_port;
            tcp->dest = (direction == 1) ? ((struct sockaddr_in6*)&remote_addr)->sin6_port
                                         : ((struct sockaddr_in6*)&local_addr)->sin6_port;
            tcp->seq = htonl((direction == 1) ? seq_local : seq_remote);
            tcp->ack_seq = htonl((direction == 1) ? seq_remote : seq_local);
            tcp->doff = 5;
            tcp->ack = 1;
            tcp->psh = 1;
            tcp->window = htons(65535);
            offset += sizeof(struct tcphdr);
        }
        else {
            struct udphdr* udp = (struct udphdr*)(packet + offset);

            memset(udp, 0, sizeof(struct udphdr));
            udp->source = (direction == 1) ? ((struct sockaddr_in6*)&local_addr)->sin6_port
                                           : ((struct sockaddr_in6*)&remote_addr)->sin6_port;
            udp->dest = (direction == 1) ? ((struct sockaddr_in6*)&remote_addr)->sin6_port
                                         : ((struct sockaddr_in6*)&local_addr)->sin6_port;
            offset += sizeof(struct udphdr);
        }
    }
    else {
        return; /* Unsupported family for PCAP */
    }

    full_payload_len = (len > PCAP_MAX_PAYLOAD_SIZE) ? PCAP_MAX_PAYLOAD_SIZE : len;
    full_packet_len = offset + full_payload_len;
    incl_len = full_packet_len;
    if (incl_len > pcap_snaplen)
        incl_len = pcap_snaplen;
    if (incl_len < full_packet_len)
        pcap_capture_counter_shard.packets_truncated++;

    if (pcap_family == AF_INET) {
        struct iphdr* ip = (struct iphdr*)packet;

        ip->tot_len = htons((uint16_t)full_packet_len);
        if (pcap_socktype == SOCK_STREAM) {
            if (direction == 1)
                seq_local += full_payload_len;
            else
                seq_remote += full_payload_len;
        }
        else {
            struct udphdr* udp = (struct udphdr*)(packet + sizeof(struct iphdr));

            udp->len = htons((uint16_t)(sizeof(struct udphdr) + full_payload_len));
        }
    }
    else {
        struct ip6_hdr* ip6 = (struct ip6_hdr*)packet;

        ip6->ip6_plen = htons((uint16_t)(full_packet_len - sizeof(struct ip6_hdr)));
        if (pcap_socktype == SOCK_STREAM) {
            if (direction == 1)
                seq_local += full_payload_len;
            else
                seq_remote += full_payload_len;
        }
        else {
            struct udphdr* udp = (struct udphdr*)(packet + sizeof(struct ip6_hdr));

            udp->len = htons((uint16_t)(sizeof(struct udphdr) + full_payload_len));
        }
    }

    if (incl_len > offset)
        memcpy(packet + offset, buf, incl_len - offset);

    rec.ts_sec = tv.tv_sec;
    rec.ts_usec = tv.tv_usec;
    rec.incl_len = incl_len;
    rec.orig_len = full_packet_len;

    pthread_mutex_lock(&pcap_ring_lock);
    if (atomic_load(&pcap_writer_stop)) {
        pcap_capture_counter_shard.packets_dropped_io++;
        pthread_mutex_unlock(&pcap_ring_lock);
        return;
    }
    if (pcap_ring_count == PCAP_RING_CAPACITY) {
        pcap_capture_counter_shard.packets_dropped_queue++;
        pthread_mutex_unlock(&pcap_ring_lock);
        return;
    }

    pcap_ring[pcap_ring_tail].rec = rec;
    pcap_ring[pcap_ring_tail].capture_time = now;
    pcap_ring[pcap_ring_tail].incl_len = incl_len;
    pcap_ring[pcap_ring_tail].full_packet_len = full_packet_len;
    if (incl_len > 0)
        memcpy(pcap_ring[pcap_ring_tail].packet, packet, incl_len);

    pcap_ring_tail = (pcap_ring_tail + 1) % PCAP_RING_CAPACITY;
    pcap_ring_count++;
    pthread_cond_signal(&pcap_ring_cond);
    pthread_mutex_unlock(&pcap_ring_lock);
}

int pcap_close(void) {
    int error = 0;

    if (pcap_writer_running) {
        atomic_store(&pcap_writer_stop, 1);
        int notified;
        do {
            notified = eventfd_write(pcap_cancel_fd, 1);
        } while (notified == -1 && errno == EINTR);
        if (notified == -1)
            return -1;
        pthread_mutex_lock(&pcap_ring_lock);
        pthread_cond_signal(&pcap_ring_cond);
        pthread_mutex_unlock(&pcap_ring_lock);
        error = pthread_join(pcap_writer_thread, NULL);
        if (error != 0) {
            errno = error;
            return -1;
        }
        pcap_writer_running = 0;
        error = pcap_writer_error;
    }

    if (pcap_file_fd != -1) {
        if (close(pcap_file_fd) == -1 && error == 0)
            error = errno;
        pcap_file_fd = -1;
    }
    if (pcap_cancel_fd != -1) {
        if (close(pcap_cancel_fd) == -1 && error == 0)
            error = errno;
        pcap_cancel_fd = -1;
    }

    pcap_emit_summary();
    pcap_reset_state();
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}
