/* rtlink.c: report NETLINK_ROUTE events.
 *
 * Copyright (c) 2017-2026, Aliaksei Katovich <aliaksei.katovich at gmail.com>
 *
 * Released under the GNU General Public License, version 2
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <poll.h>
#include <net/if.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include "misc.h"

#define MAX_BUF 8192

static inline void handle_message(struct nlmsghdr *nh)
{
	// 1. Link State Changes (ifconfig eth0 up/down)
	if (nh->nlmsg_type == RTM_NEWLINK) {
		printf("RTM_NEWLINK\n");
	} else if (nh->nlmsg_type == RTM_DELLINK) {
		printf("RTM_DELLINK\n");
	} else if (nh->nlmsg_type == RTM_NEWROUTE) {
		printf("RTM_NEWROUTE\n");
	} else if (nh->nlmsg_type == RTM_DELROUTE) {
		printf("RTM_DELROUTE\n");
	} else if (nh->nlmsg_type == RTM_NEWADDR) {
		printf("RTM_NEWADDR\n");
	} else if (nh->nlmsg_type == RTM_DELADDR) {
		printf("RTM_DELADDR\n");
	} else if (nh->nlmsg_type == RTM_GETADDR) {
		printf("RTM_GETADDR\n");
	} else {
		printf("0x%x\n", nh->nlmsg_type);
	}
	fflush(stdout);
}

static void handle_event(const char *buf, size_t n)
{
	struct nlmsghdr *nh;
	for (nh = (struct nlmsghdr *)buf; NLMSG_OK(nh, n);) {
		if (nh->nlmsg_type == NLMSG_DONE) {
			break;
		}

		if (nh->nlmsg_type != NLMSG_ERROR) {
			handle_message(nh);
		}
		nh = NLMSG_NEXT(nh, n);
	}
}

int main(int argc, const char *argv[])
{
	char buf[MAX_BUF];
	struct sockaddr_nl sa = {0};
	struct pollfd fds;

	fds.fd = socket(PF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fds.fd < 0) {
		ee("socket() failed\n");
		return 1;
	}

	sa.nl_family = AF_NETLINK;
	sa.nl_pid = getpid();
	sa.nl_groups = RTMGRP_LINK
		| RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR
		| RTMGRP_IPV4_ROUTE  | RTMGRP_IPV6_ROUTE;


	if (bind(fds.fd, (struct sockaddr *) &sa, sizeof(sa)) < 0) {
		ee("bind() failed\n");
		close(fds.fd);
		return 1;
	}

	fds.events = POLLIN;
	fds.revents = 0;

	while (1) {
		if (poll(&fds, 1, -1) < 0) {
			ee("poll(%d) failed\n", fds.fd);
			sleep(1);
			continue;
		}

		if (fds.revents & POLLIN) {
			ssize_t n = recv(fds.fd, buf, sizeof(buf), 0);
			if (n > 0) {
				handle_event(buf, n);
			}
		}
		fds.revents = 0;
	}

	close(fds.fd);
	return EXIT_FAILURE;
}
