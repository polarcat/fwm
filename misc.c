/* misc.c: miscellaneous utils
 *
 * Copyright (c) 2015, Aliaksei Katovich <aliaksei.katovich at gmail.com>
 *
 * Released under the GNU General Public License, version 2
 */

#include <poll.h>
#include <signal.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/uio.h>

#include "misc.h"

#ifndef POLLFD_TIMEOUT
#define POLLFD_TIMEOUT 3000 /* ms */
#endif

int16_t pollfd(int fd, int16_t events, int timeout)
{
	int16_t ret;
	struct pollfd pfd;

	pfd.fd = fd;
	pfd.events = events;
	pfd.revents = 0;

repoll:
	ret = poll(&pfd, 1, timeout);
	if (ret == 0) {
		errno = EAGAIN;
		ee("poll() timeout\n");
		return -1;
	}

	if (ret < 0) {
		if (errno == EINTR) {
			ii("EINTR occured during poll(), restart\n");
			goto repoll;
		}
		ee("poll() failed\n");
		return -1;
	}

	return pfd.revents;
}

size_t pushv(int fd, const struct iovec *iov, int8_t iovcnt)
{
	int16_t events;

	events = pollfd(fd, POLLOUT, POLLFD_TIMEOUT);
	if (events < 0)
		return -1;

	if (events & POLLOUT)
		return writev(fd, iov, iovcnt);

	ee("unhandled poll events %x\n", events);
	return -1;
}

size_t push(int fd, const void *data, size_t size)
{
	struct iovec iov ;

	iov.iov_base = (void *) data;
	iov.iov_len = size;

	return pushv(fd, &iov, 1);
}

size_t pullv(int fd, const struct iovec *iov, int8_t iovcnt, int timeout)
{
	int16_t events;

	events = pollfd(fd, POLLIN, POLLFD_TIMEOUT);
	if (events < 0)
		return -1;

	if (events & POLLIN)
		return readv(fd, iov, iovcnt);

	ee("unhandled poll events %x\n", events);
	return -1;
}

size_t pull(int fd, void *data, size_t size, int timeout)
{
	struct iovec iov;

	iov.iov_base = data;
	iov.iov_len = size;

	return pullv(fd, &iov, 1, timeout);
}

size_t pullstr(int fd, char *buf, int16_t len, struct request *req,
	       int8_t reqcnt)
{
	int16_t events;

	events = pollfd(fd, POLLIN, POLLFD_TIMEOUT);
	if (events < 0)
		return -1;

	if (events & POLLIN) {
		size_t bytes;
		struct iovec iov;
		char *str;
		struct request *reqptr, *reqend;
		char *bufptr, *bufend;

		iov.iov_base = buf;
		iov.iov_len = len;

		if ((bytes = readv(fd, &iov, 1)) < 0)
			return -1;

		str = buf;
		bufptr = buf;
		bufend = buf + bytes;
		reqptr = req;
		reqend = req + reqcnt;

		while (bufptr <= bufend && reqptr < reqend) {
			if (!isprint(*bufptr)) {
				reqptr->str = str;
				reqptr->len = bufptr - str;
				reqptr++;
				*bufptr++ = '\0';
				while (!isprint(*bufptr++)) {
					if (bufptr >= bufend)
						return reqptr - req;
				}
				str = bufptr - 1;
			}
			bufptr++;
		};

		return reqptr - req;
	}

	ee("unhandled poll events %x\n", events);
	return -1;
}
