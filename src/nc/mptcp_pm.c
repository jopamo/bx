#include "mptcp_pm.h"

#ifdef __linux__

#include <arpa/inet.h>
#include <linux/genetlink.h>
#include <linux/mptcp_pm.h>
#include <string.h>
#include <stdio.h>

static int nla_ok(const struct nlattr* nla, int rem) {
    return rem >= (int)sizeof(*nla) && nla->nla_len >= sizeof(*nla) && nla->nla_len <= rem;
}

static const struct nlattr* nla_next(const struct nlattr* nla, int* rem) {
    int len = NLA_ALIGN(nla->nla_len);

    *rem -= len;
    return (const struct nlattr*)((const char*)nla + len);
}

static const void* nla_data_ptr(const struct nlattr* nla) {
    return (const char*)nla + NLA_HDRLEN;
}

static int nla_data_len(const struct nlattr* nla) {
    return (int)nla->nla_len - NLA_HDRLEN;
}

static int nla_get_u8(const struct nlattr* nla, uint8_t* out) {
    if (nla_data_len(nla) < (int)sizeof(uint8_t))
        return -1;
    memcpy(out, nla_data_ptr(nla), sizeof(uint8_t));
    return 0;
}

static int nla_get_u16(const struct nlattr* nla, uint16_t* out) {
    uint16_t v;

    if (nla_data_len(nla) < (int)sizeof(uint16_t))
        return -1;
    memcpy(&v, nla_data_ptr(nla), sizeof(v));
    if (nla->nla_type & NLA_F_NET_BYTEORDER)
        v = ntohs(v);
    *out = v;
    return 0;
}

static int nla_get_u32(const struct nlattr* nla, uint32_t* out) {
    uint32_t v;

    if (nla_data_len(nla) < (int)sizeof(uint32_t))
        return -1;
    memcpy(&v, nla_data_ptr(nla), sizeof(v));
    if (nla->nla_type & NLA_F_NET_BYTEORDER)
        v = ntohl(v);
    *out = v;
    return 0;
}

static int nla_string_eq(const struct nlattr* nla, const char* s) {
    const char* data;
    int len;
    size_t slen;

    if (s == NULL)
        return 0;

    len = nla_data_len(nla);
    if (len <= 0)
        return 0;

    data = (const char*)nla_data_ptr(nla);
    slen = strlen(s);

    if ((size_t)len == slen)
        return memcmp(data, s, slen) == 0;

    if ((size_t)len == slen + 1)
        return memcmp(data, s, slen) == 0 && data[slen] == '\0';

    return 0;
}

void mptcp_pm_reset_event(struct mptcp_pm_event* event) {
    memset(event, 0, sizeof(*event));
}

int mptcp_pm_parse_genl_event(const struct nlmsghdr* nlh, size_t nlh_len, struct mptcp_pm_event* event) {
    const struct genlmsghdr* ghdr;
    const struct nlattr* attr;
    int rem;

    if (nlh == NULL || event == NULL)
        return -1;
    if (nlh_len < sizeof(*nlh) || nlh->nlmsg_len < NLMSG_LENGTH(GENL_HDRLEN) || nlh->nlmsg_len > nlh_len)
        return -1;

    ghdr = (const struct genlmsghdr*)((const char*)nlh + NLMSG_HDRLEN);
    mptcp_pm_reset_event(event);
    event->cmd = ghdr->cmd;

    rem = (int)nlh->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;
    attr = (const struct nlattr*)((const char*)ghdr + GENL_HDRLEN);

    while (nla_ok(attr, rem)) {
        uint16_t type = attr->nla_type & NLA_TYPE_MASK;

        switch (type) {
            case MPTCP_ATTR_TOKEN:
                if (nla_get_u32(attr, &event->token) == 0)
                    event->has_token = 1;
                break;
            case MPTCP_ATTR_FAMILY:
                if (nla_get_u16(attr, &event->family) == 0)
                    event->has_family = 1;
                break;
            case MPTCP_ATTR_LOC_ID:
                if (nla_get_u8(attr, &event->loc_id) == 0)
                    event->has_loc_id = 1;
                break;
            case MPTCP_ATTR_REM_ID:
                if (nla_get_u8(attr, &event->rem_id) == 0)
                    event->has_rem_id = 1;
                break;
            case MPTCP_ATTR_IF_IDX:
                if (nla_get_u32(attr, &event->if_idx) == 0)
                    event->has_if_idx = 1;
                break;
            case MPTCP_ATTR_BACKUP: {
                uint8_t backup;

                if (nla_get_u8(attr, &backup) == 0) {
                    event->has_backup = 1;
                    event->backup = backup != 0;
                }
                break;
            }
            case MPTCP_ATTR_SPORT:
                if (nla_get_u16(attr, &event->sport) == 0)
                    event->has_sport = 1;
                break;
            case MPTCP_ATTR_DPORT:
                if (nla_get_u16(attr, &event->dport) == 0)
                    event->has_dport = 1;
                break;
            case MPTCP_ATTR_SADDR4:
                if (nla_data_len(attr) >= (int)sizeof(struct in_addr)) {
                    memcpy(&event->saddr4, nla_data_ptr(attr), sizeof(struct in_addr));
                    event->has_saddr4 = 1;
                }
                break;
            case MPTCP_ATTR_DADDR4:
                if (nla_data_len(attr) >= (int)sizeof(struct in_addr)) {
                    memcpy(&event->daddr4, nla_data_ptr(attr), sizeof(struct in_addr));
                    event->has_daddr4 = 1;
                }
                break;
            case MPTCP_ATTR_SADDR6:
                if (nla_data_len(attr) >= (int)sizeof(struct in6_addr)) {
                    memcpy(&event->saddr6, nla_data_ptr(attr), sizeof(struct in6_addr));
                    event->has_saddr6 = 1;
                }
                break;
            case MPTCP_ATTR_DADDR6:
                if (nla_data_len(attr) >= (int)sizeof(struct in6_addr)) {
                    memcpy(&event->daddr6, nla_data_ptr(attr), sizeof(struct in6_addr));
                    event->has_daddr6 = 1;
                }
                break;
            default:
                break;
        }

        attr = nla_next(attr, &rem);
    }

    return 0;
}

int mptcp_pm_format_endpoints(const struct mptcp_pm_event* event,
                              char* local,
                              size_t local_len,
                              char* remote,
                              size_t remote_len) {
    char laddr[INET6_ADDRSTRLEN];
    char raddr[INET6_ADDRSTRLEN];

    if (event == NULL || local == NULL || remote == NULL)
        return -1;
    if (!event->has_family)
        return -1;

    if (event->family == AF_INET) {
        if (!event->has_saddr4 || !event->has_daddr4)
            return -1;
        if (inet_ntop(AF_INET, &event->saddr4, laddr, sizeof(laddr)) == NULL)
            return -1;
        if (inet_ntop(AF_INET, &event->daddr4, raddr, sizeof(raddr)) == NULL)
            return -1;
        snprintf(local, local_len, "%s:%u", laddr, (unsigned int)(event->has_sport ? event->sport : 0));
        snprintf(remote, remote_len, "%s:%u", raddr, (unsigned int)(event->has_dport ? event->dport : 0));
        return 0;
    }

    if (event->family == AF_INET6) {
        if (!event->has_saddr6 || !event->has_daddr6)
            return -1;
        if (inet_ntop(AF_INET6, &event->saddr6, laddr, sizeof(laddr)) == NULL)
            return -1;
        if (inet_ntop(AF_INET6, &event->daddr6, raddr, sizeof(raddr)) == NULL)
            return -1;
        snprintf(local, local_len, "[%s]:%u", laddr, (unsigned int)(event->has_sport ? event->sport : 0));
        snprintf(remote, remote_len, "[%s]:%u", raddr, (unsigned int)(event->has_dport ? event->dport : 0));
        return 0;
    }

    return -1;
}

int mptcp_pm_parse_ctrl_getfamily(const struct nlmsghdr* nlh,
                                  size_t nlh_len,
                                  const char* group_name,
                                  uint16_t* family_id,
                                  uint32_t* group_id) {
    const struct genlmsghdr* ghdr;
    const struct nlattr* attr;
    int rem;
    uint16_t fam = 0;
    uint32_t grp = 0;
    int fam_ok = 0;
    int grp_ok = 0;

    if (nlh == NULL || family_id == NULL || group_id == NULL)
        return -1;
    if (nlh_len < sizeof(*nlh) || nlh->nlmsg_len < NLMSG_LENGTH(GENL_HDRLEN) || nlh->nlmsg_len > nlh_len)
        return -1;

    ghdr = (const struct genlmsghdr*)((const char*)nlh + NLMSG_HDRLEN);
    if (ghdr->cmd != CTRL_CMD_NEWFAMILY && ghdr->cmd != CTRL_CMD_GETFAMILY)
        return -1;

    rem = (int)nlh->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN;
    attr = (const struct nlattr*)((const char*)ghdr + GENL_HDRLEN);

    while (nla_ok(attr, rem)) {
        uint16_t type = attr->nla_type & NLA_TYPE_MASK;

        if (type == CTRL_ATTR_FAMILY_ID) {
            if (nla_get_u16(attr, &fam) == 0)
                fam_ok = 1;
        }
        else if (type == CTRL_ATTR_MCAST_GROUPS) {
            int nrem = nla_data_len(attr);
            const struct nlattr* group = (const struct nlattr*)nla_data_ptr(attr);

            while (nla_ok(group, nrem)) {
                int grem = nla_data_len(group);
                const struct nlattr* ga = (const struct nlattr*)nla_data_ptr(group);
                uint32_t candidate_id = 0;
                int has_id = 0;
                int matches_name = 0;

                while (nla_ok(ga, grem)) {
                    uint16_t gtype = ga->nla_type & NLA_TYPE_MASK;

                    if (gtype == CTRL_ATTR_MCAST_GRP_NAME)
                        matches_name = nla_string_eq(ga, group_name);
                    else if (gtype == CTRL_ATTR_MCAST_GRP_ID)
                        has_id = (nla_get_u32(ga, &candidate_id) == 0);

                    ga = nla_next(ga, &grem);
                }

                if (matches_name && has_id) {
                    grp = candidate_id;
                    grp_ok = 1;
                }

                group = nla_next(group, &nrem);
            }
        }

        attr = nla_next(attr, &rem);
    }

    if (!fam_ok || !grp_ok)
        return -1;

    *family_id = fam;
    *group_id = grp;
    return 0;
}

#endif /* __linux__ */
