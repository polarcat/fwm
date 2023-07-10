/* clock.c: simple xclock
 *
 * Copyright (c) 2015, Aliaksei Katovich <aliaksei.katovich at gmail.com>
 *
 * Released under the GNU General Public License, version 2
 */

#include "text.h"
#include "misc.h"

#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <poll.h>

#include <xcb/xcb.h>

#define DEFAULT_FONT_SIZE 10.5
#define DEFAULT_DPI 96
#define DEFAULT_STR "0000-00-00/00 Nil 00:00"

static xcb_connection_t *dpy;
static xcb_drawable_t win;
static xcb_gcontext_t gc;
static uint8_t done;

static uint16_t width;
static uint16_t height;

static uint32_t bg = 0x202020;
static uint32_t fg = 0xa0a0a0;

static struct text *text;
static struct xcb xcb;

static const char *timefmt;
static const char *cmd;

static char str[200];
static char prev_str[200];

static void clear(int16_t x, int16_t y, uint16_t w, uint16_t h)
{
	xcb_rectangle_t rect = { x, y, w, h, };
	xcb_change_gc(dpy, gc, XCB_GC_FOREGROUND, &bg);
	xcb_poly_fill_rectangle(dpy, win, gc, 1, &rect);
}

static void print_time(uint8_t force)
{
	time_t t;
	struct tm *tmp;

	t = time(NULL);

	if (!(tmp = localtime(&t)))
		return;

	if (!strftime(str, sizeof(str), timefmt, tmp))
		return;

	if (strncmp(prev_str, str, sizeof(str)) == 0 && !force)
		return;

	memcpy(prev_str, str, sizeof(prev_str));
	set_text_str(text, str, strlen(str));
	draw_text_xcb(&xcb, text);
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

static void handle_configure_notify(xcb_configure_notify_event_t *e)
{
	if (e->height > height) {
		set_text_pos(text, 0, (e->height - height) / 2);
	}

	dd("prop:\n"
	   " response_type=%u\n"
	   " sequence=%u\n"
	   " event=%#x\n"
	   " window=%#x\n"
	   " above_sibling=%#x\n"
	   " x=%d\n"
	   " y=%d\n"
	   " width=%u\n"
	   " height=%u\n"
	   " border_width=%u\n"
	   " override_redirect=%u\n",
	   e->response_type,
	   e->sequence,
	   e->event,
	   e->window,
	   e->above_sibling,
	   e->x,
	   e->y,
	   e->width,
	   e->height,
	   e->border_width,
	   e->override_redirect);
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
			print_time(1);
			break;
		}
		break;
	case XCB_CONFIGURE_NOTIFY:
		handle_configure_notify((xcb_configure_notify_event_t *) e);
		break;
	case XCB_EXPOSE:
		print_time(1);
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
	fontid_t font_id;
	struct pollfd pfd;
	uint32_t mask;
	xcb_screen_t *scr;
	uint32_t val[2];
	const char *arg;
	const char *name;
	uint8_t name_len;
	xcb_atom_t atom;
	uint16_t hdpi = 0;
	uint16_t vdpi = 0;
	float font_size = 0;

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
		} else if (opt(arg, "-fs", "--font-size")) {
			font_size = strtof(argv[argc + 1], NULL);
		} else if (opt(arg, "-hd", "--horiz-dpi")) {
			hdpi = atoi(argv[argc + 1]);
		} else if (opt(arg, "-vd", "--vert-dpi")) {
			vdpi = atoi(argv[argc + 1]);
		}
	}

	if (font_size == 0) {
		font_size = DEFAULT_FONT_SIZE;
		ww("using default size %.02f\n", font_size);
	}

	if (hdpi == 0 || vdpi == 0) {
		hdpi = DEFAULT_DPI;
		vdpi = DEFAULT_DPI;
		ww("using default dpi %u\n", hdpi);
	}

	if (!timefmt) {
		printf("Usage: %s <options>\n"
			"Options:\n"
			"-n, --name <str>          window name (%s)\n"
			"-fs, --font-size <float>  font size (%.02f)\n"
			"-hd, --horiz-dpi <int>    horizontal display DPI (%u)\n"
			"-vd, --vert-dpi <int>     vertical display DPI (%u)\n"
			"-f, --fmt <str>           RFC 2822-compliant date format\n"
			"-c, --cmd <str>           command to run on mouse click\n"
			"-bg, --bgcolor <hex>      clock background color (0x%x)\n"
			"-fg, --fgcolor <hex>      clock foreground color (0x%x)\n\n"
			, argv[0], name, font_size, hdpi, vdpi, bg, fg);
		return 1;
	}

	font_id = open_font(getenv("FWM_FONT"), hdpi, vdpi);
	if (invalid_font_id(font_id)) {
		return 1;
	}

	if (!(text = create_text())) {
		return 1;
	}
	set_text_font(text, font_id, font_size);
	set_text_pos(text, 0, 0);

	dpy = xcb_connect(NULL, NULL);
	if (!dpy) {
		ee("xcb_connect() failed\n");
		return 1;
	}

	scr = xcb_setup_roots_iterator(xcb_get_setup(dpy)).data;
	gc = xcb_generate_id(dpy);

	mask = XCB_GC_GRAPHICS_EXPOSURES;
	val[0] = 0;
	xcb_create_gc(dpy, gc, scr->root, mask, val);
	win = xcb_generate_id(dpy);

	set_text_str(text, DEFAULT_STR, sizeof(DEFAULT_STR));
	get_text_size(text, &width, &height);
	set_text_color(text, fg, bg);

	mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	val[0] = bg;
	val[1] = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_VISIBILITY_CHANGE |
		 XCB_EVENT_MASK_KEY_PRESS  | XCB_EVENT_MASK_BUTTON_PRESS |
		 XCB_EVENT_MASK_POINTER_MOTION;
	val[1] |= XCB_EVENT_MASK_STRUCTURE_NOTIFY;
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
	clear(0, 0, width, height);
	xcb_flush(dpy);

	pfd.fd = xcb_get_file_descriptor(dpy);
	pfd.events = POLLIN;
	pfd.revents = 0;

	xcb.dpy = dpy;
	xcb.win = win;
	xcb.gc = gc;

	while (!done) {
		int rc = poll(&pfd, 1, 5000);
		if (rc == 0) { /* timeout */
			print_time(0);
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

	destroy_text(&text);
	xcb_destroy_window(dpy, win);
	xcb_disconnect(dpy);
	close_font(font_id);
	return 0;
}
