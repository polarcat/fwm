/* netlink.c: wait for NETLINK_KOBJECT_UEVENT event and exit.
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

#include "misc.h"

#define MAX_BUF 256

int main(void)
{
        char msg[MAX_BUF] = {0};
	int32_t size = MAX_BUF;
	struct sockaddr_nl sa = {0};
	struct pollfd fds;

	fds.fd = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);

	if (fds.fd < 0) {
		ee("socket() failed\n");
		return 1;
	}

	sa.nl_family = AF_NETLINK;
	sa.nl_pid = getpid();
	sa.nl_groups = 1;

	setsockopt(fds.fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));

	if (bind(fds.fd, (struct sockaddr *) &sa, sizeof(sa)) < 0) {
		ee("bind() failed\n");
		return 1;
	}

	fds.events = POLLIN;
	fds.revents = 0;

	if (poll(&fds, 1, -1) < 0)
		return 1;

	if (fds.revents & POLLIN) {
		recv(fds.fd, msg, sizeof(msg), MSG_DONTWAIT);
		printf("%s\n", msg);
		return 0;
	}

	return 1;
}
