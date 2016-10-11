/* getip.c: simple tool to get ip address and little more of given interface
 *
 * Copyright (c) 2015, Aliaksei Katovich <aliaksei.katovich at gmail.com>
 *
 * Released under the GNU General Public License, version 2
 */

#include <stdio.h>
#include <errno.h>
#include <string.h>

#include <sys/socket.h>
#include <linux/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <net/if_arp.h>

#define prhwaddr0() printf("- ")
#define pripaddr0() printf("- ")
#define praddr(req)\
	printf("%s ", inet_ntoa(((struct sockaddr_in *) &req)->sin_addr))

int main(int argc, const char *argv[])
{
	int sd, rc;
	struct ifreq ifr = { 0 };

	if (argc != 2) {
		fprintf(stderr, "Usage: %s <iface>\n", argv[0]);
		goto err;
	}

	sd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sd < 0) {
		fprintf(stderr, "socket() failed, %s\n", strerror(errno));
		goto err;
	}

	strncpy(ifr.ifr_name, argv[1], sizeof(ifr.ifr_name));

	rc = ioctl(sd, SIOCGIFADDR, &ifr);
	if (rc < 0) {
		fprintf(stderr, "ioctl(SIOCGIFADDR) failed, %s\n",
			strerror(errno));
		pripaddr0();
	} else {
		praddr(ifr.ifr_addr);
	}

	rc = ioctl(sd, SIOCGIFNETMASK, &ifr);
	if (rc < 0) {
		fprintf(stderr, "ioctl(SIOCGIFNETMASK) failed, %s\n",
			strerror(errno));
		pripaddr0();
	} else {
		praddr(ifr.ifr_netmask);
	}

	rc = ioctl(sd, SIOCGIFBRDADDR, &ifr);
	if (rc < 0) {
		fprintf(stderr, "ioctl(SIOCGIFBRDADDR) failed, %s\n",
			strerror(errno));
		pripaddr0();
	} else {
		praddr(ifr.ifr_broadaddr);
	}

	rc = ioctl(sd, SIOCGIFHWADDR, &ifr);
	if (rc < 0) {
		fprintf(stderr, "ioctl(SIOCGIFHWADDR) failed, %s\n",
			strerror(errno));
		pripaddr0();
	} else if (ifr.ifr_hwaddr.sa_family != ARPHRD_ETHER) {
		prhwaddr0();
	} else {
		char *ptr = (char *) ifr.ifr_hwaddr.sa_data;
		printf("%02x:%02x:%02x:%02x:%02x:%02x ",
		       *ptr & 0xff, *(ptr + 1) & 0xff,
		       *(ptr + 2) & 0xff, *(ptr + 3) & 0xff,
		       *(ptr + 4) & 0xff, *(ptr + 5) & 0xff);
	}

	printf("\n");
	close(sd);
	return 0;
err:
	printf("- - - -\n");
	return 1;
}
