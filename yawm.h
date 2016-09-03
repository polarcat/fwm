/* yawm.h: yet another window manager header
 *
 * Copyright (c) 2015, Aliaksei Katovich <aliaksei.katovich at gmail.com>
 *
 * Released under the GNU General Public License, version 2
 */

#ifndef YAWM_H
#define YAWM_H

#include <stdint.h>

#include "list.h"

#define FONT_SIZE 10.5
#define FONT_NAME "Monospace"

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

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define YAWM_BASE ".yawm"
#define YAWM_LIST "winlist"
#define YAWM_FIFO ".yawmd"

enum reqtype {
	REQTYPE_CLEAN,
	REQTYPE_STORE,
	REQTYPE_FLUSH,
	REQTYPE_RESET,
};

struct clientinfo {
	uint32_t win;
	uint8_t scr;
	uint8_t tag;
};

struct clientreq {
	uint8_t type;
	struct clientinfo info;
};

#endif /* YAWM_H */
