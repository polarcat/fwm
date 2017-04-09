/* yawm.h: yet another window manager header
 *
 * Copyright (c) 2015, Aliaksei Katovich <aliaksei.katovich at gmail.com>
 *
 * Released under the GNU General Public License, version 2
 */

#ifndef YAWM_H
#define YAWM_H

#include <stdint.h>

#define FONT1_SIZE 10.5
#define FONT1_NAME "Monospace"
#define FONT2_SIZE 10.5
#define FONT2_NAME "fontawesome"

#define YAWM_BASE ".yawm"
#define YAWM_LIST "winlist"
#define YAWM_FIFO ".yawm"
#define YAWMD_FIFO ".yawmd"

enum reqtype {
	REQTYPE_CLEAN,
	REQTYPE_STORE,
	REQTYPE_FLUSH,
	REQTYPE_RESET,
};

struct clientinfo {
	uint8_t scr;
	uint8_t tag;
	uint32_t win;
};

struct clientreq {
	uint8_t type;
	struct clientinfo info;
};

#endif /* YAWM_H */
