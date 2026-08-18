/* Source: socat.c */
/* Copyright Gerhard Rieger and contributors (see file CHANGES) */
/* Published under the GNU General Public License V.2, see file COPYING */

/* This file contains code for the netlink operations */

#include "xiosysincludes.h"

#include "xioopen.h"

#include "xio-netlink.h"


#if HAVE_LINUX_RTNETLINK_H

int xio_netlink_mtu(
	int interface_index,
	unsigned int mtu)
{
	/* For request */
	struct {
		struct nlmsghdr  nh;
		struct ifinfomsg intf;
		char		 attrbuf[512];
	} req;
	struct rtattr *rta;
	int rtnetlink_sk;
	/* For answer */
	int len;
	struct nlmsghdr buf[8192/sizeof(struct nlmsghdr)];
	struct iovec iov = { buf, sizeof(buf) };
	struct sockaddr_nl sa;
	struct msghdr rtmsg = { &sa, sizeof(sa), &iov, 1
				/* rest is 0 or NULL */ };
	struct nlmsghdr *nh;

	Info2("Setting interface %d MTU to %u using netlink", interface_index, mtu);

	/* Send the modified attributes - rtnetlink(7) */
	rtnetlink_sk = Socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
	if (rtnetlink_sk < 0) {
		Error3("xio_netlink_mtu(index=%d, mtu=%u): socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE): %s",
		       interface_index, mtu, strerror(errno));
		return STAT_NORETRY;
	}
	memset(&req, 0, sizeof(req));
	req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(req.intf));
	req.nh.nlmsg_flags = NLM_F_REQUEST;
	req.nh.nlmsg_type = RTM_NEWLINK;
	req.intf.ifi_family = AF_UNSPEC;
	req.intf.ifi_index = interface_index;
	req.intf.ifi_change = 0xffffffff; /* ??? */
	req.intf.ifi_change = 0x0; /* ??? */
	rta = (struct rtattr *)(((char *) &req) +
				NLMSG_ALIGN(req.nh.nlmsg_len));
	rta->rta_type = IFLA_MTU;
	rta->rta_len = RTA_LENGTH(sizeof(mtu));
	req.nh.nlmsg_len = NLMSG_ALIGN(req.nh.nlmsg_len) +
		RTA_LENGTH(sizeof(mtu));
	req.nh.nlmsg_flags |= NLM_F_ACK; /* netlink(7) */
	memcpy(RTA_DATA(rta), &mtu, sizeof(mtu));
	if (Send(rtnetlink_sk, &req, req.nh.nlmsg_len, 0) < 0) {
		Error3("xio_netlink_mtu(index=%d, mtu=%u): send(): %s",
		       interface_index, mtu, strerror(errno));
		Close(rtnetlink_sk);
		return STAT_NORETRY;
	}

	/* Try to read the answer - netlink(7) */
	if ((len = Recvmsg(rtnetlink_sk, &rtmsg, MSG_DONTWAIT)) < 0) {
		Warn3("xio_netlink_mtu(index=%d, mtu=%u): recvmsg(): %s",
		      interface_index, mtu, strerror(errno));
		Close(rtnetlink_sk);
		return STAT_NORETRY;
	}

	for (nh = (struct nlmsghdr *)buf; NLMSG_OK (nh, len);
	     nh = NLMSG_NEXT(nh, len)) {
		/* The end of multipart message */
		if (nh->nlmsg_type == NLMSG_DONE) {
		Close(rtnetlink_sk);
			return STAT_OK;
		}

		if (nh->nlmsg_type == NLMSG_ERROR) {
			/* Do some error handling */
			struct nlmsgerr *err;
			err = (struct nlmsgerr *)(NLMSG_DATA(&buf));
			if (err->error != 0) {
				Error3("xio_netlink_mtu(index=%d, mtu=%u): rtnetlink: %s",
				       interface_index, mtu, strerror(-err->error));
				Close(rtnetlink_sk);
				return STAT_NORETRY;
			}
			/* It is the acknowledgement, good */
		}

		/* Continue with parsing payload */

	}

	Close(rtnetlink_sk);
	return 0;
}

#endif /* HAVE_LINUX_RTNETLINK_H */
