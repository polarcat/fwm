/* clock.c: simple xclock
 *
 * Copyright (c) 2015, Aliaksei Katovich <aliaksei.katovich at gmail.com>
 *
 * Released under the GNU General Public License, version 2
 */

#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <poll.h>

#include <xcb/xcb.h>

#include <X11/Xlib-xcb.h>
#include <X11/Xft/Xft.h>

#include "yawm.h"
#include "misc.h"

#define FONT_H_MARGIN 2

static int xscr;
static Display *xdpy;
static xcb_connection_t *dpy;
static xcb_drawable_t win;
static xcb_gcontext_t gc;
static uint8_t done;

static uint16_t width;
static uint16_t height;

static uint32_t bg = 0x202020;
static uint32_t fg = 0xa0a0a0;
static XftFont *font;
static XftDraw *draw;
static XftColor fgx;

static const char *timefmt;
static const char *cmd;
static uint16_t xpos;
static uint16_t ypos;

static char str[200];

static void textexts(const char *text, int len, uint16_t *w, uint16_t *h)
{
	XGlyphInfo ext;

	XftTextExtentsUtf8(xdpy, font, (XftChar8 *) text, len, &ext);

	dd("text: %s, geo (%d,%d %d,%d) off (%d,%d)\n",
	   text, ext.x, ext.y, ext.width, ext.height, ext.xOff, ext.yOff);

        ext.width % 2 ? (*w = ext.width + 1) : (*w = ext.width);
        ext.height % 2 ? (*h = ext.height + 1) : (*h = ext.height);
}

static void clear(int16_t x, int16_t y, uint16_t w, uint16_t h)
{
	xcb_rectangle_t rect = { x, y, w, h, };
	xcb_change_gc(dpy, gc, XCB_GC_FOREGROUND, &bg);
	xcb_poly_fill_rectangle(dpy, win, gc, 1, &rect);
}

static void printtime(void)
{
	time_t t;
	struct tm *tmp;

	t = time(NULL);

	if (!(tmp = localtime(&t)))
		return;

	if (!strftime(str, sizeof(str), timefmt, tmp))
		return;

	clear(0, 0, width, height);
	XftDrawStringUtf8(draw, &fgx, font, xpos, ypos, (XftChar8 *) str,
			  strlen(str));
	XSync(xdpy, 0);
}

static void wait(void)
{
	int state;
	xcb_generic_event_t *e;

	while (1) {
		e = xcb_wait_for_event(dpy);
		if (!e)
			continue;

		state = ((xcb_visibility_notify_event_t *) e)->state;
		free(e);
		if (state == XCB_VISIBILITY_UNOBSCURED)
			break;
		else if (state == XCB_VISIBILITY_PARTIALLY_OBSCURED)
			break;
	}
}

static int events(void)
{
	xcb_generic_event_t *e = xcb_poll_for_event(dpy);

	if (!e)
		return 0;

	switch (e->response_type & ~0x80) {
	case XCB_VISIBILITY_NOTIFY:
		switch (((xcb_visibility_notify_event_t *) e)->state) {
		case XCB_VISIBILITY_FULLY_OBSCURED:
			wait();
			break;
		case XCB_VISIBILITY_PARTIALLY_OBSCURED: /* fall through */
		case XCB_VISIBILITY_UNOBSCURED:
			dd("XCB_VISIBILITY_UNOBSCURED\n");
			printtime();
			break;
		}
		break;
	case XCB_EXPOSE:
		printtime();
		break;
	case XCB_KEY_PRESS:
		if (((xcb_key_press_event_t *)e)->detail == 9)
			done = 1;
		break;
	case XCB_BUTTON_PRESS:
		if (cmd)
			system(cmd);
		break;
	}
	free(e);
	return 1;
}

static int opt(const char *arg, const char *args, const char *argl)
{
	return (strcmp(arg, args) == 0 || strcmp(arg, argl) == 0);
}

static xcb_atom_t getatom(xcb_connection_t *dpy, const char *str, uint8_t len)
{
	xcb_intern_atom_cookie_t c;
	xcb_intern_atom_reply_t *r;
	xcb_atom_t a;

	c = xcb_intern_atom(dpy, 0, len, str);
	r = xcb_intern_atom_reply(dpy, c, NULL);

	if (!r) {
		ee("xcb_intern_atom(%s) failed\n", str);
		return (XCB_ATOM_NONE);
	}

	a = r->atom;
	free(r);
	return a;
}

int main(int argc, char *argv[])
{
	time_t t;
	struct tm *tm;
	struct pollfd pfd;
	uint32_t mask;
	xcb_screen_t *scr;
	uint32_t val[2];
	XRenderColor ref;
	const char *arg;
	const char *name;
	uint8_t name_len;
	xcb_atom_t atom;

	name = "clock";

	while (argc > 1) {
		arg = argv[--argc];
		if (opt(arg, "-f", "--fmt")) {
			timefmt = argv[argc + 1];
		} else if (opt(arg, "-n", "--name")) {
			name = argv[argc + 1];
		} else if (opt(arg, "-c", "--cmd")) {
			cmd = argv[argc + 1];
		} else if (opt(arg, "-bg", "--bgcolor")) {
			bg = strtol(argv[argc + 1], NULL, 16);
		} else if (opt(arg, "-fg", "--fgcolor")) {
			fg = strtol(argv[argc + 1], NULL, 16);
		}
	}

	if (!timefmt) {
		printf("Usage: %s <options>\n"
			"Options:\n"
			"-n, --name <str>      window name (%s)\n"
			"-f, --fmt <str>       RFC 2822-compliant date format\n"
			"-c, --cmd <str>       command to run on mouse click\n"
			"-bg, --bgcolor <hex>  clock background color (0x%x)\n"
			"-fg, --fgcolor <hex>  clock foreground color (0x%x)\n\n"
			, argv[0], name, bg, fg);
		return 1;
	}

	xdpy = XOpenDisplay(NULL);
	if (!xdpy) {
		ee("XOpenDisplay() failed\n");
		return 1;
	}
	xscr = DefaultScreen(xdpy);
	dpy = XGetXCBConnection(xdpy);
	if (!dpy) {
		ee("xcb_connect() failed\n");
		return 1;
	}

	font = XftFontOpen(xdpy, xscr, XFT_FAMILY, XftTypeString,
			   FONT1_NAME, XFT_SIZE, XftTypeDouble,
			   FONT1_SIZE, NULL);
	if (!font) {
		ee("XftFontOpen(%s)\n", FONT1_NAME);
		return 1;
	}

	ref.alpha = 0xffff;
	ref.red = (fg & 0xff0000) >> 8;
	ref.green = fg & 0xff00;
	ref.blue = (fg & 0xff) << 8;
	XftColorAllocValue(xdpy, DefaultVisual(xdpy, xscr),
			DefaultColormap(xdpy, xscr), &ref,
			&fgx);

	scr = xcb_setup_roots_iterator(xcb_get_setup(dpy)).data;
	gc = xcb_generate_id(dpy);

	mask = XCB_GC_FOREGROUND;
	val[0] = fg;
	mask |= XCB_GC_GRAPHICS_EXPOSURES;
	val[1] = 0;
	xcb_create_gc(dpy, gc, scr->root, mask, val);

	win = xcb_generate_id(dpy);
	draw = XftDrawCreate(xdpy, win,
			     DefaultVisual(xdpy, xscr),
			     DefaultColormap(xdpy, xscr));
	if (!draw) {
		ee("XftDrawCreate() failed\n");
		return 1;
	}

	t = time(NULL);

	if (!(tm = localtime(&t))) {
		ee("localtime() failed\n");
		return 1;
	}

	if (!strftime(str, sizeof(str), timefmt, tm)) {
		ee("strftime() failed\n");
		return 1;
	}

	textexts(str, strlen(str), &width, &height);

	xpos = 2;
	width += 2;

	ypos = font->ascent;
	height = font->ascent + font->descent + 2 * FONT_H_MARGIN;

	dd("win dim (%d,%d) text pos (%d,%d)\n", width, height, xpos, ypos);

	mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	val[0] = bg;
	val[1] = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_VISIBILITY_CHANGE |
		 XCB_EVENT_MASK_KEY_PRESS  | XCB_EVENT_MASK_BUTTON_PRESS |
		 XCB_EVENT_MASK_POINTER_MOTION;
	xcb_create_window(dpy, XCB_COPY_FROM_PARENT, win, scr->root,
			  0, 0, width, height, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  scr->root_visual, mask, val);

	name_len = strlen(name);

        xcb_change_property(dpy, XCB_PROP_MODE_REPLACE, win,
			    XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                            name_len, name);

        xcb_change_property(dpy, XCB_PROP_MODE_REPLACE, win,
			    XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
                            name_len, name);

	atom = getatom(dpy, "_NET_WM_PID", sizeof("_NET_WM_PID") - 1);

	if (atom != XCB_ATOM_NONE) {
		pid_t pid = getpid();
		xcb_change_property(dpy, XCB_PROP_MODE_REPLACE, win,
				    atom, XCB_ATOM_CARDINAL, 32, 1, &pid);
	}

	xcb_map_window(dpy, win);
	xcb_flush(dpy);

	pfd.fd = xcb_get_file_descriptor(dpy);
	pfd.events = POLLIN;
	pfd.revents = 0;

	while (!done) {
		int rc = poll(&pfd, 1, 5000);
		if (rc == 0) { /* timeout */
			printtime();
		} else if (rc < 0) {
			if (errno == EINTR)
				continue;
			sleep(1);
			continue;
		}

		if (pfd.revents & POLLHUP)
			break;
		else if (pfd.revents & POLLIN)
			while (events()) {} /* read all events */

		pfd.revents = 0;
	}

	xcb_destroy_window(dpy, win);
	if (font)
		XftFontClose(xdpy, font);

	return 0;
}
