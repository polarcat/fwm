/* misc.h: miscellaneous utils
 *
 * Copyright (c) 2015, Aliaksei Katovich <aliaksei.katovich at gmail.com>
 *
 * Released under the GNU General Public License, version 2
 */

#ifndef MISC_H
#define MISC_H

#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#ifndef MEMDEBUG
#define mallinfo_start(name) do {} while(0)
#define mallinfo_stop(name) do {} while(0)
#define mallinfo_call(fn) do {} while(0)
#else /* MEMDEBUG */
#include <malloc.h> /* for mallinfo */

#define mallinfo_start(name) struct mallinfo name = mallinfo();

#define mallinfo_stop(name) {\
	struct mallinfo name_ = mallinfo();\
	ii(#name": mem %d --> %d, diff %d\n", name.uordblks, name_.uordblks,\
	   name_.uordblks - name.uordblks);\
}

#define mallinfo_call(fn) {\
	struct mallinfo mi0__ = mallinfo(), mi1__;\
	ii("> %s:%d: "#fn"() mem before %d\n", __func__, __LINE__,\
	   mi0__.uordblks);\
	fn;\
	mi1__ = mallinfo();\
	ii("< %s:%d: "#fn"() mem after %d, diff %d\n", __func__, __LINE__,\
	   mi1__.uordblks, mi1__.uordblks - mi0__.uordblks);\
}
#endif /* MEMDEBUG */

#define eval(cond, act) { if (cond) { ee("assert "#cond"\n"); act; } }

#define ee(fmt, ...) {\
	int errno_save__ = errno;\
	fprintf(stderr, "(ee) %s: " fmt, __func__, ##__VA_ARGS__);\
	if (errno_save__ != 0)\
		fprintf(stderr, "(ee) %s: %s, errno=%d\n", __func__,\
		     strerror(errno_save__), errno_save__);\
	errno = errno_save__;\
	fprintf(stderr, "(ee) %s: %s at %d\n", __func__, __FILE__, __LINE__);\
}

#ifdef DEBUG
#define dd(fmt, ...) printf("(dd) %s: " fmt, __func__, ##__VA_ARGS__)
#else
#define dd(fmt, ...) do {} while(0)
#endif

#ifdef VERBOSE
#define mm(fmt, ...) printf("(==) " fmt, ##__VA_ARGS__)
#else
#define mm(fmt, ...) do {} while(0)
#endif

#define ww(fmt, ...) printf("(ww) " fmt, ##__VA_ARGS__)
#define ii(fmt, ...) printf("(ii) " fmt, ##__VA_ARGS__)

#ifdef TRACE
#define tt(fmt, ...) printf("(tt) %s: " fmt, __func__, ##__VA_ARGS__)
#else
#define tt(fmt, ...) do {} while(0)
#endif

#ifdef TRACE_EVENTS
#define te(fmt, ...) printf("(tt) %s: " fmt, __func__, ##__VA_ARGS__)
#else
#define te(fmt, ...) do {} while(0)
#endif

static inline void hexdump(const char *buf, int16_t len)
{
	int16_t i, ii, n;

	if (!buf)
		len = 0;

	printf("%u bytes, %p\n", len, buf);
	printf(" 0000 | ");
	for (i = 0, n = 1; i < len; i++) {
		if (i >= (16 * n)) {
			printf(" | ");
			for (ii = i - 16; ii < i; ii++)
				printf("%c", isprint(buf[ii]) ? buf[ii] : '.');
			printf("\n %04x | ", n);
			n++;
		}

		printf("%02x%02x ", buf[i], buf[i + 1]);
		i++;
	}
	printf("\n");
}

int16_t pollfd(int fd, int16_t events, int timeout);
size_t pull(int fd, void *data, size_t size, int timeout);
size_t push(int fd, const void *data, size_t size);
size_t pushv(int fd, const struct iovec *iov, int8_t iovcnt);
size_t pullv(int fd, const struct iovec *iov, int8_t iovcnt, int timeout);

struct request {
	char *str;
	int16_t len;
	int16_t flag; /* mark incomplete string */
};

size_t pullstr(int fd, char *buf, int16_t len, struct request *req,
	       int8_t reqcnt);

#endif /* MISC_H */
