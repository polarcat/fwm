/* rtlink.c: wait for NETLINK_ROUTE event and exit.
 * Retrun 0 if event occured or 1 otherwise
 *
 * Copyright (c) 2017, Aliaksei Katovich <aliaksei.katovich at gmail.com>
 *
 * Released under the GNU General Public License, version 2
 */

#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <poll.h>
#include <net/if.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include "misc.h"

#define MAX_BUF 1

int main(void)
{
	int32_t size = MAX_BUF;
	struct sockaddr_nl sa = {0};
	struct pollfd fds;

	fds.fd = socket(PF_NETLINK, SOCK_RAW, NETLINK_ROUTE);

	if (fds.fd < 0) {
		ee("socket() failed\n");
		return 1;
	}

	sa.nl_family = AF_NETLINK;
	sa.nl_pid = getpid();
	sa.nl_groups = RTMGRP_LINK;

	setsockopt(fds.fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));

	if (bind(fds.fd, (struct sockaddr *) &sa, sizeof(sa)) < 0) {
		ee("bind() failed\n");
		return 1;
	}

	fds.events = POLLIN;
	fds.revents = 0;

	if (poll(&fds, 1, -1) < 0)
		return 1;

	if (fds.revents & POLLIN)
		return 0;

	return 1;
}
