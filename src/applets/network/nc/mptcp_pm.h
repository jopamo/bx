#ifndef MPTCP_PM_H
#define MPTCP_PM_H

#ifdef __linux__

#include <stddef.h>
#include <stdint.h>
#include <linux/netlink.h>
#include <netinet/in.h>

struct mptcp_pm_event {
    uint8_t cmd;

    int has_token;
    uint32_t token;

    int has_family;
    uint16_t family;

    int has_loc_id;
    uint8_t loc_id;

    int has_rem_id;
    uint8_t rem_id;

    int has_if_idx;
    uint32_t if_idx;

    int has_backup;
    int backup;

    int has_sport;
    uint16_t sport;

    int has_dport;
    uint16_t dport;

    int has_saddr4;
    struct in_addr saddr4;

    int has_daddr4;
    struct in_addr daddr4;

    int has_saddr6;
    struct in6_addr saddr6;

    int has_daddr6;
    struct in6_addr daddr6;
};

void mptcp_pm_reset_event(struct mptcp_pm_event* event);
int mptcp_pm_parse_genl_event(const struct nlmsghdr* nlh, size_t nlh_len, struct mptcp_pm_event* event);
int mptcp_pm_format_endpoints(const struct mptcp_pm_event* event,
                              char* local,
                              size_t local_len,
                              char* remote,
                              size_t remote_len);
int mptcp_pm_parse_ctrl_getfamily(const struct nlmsghdr* nlh,
                                  size_t nlh_len,
                                  const char* group_name,
                                  uint16_t* family_id,
                                  uint32_t* group_id);

#endif /* __linux__ */

#endif /* MPTCP_PM_H */
