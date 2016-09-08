/* yawm.h: yet another window manager header
 *
 * Copyright (c) 2015, Aliaksei Katovich <aliaksei.katovich at gmail.com>
 *
 * Released under the GNU General Public License, version 2
 */

#ifndef YAWM_H
#define YAWM_H

#include <stdint.h>

#define FONT_SIZE 10.5
#define FONT_NAME "Monospace"

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
