#include "netcat.h"
#include "quic.h"

/*
 * QUIC Probing
 *
 * We send a QUIC Long Header packet with a reserved version to trigger
 * a Version Negotiation packet from the server.
 *
 * Packet Format (Initial/Long Header):
 * Byte 0: 1 (Header Form) | 1 (Fixed Bit) | Type (2 bits) | Type Specific (4 bits)
 *         We use 0xC0 (11000000) - Long Header, Fixed Bit set, Initial?
 *         Actually type for Initial is 0x0.
 *         So 0x80 | 0x40 | 0x00 ... = 0xC0?
 *         RFC 9000: Initial Packet Type is 0x0.
 *         Header Form (1) | Fixed Bit (1) | Long Packet Type (2) | Type Specific (4)
 *         1 1 00 0000 -> 0xC0.
 * Bytes 1-4: Version. We use a grease version or reserved version.
 *         0x0a0a0a0a (Grease)
 * Byte 5: DCID Len.
 * Bytes 6..: DCID.
 * Byte ..: SCID Len.
 * Bytes ..: SCID.
 *
 * We keep it simple.
 */

int quic_test(int s, char* host, char* port) {
    unsigned char buf[1200]; /* Min UDP payload for QUIC is often 1200, though initial can be smaller?
                                Clients MUST expand UDP payloads to at least 1200 bytes. */
    unsigned char recv_buf[2048];
    struct sockaddr_storage peer;
    socklen_t peerlen = sizeof(peer);
    ssize_t len;
    struct pollfd pfd;
    char src_addr[NI_MAXHOST];
    char src_port[NI_MAXSERV];
    char dst_addr[NI_MAXHOST];
    char dst_port[NI_MAXSERV];
    const char* src_addr_p = NULL;
    const char* src_port_p = NULL;
    const char* dst_addr_p = NULL;
    const char* dst_port_p = NULL;

    if (json_socket_tuple_from_fd(s, src_addr, sizeof(src_addr), src_port, sizeof(src_port), dst_addr, sizeof(dst_addr),
                                  dst_port, sizeof(dst_port)) == 0) {
        src_addr_p = src_addr;
        src_port_p = src_port;
        dst_addr_p = dst_addr;
        dst_port_p = dst_port;
    }

    /*
     * Construct a QUIC Initial Packet with a reserved version.
     * Use version 0xbadc0de1 (Reserved/Grease-like)
     */
    memset(buf, 0, sizeof(buf));

    /* Header Byte: Long Header (0x80) | Fixed Bit (0x40) | Initial (0x00) */
    buf[0] = 0xC0;

    /* Version: 0xbadc0de1 (Not supported, triggers Version Negotiation) */
    buf[1] = 0xba;
    buf[2] = 0xdc;
    buf[3] = 0x0d;
    buf[4] = 0xe1;

    /* DCID Length: 8 bytes */
    buf[5] = 0x08;

    /* DCID: Random 8 bytes */
    nc_random_buf(&buf[6], 8);

    /* SCID Length: 0 bytes */
    buf[14] = 0x00;

    /* Token Length: 0 (Varint) */
    buf[15] = 0x00;

    /* Length: 0 (Varint) - Payload is empty/padding */
    buf[16] = 0x00;

    /*
     * Fill the rest with padding to reach 1200 bytes, as servers might drop smaller packets.
     * RFC 9000 8.1: "A client MUST expand the payload of all UDP datagrams carrying an Initial packet to at least 1200
     * bytes"
     */

    /* Log QUIC initial packet sent */
    if (jflag) {
        char dcid_hex[17];
        int i;

        for (i = 0; i < 8; i++)
            snprintf(dcid_hex + i * 2, 3, "%02x", buf[6 + i]);
        dcid_hex[16] = '\0';

        json_event_begin(stderr, "info", "quic_initial_sent", "out", "udp", "disabled", "initial_sent", src_addr_p,
                         src_port_p, dst_addr_p, dst_port_p);
        fprintf(stderr,
                ",\"host\":\"%s\",\"port\":\"%s\",\"version\":\"%02x%02x%02x%02x\",\"dcid\":\"%s\","
                "\"scid_len\":%u}\n",
                host, port, buf[1], buf[2], buf[3], buf[4], dcid_hex, (unsigned int)buf[14]);
    }

    /* Send the probe */
    if (write(s, buf, 1200) != 1200) {
        warn("quic write failed");
        return -1;
    }

    /* Wait for response */
    pfd.fd = s;
    pfd.events = POLLIN;

    /* Wait up to timeout (default 2s if not set?) - Use 'timeout' global or 2000ms */
    int t = (timeout == -1) ? 2000 : timeout;

    if (poll(&pfd, 1, t) == -1) {
        warn("quic poll failed");
        return -1;
    }

    if (pfd.revents & POLLIN) {
        len = recvfrom(s, recv_buf, sizeof(recv_buf), 0, (struct sockaddr*)&peer, &peerlen);
        if (len < 0) {
            warn("quic recv failed");
            return -1;
        }

        /* Check for Version Negotiation Packet */
        /* Header: 1xxxxxxx */
        if ((recv_buf[0] & 0x80) == 0) {
            /* Short header? Unlikely response to Initial with bad version. */
            if (vflag)
                warnx("Received short header packet from %s:%s", host, port);
            if (jflag) {
                const char* event = (len >= 21) ? "quic_stateless_reset_detected" : "quic_short_header_received";
                const char* quic_state = (len >= 21) ? "stateless_reset_detected" : "short_header_received";
                json_event_begin(stderr, "info", event, "in", "udp", "disabled", quic_state, src_addr_p, src_port_p,
                                 dst_addr_p, dst_port_p);
                fprintf(stderr, ",\"host\":\"%s\",\"port\":\"%s\",\"length\":%zd}\n", host, port, len);
            }
            return 0;
        }

        /* Version must be 0 for Version Negotiation */
        if (len >= 5 && recv_buf[1] == 0 && recv_buf[2] == 0 && recv_buf[3] == 0 && recv_buf[4] == 0) {
            if (vflag)
                fprintf(stderr, "QUIC Version Negotiation packet received from %s:%s\n", host, port);
            if (jflag) {
                char dcid_hex[513];
                char scid_hex[513];
                char versions[2048];
                size_t i, off;
                size_t dcid_len, scid_len;
                size_t recv_len = (size_t)len;

                dcid_len = (recv_len > 6) ? recv_buf[5] : 0;
                if (6 + dcid_len > recv_len)
                    dcid_len = (recv_len >= 6) ? recv_len - 6 : 0;
                for (i = 0; i < dcid_len; i++)
                    snprintf(dcid_hex + i * 2, 3, "%02x", recv_buf[6 + i]);
                dcid_hex[dcid_len * 2] = '\0';

                off = 6 + dcid_len;
                scid_len = (off < recv_len) ? recv_buf[off] : 0;
                if (off + 1 + scid_len > recv_len)
                    scid_len = (recv_len > off + 1) ? recv_len - off - 1 : 0;
                for (i = 0; i < scid_len; i++)
                    snprintf(scid_hex + i * 2, 3, "%02x", recv_buf[off + 1 + i]);
                scid_hex[scid_len * 2] = '\0';

                off = off + 1 + scid_len;
                versions[0] = '\0';
                if (recv_len >= off + 4 && ((recv_len - off) % 4 == 0)) {
                    size_t num_versions = (recv_len - off) / 4;
                    char* p = versions;
                    for (i = 0; i < num_versions; i++) {
                        if (i > 0)
                            *p++ = ',';
                        p += sprintf(p, "\"0x%02x%02x%02x%02x\"", recv_buf[off + i * 4], recv_buf[off + i * 4 + 1],
                                     recv_buf[off + i * 4 + 2], recv_buf[off + i * 4 + 3]);
                    }
                    *p = '\0';
                }

                json_event_begin(stderr, "info", "quic_version_negotiation_received", "in", "udp", "disabled",
                                 "version_negotiation", src_addr_p, src_port_p, dst_addr_p, dst_port_p);
                fprintf(stderr,
                        ",\"host\":\"%s\",\"port\":\"%s\",\"dcid_len\":%zu,\"dcid\":\"%s\","
                        "\"scid_len\":%zu,\"scid\":\"%s\",\"offered_versions\":[%s]}\n",
                        host, port, dcid_len, dcid_hex, scid_len, scid_hex, versions);
            }
            return 1;
        }

        /* Or maybe the server accepted our bogus version? Unlikely. */
        if (vflag)
            warnx("Received QUIC packet with version %02x%02x%02x%02x from %s:%s", recv_buf[1], recv_buf[2],
                  recv_buf[3], recv_buf[4], host, port);
        if (jflag) {
            char dcid_hex[513];
            char scid_hex[513];
            dcid_hex[0] = '\0';
            scid_hex[0] = '\0';
            size_t i, off;
            size_t dcid_len, scid_len;
            size_t recv_len = (size_t)len;
            unsigned char packet_type = (recv_buf[0] >> 4) & 0x03;

            dcid_len = (recv_len > 6) ? recv_buf[5] : 0;
            /* Ensure DCID doesn't exceed packet bounds */
            if (6 + dcid_len > recv_len)
                dcid_len = (recv_len >= 6) ? recv_len - 6 : 0;
            for (i = 0; i < dcid_len; i++)
                snprintf(dcid_hex + i * 2, 3, "%02x", recv_buf[6 + i]);
            dcid_hex[dcid_len * 2] = '\0';

            off = 6 + dcid_len;
            if (off >= recv_len) {
                scid_len = 0;
                off = recv_len;
            }
            else {
                scid_len = recv_buf[off];
                if (off + 1 + scid_len > recv_len)
                    scid_len = (recv_len >= off + 1) ? recv_len - off - 1 : 0;
                for (i = 0; i < scid_len; i++)
                    snprintf(scid_hex + i * 2, 3, "%02x", recv_buf[off + 1 + i]);
                scid_hex[scid_len * 2] = '\0';
                off = off + 1 + scid_len;
            }

            if (packet_type == 3) { /* Retry */
                /* Retry packet has token and 16-byte integrity tag */
                size_t token_len = (recv_len >= off + 16) ? (recv_len - off - 16) : 0;
                json_event_begin(stderr, "info", "quic_retry_received", "in", "udp", "disabled", "retry_received",
                                 src_addr_p, src_port_p, dst_addr_p, dst_port_p);
                fprintf(stderr,
                        ",\"host\":\"%s\",\"port\":\"%s\",\"version\":\"%02x%02x%02x%02x\","
                        "\"dcid_len\":%zu,\"dcid\":\"%s\",\"scid_len\":%zu,\"scid\":\"%s\",\"token_len\":%zu}\n",
                        host, port, recv_buf[1], recv_buf[2], recv_buf[3], recv_buf[4], dcid_len, dcid_hex, scid_len,
                        scid_hex, token_len);
            }
            else {
                const char* event = "quic_response_received";
                const char* quic_state = "response_received";
                if (packet_type == 0)
                    event = "quic_initial_received";
                else if (packet_type == 1)
                    event = "quic_0rtt_received";
                else if (packet_type == 2)
                    event = "quic_handshake_received";
                if (packet_type == 0)
                    quic_state = "initial_received";
                else if (packet_type == 1)
                    quic_state = "0rtt_received";
                else if (packet_type == 2)
                    quic_state = "handshake_received";
                json_event_begin(stderr, "info", event, "in", "udp", "disabled", quic_state, src_addr_p, src_port_p,
                                 dst_addr_p, dst_port_p);
                fprintf(stderr,
                        ",\"host\":\"%s\",\"port\":\"%s\",\"version\":\"%02x%02x%02x%02x\","
                        "\"dcid_len\":%zu,\"dcid\":\"%s\",\"scid_len\":%zu,\"scid\":\"%s\"}\n",
                        host, port, recv_buf[1], recv_buf[2], recv_buf[3], recv_buf[4], dcid_len, dcid_hex, scid_len,
                        scid_hex);
            }
        }

        /* If we got ANY valid-looking QUIC packet back, we can probably say it speaks QUIC. */
        return 1;
    }

    return 0; /* Timeout or no response */
}
