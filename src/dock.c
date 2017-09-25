/* dock.c: run command by clicking icon
 *
 * Copyright (c) 2017, Aliaksei Katovich <aliaksei.katovich at gmail.com>
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
#include <xcb/xcb_util.h>

#include <X11/Xlib-xcb.h>
#include <X11/Xft/Xft.h>

#include "yawm.h"
#include "misc.h"

static char *font_name_ = FONT2_NAME;
static float font_size_ = FONT1_SIZE;
static uint32_t fg_ = 0x808080;
static uint32_t bg_ = 0x202020;

static int xscr_;
static Display *xdpy_;
static xcb_connection_t *dpy_;
static xcb_drawable_t win_;
static xcb_gcontext_t gc_;
static uint8_t done_;

static XftFont *font_;
static XftDraw *draw_;
static XftColor fgx_;

static const char *icon_;
static uint8_t len_;
static const char *cmd_;

static uint16_t x_;
static uint16_t y_;
static uint16_t w_;
static uint16_t h_;
static uint16_t margin_;
static XGlyphInfo ext_;

static void draw(void)
{
	xcb_rectangle_t rect = { 0, 0, w_, h_, };
	XftChar8 *icon = (XftChar8 *) icon_;

	xcb_change_gc(dpy_, gc_, XCB_GC_FOREGROUND, &bg_);
	xcb_poly_fill_rectangle(dpy_, win_, gc_, 1, &rect);
	XftDrawStringUtf8(draw_, &fgx_, font_, x_, y_, icon, len_);
	XSync(xdpy_, 0);
}

static void *task(void *arg)
{
	system((const char *) arg);
	dd("'%s' exited", (const char *) arg);
	return NULL;
}

static void spawn(const char *cmd)
{
	pthread_t t;
	const char *home;

	if ((home = getenv("HOME")))
		chdir(home);

	pthread_create(&t, NULL, task, (void *) cmd);
}

static void resize(xcb_resize_request_event_t *e)
{
	dd("win 0x%x (0x%x) dim %ux%u", e->window, win_, e->width, e->height);

	if (h_ != e->height) {
		h_ = e->height;
		y_ = (h_ + ext_.y) / 2;
	}
}

static int events(void)
{
	uint8_t type;
	xcb_generic_event_t *e = xcb_wait_for_event(dpy_);

	if (!e)
		return 0;

	type = XCB_EVENT_RESPONSE_TYPE(e);

	switch (type) {
	case XCB_VISIBILITY_NOTIFY:
		switch (((xcb_visibility_notify_event_t *) e)->state) {
		case XCB_VISIBILITY_PARTIALLY_OBSCURED: /* fall through */
		case XCB_VISIBILITY_UNOBSCURED:
			dd("XCB_VISIBILITY_UNOBSCURED");
			draw();
			break;
		}
		break;
	case XCB_EXPOSE:
		draw();
		break;
	case XCB_KEY_PRESS:
		if (((xcb_key_press_event_t *)e)->detail == 9)
			done_ = 1;
		break;
	case XCB_BUTTON_PRESS:
		if (cmd_)
			spawn(cmd_);
		break;
	case XCB_RESIZE_REQUEST:
		dd("XCB_RESIZE_REQUEST");
		resize((xcb_resize_request_event_t *) e);
		break;
	}

	free(e);
	return 1;
}

static XftFont *load_font(char *info, float defsize)
{
	XftFont *font;
	float size = 0.f;
	char *ptr = info;
	char *end = info + strlen(info);
	char *size_ptr = NULL;

	while (ptr < end) {
		if (*ptr == ':') {
			size_ptr = ptr + 1;
			*ptr = '\0';
			break;
		}

		ptr++;
	}

	if (size_ptr && strlen(size_ptr))
		size = strtof(size_ptr, NULL);

	if (!size)
		size = defsize;

	font = XftFontOpen(xdpy_, xscr_, XFT_FAMILY, XftTypeString, info,
			    XFT_SIZE, XftTypeDouble, size, NULL);

	if (!font) {
		ee("XftFontOpen(%s) failed\n", info);
		return NULL;
	}

	dd("loaded font %s size %.1f", info, size);
	return font;
}

static int opt(const char *arg, const char *args, const char *argl)
{
	return (strcmp(arg, args) == 0 || strcmp(arg, argl) == 0);
}

static void help(const char *prog, const char *name)
{
	printf("Usage: %s <options>\n"
	       "Options:\n"
	       "-n, --name <str>      window name (%s)\n"
	       "-f, --font <font>     icon font (%s:%.1f)\n"
	       "-m, --margin <num>    icon margin (%u)\n"
	       "-s, --size <num>      window size (auto)\n"
	       "-i, --icon <glyph>    icon to display\n"
	       "-c, --cmd <str>       command to run on mouse click\n"
	       "-bg, --bgcolor <hex>  rgb color, default 0x%x\n"
	       "-fg, --fgcolor <hex>  rgb color, default 0x%x\n\n",
               prog, name, font_name_, font_size_, margin_, bg_, fg_);
}

int main(int argc, char *argv[])
{
	uint32_t mask;
	xcb_screen_t *scr;
	uint32_t val[2];
	XRenderColor ref;
	const char *arg;
	const char *name = "dock";
	uint8_t name_len;
	uint16_t size;

	if (argc < 2) {
		help(argv[0], name);
		exit(0);
	}

	size = 0; /* auto */

	while (argc > 1) {
		arg = argv[--argc];
		if (opt(arg, "-f", "--font")) {
			font_name_ = argv[argc + 1];
		} else if (opt(arg, "-m", "--margin")) {
			margin_ = atoi(argv[argc + 1]);
		} else if (opt(arg, "-s", "--size")) {
			size = atoi(argv[argc + 1]);
		} else if (opt(arg, "-i", "--icon")) {
			icon_ = argv[argc + 1];
		} else if (opt(arg, "-c", "--cmd")) {
			cmd_ = argv[argc + 1];
		} else if (opt(arg, "-bg", "--bgcolor")) {
			bg_ = strtol(argv[argc + 1], NULL, 16);
		} else if (opt(arg, "-fg", "--fgcolor")) {
			fg_ = strtol(argv[argc + 1], NULL, 16);
		} else if (opt(arg, "-n", "--name")) {
			name = argv[argc + 1];
		}
	}

	if (!icon_) {
		help(argv[0], name);
		exit(0);
	}

	len_ = strlen(icon_);

	if (!(xdpy_ = XOpenDisplay(NULL))) {
		ee("XOpenDisplay() failed\n");
		return 1;
	}

	xscr_ = DefaultScreen(xdpy_);

	if (!(dpy_ = XGetXCBConnection(xdpy_))) {
		ee("xcb_connect() failed\n");
		return 1;
	}

	if (!(font_ = load_font(font_name_, font_size_)))
		return 1;


	ref.alpha = 0xffff;
	ref.red = (fg_ & 0xff0000) >> 8;
	ref.green = fg_ & 0xff00;
	ref.blue = (fg_ & 0xff) << 8;
	XftColorAllocValue(xdpy_, DefaultVisual(xdpy_, xscr_),
			DefaultColormap(xdpy_, xscr_), &ref,
			&fgx_);

	scr = xcb_setup_roots_iterator(xcb_get_setup(dpy_)).data;
	gc_ = xcb_generate_id(dpy_);

	mask = XCB_GC_FOREGROUND;
	val[0] = fg_;
	mask |= XCB_GC_GRAPHICS_EXPOSURES;
	val[1] = 0;
	xcb_create_gc(dpy_, gc_, scr->root, mask, val);

	win_ = xcb_generate_id(dpy_);
	draw_ = XftDrawCreate(xdpy_, win_,
			      DefaultVisual(xdpy_, xscr_),
			      DefaultColormap(xdpy_, xscr_));

	if (!draw_) {
		ee("XftDrawCreate() failed\n");
		return 1;
	}

	XftTextExtentsUtf8(xdpy_, font_, (XftChar8 *) icon_, len_, &ext_);

	x_ = margin_;

	if (size)
		w_ = h_ = size;
	else if (ext_.xOff > ext_.height)
		w_ = h_ = ext_.xOff + 2 * x_;
	else
		w_ = h_ = ext_.height + 2 * x_;

	y_ = (h_ + ext_.y) / 2;

	dd("info: %ux%u%+d%+d offs %+d%+d", ext_.width, ext_.height,
	   ext_.x, ext_.y, ext_.xOff, ext_.yOff);
	dd("font: asc %u desc %u aw %u", font_->ascent, font_->descent,
	   font_->max_advance_width);
	dd("win dim (%d,%d) text pos (%d,%d) icon '%s'", w_, h_, x_, y_, icon_);

	mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	val[0] = bg_;
	val[1] = XCB_EVENT_MASK_EXPOSURE;
	val[1] |= XCB_EVENT_MASK_VISIBILITY_CHANGE;
	val[1] |= XCB_EVENT_MASK_KEY_PRESS;
	val[1] |= XCB_EVENT_MASK_BUTTON_PRESS;
	val[1] |= XCB_EVENT_MASK_RESIZE_REDIRECT;

	xcb_create_window(dpy_, XCB_COPY_FROM_PARENT, win_, scr->root,
			  0, 0, w_, h_, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  scr->root_visual, mask, val);

	name_len = strlen(name);

        xcb_change_property(dpy_, XCB_PROP_MODE_REPLACE, win_,
			    XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                            name_len, name);

        xcb_change_property(dpy_, XCB_PROP_MODE_REPLACE, win_,
			    XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
			    name_len, name);

	xcb_map_window(dpy_, win_);
	xcb_flush(dpy_);

	while (1)
		events();

	xcb_destroy_window(dpy_, win_);

	if (font_)
		XftFontClose(xdpy_, font_);

	return 0;
}
