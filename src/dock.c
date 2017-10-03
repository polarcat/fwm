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

#define TEXT_MAXLEN 16

static char *tft_name_ = FONT1_NAME;
static float tft_size_ = FONT1_SIZE;
static char *ift_name_ = FONT2_NAME;
static float ift_size_ = FONT2_SIZE;
static uint32_t fg_ = 0x808080;
static uint32_t bg_ = 0x202020;

static int xscr_;
static Display *xdpy_;
static xcb_screen_t *scr_;
static xcb_connection_t *dpy_;
static xcb_gcontext_t gc_;
static const char *name_ = "dock";
static uint8_t done_;

static xcb_drawable_t win_;
static uint16_t w_;
static uint16_t h_;

static XftFont *ift_;
static XftFont *tft_;
static XftDraw *draw_;
static XftColor fgx_;

static const char *cmd_;
static pid_t pid_;

static char *icon_;
static xcb_rectangle_t irect_;
static uint8_t ilen_;
static XGlyphInfo iinfo_;

static char *text_;
static xcb_rectangle_t trect_;
static uint8_t tlen_;
static XGlyphInfo tinfo_;

static void clean(void)
{
	xcb_rectangle_t rect = { 0, 0, w_, h_, };
	xcb_change_gc(dpy_, gc_, XCB_GC_FOREGROUND, &bg_);
	xcb_poly_fill_rectangle(dpy_, win_, gc_, 1, &rect);
}

static void draw(XftFont *font)
{
	xcb_rectangle_t *rect;
	XftChar8 *str;
	uint8_t len;

	if (font == ift_) {
		str = (XftChar8 *) icon_;
		len = ilen_;
		rect = &irect_;
	} else {
		str = (XftChar8 *) text_;
		len = tlen_;
		rect = &trect_;
	}

	XftDrawStringUtf8(draw_, &fgx_, font, rect->x, rect->y, str, len);
	xcb_flush(dpy_);
	XSync(xdpy_, 0);
}

static void show(void)
{
	clean();

	if (icon_)
		draw(ift_);

	if (text_)
		draw(tft_);
}

static void *task(void *arg)
{
	system((const char *) arg);
	dd("'%s' exited", (const char *) arg);
	return NULL;
}

static void spawn(const char *cmd)
{
	char *arg;
	size_t len = strlen(cmd) + sizeof("65535") + 1;
	pthread_t t;
	const char *home;

	if ((arg = calloc(1, len))) {
		snprintf(arg, len, "%s %u", cmd, pid_);
	} else {
		ww("calloc(%zu) failed\n", len);
		arg = (char *) cmd;
	}

	if ((home = getenv("HOME")))
		chdir(home);

	pthread_create(&t, NULL, task, (void *) arg);
}

static uint16_t h_margin(uint16_t h, XGlyphInfo *info)
{
	uint16_t margin;

	if (!info->height)
		return 0;

	margin = h - info->height;

	if (margin % 2)
		margin++;

	dd("h margin %u height %u", margin / 2, info->height);
	return margin / 2;
}

static void adjust_rects(uint16_t w, uint16_t h)
{
	if (icon_ && text_) {
		w_ = iinfo_.width + tft_size_ / 2 + tinfo_.width;
		irect_.x = iinfo_.x;
		irect_.width = iinfo_.width;
		irect_.y = iinfo_.y + h_margin(h, &iinfo_);
		irect_.height = h_;
		trect_.x = irect_.x + tft_size_ / 2 + iinfo_.width;
		trect_.width = tinfo_.width;
		trect_.y = tinfo_.y + h_margin(h, &tinfo_);
		trect_.height = h_;
	} else if (text_) {
		trect_.x = tinfo_.x;
		trect_.y = tinfo_.y + h_margin(h, &tinfo_);
		trect_.width = w_ = tinfo_.width;
		trect_.height = h_;
	} else { /* only icon */
		irect_.x = iinfo_.x;
		irect_.y = iinfo_.y + h_margin(h, &iinfo_);
		irect_.width = w_ = iinfo_.width;
		irect_.height = h_;
	}

	dd("i rect: %ux%u%+d%+d", irect_.width, irect_.height,
	   irect_.x, irect_.y);
	dd("t rect: %ux%u%+d%+d", trect_.width, trect_.height,
	   trect_.x, trect_.y);
	dd("win %ux%u icon '%s' pos %d,%d text '%s' pos %d,%d",
	   w_, h_, icon_, irect_.x, irect_.y, text_, trect_.x, trect_.y);
}

static xcb_atom_t getatom(const char *str, uint8_t len)
{
	xcb_intern_atom_cookie_t c;
	xcb_intern_atom_reply_t *r;
	xcb_atom_t a;

	c = xcb_intern_atom(dpy_, 0, len, str);
	r = xcb_intern_atom_reply(dpy_, c, NULL);

	if (!r) {
		ee("xcb_intern_atom(%s) failed\n", str);
		return (XCB_ATOM_NONE);
	}

	a = r->atom;
	free(r);
	return a;
}

static int create_window(void)
{
	uint32_t mask;
	uint32_t val[2];
	uint8_t name_len;
	xcb_atom_t atom;


	win_ = xcb_generate_id(dpy_);
	draw_ = XftDrawCreate(xdpy_, win_,
			      DefaultVisual(xdpy_, xscr_),
			      DefaultColormap(xdpy_, xscr_));

	if (!draw_) {
		ee("XftDrawCreate() failed\n");
		return -1;
	}

	gc_ = xcb_generate_id(dpy_);

	mask = XCB_GC_FOREGROUND;
	val[0] = fg_;
	mask |= XCB_GC_GRAPHICS_EXPOSURES;
	val[1] = 0;
	xcb_create_gc(dpy_, gc_, scr_->root, mask, val);

	mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	val[0] = bg_;
	val[1] = XCB_EVENT_MASK_EXPOSURE;
	val[1] |= XCB_EVENT_MASK_VISIBILITY_CHANGE;
	val[1] |= XCB_EVENT_MASK_KEY_PRESS;
	val[1] |= XCB_EVENT_MASK_BUTTON_PRESS;
	val[1] |= XCB_EVENT_MASK_RESIZE_REDIRECT;

	xcb_create_window(dpy_, XCB_COPY_FROM_PARENT, win_, scr_->root,
			  0, 0, w_, h_, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  scr_->root_visual, mask, val);

	name_len = strlen(name_);

	xcb_change_property(dpy_, XCB_PROP_MODE_REPLACE, win_,
			    XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
			    name_len, name_);

	xcb_change_property(dpy_, XCB_PROP_MODE_REPLACE, win_,
			    XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
			    name_len, name_);

	atom = getatom("_NET_WM_PID", sizeof("_NET_WM_PID") - 1);

	if (atom != XCB_ATOM_NONE) {
		pid_ = getpid();
		xcb_change_property(dpy_, XCB_PROP_MODE_REPLACE, win_, atom,
				    XCB_ATOM_CARDINAL, 32, 1, &pid_);
		dd("pid %u win 0x%x", pid_, win_);
	}

	xcb_map_window(dpy_, win_);
	xcb_flush(dpy_);

	return 0;
}

static void resize(xcb_resize_request_event_t *e)
{
	dd("win 0x%x size %ux%u --> %ux%u", e->window, w_, h_, e->width,
	   e->height);

	if (e->height > h_) { /* have to re-create window to resize gc */
		h_ = e->height;
		XftDrawDestroy(draw_);
		xcb_free_gc(dpy_, gc_);
		xcb_destroy_window(dpy_, win_);
		xcb_flush(dpy_);

		if (create_window() < 0) {
			ee("failed to create new window %ux%u\n", e->width, e->height);
			exit(1); /* consider it fatal */
		}
	}

	adjust_rects(e->width, e->height);
}

static void handle_message(xcb_client_message_event_t *e)
{
	char *dat = (char *) e->data.data8;
	char *end = dat + sizeof(e->data.data8);
	char *ptr = dat;
	char *msg[3] = {0};
	uint8_t i = 0;
	XRenderColor ref;

	dd("dat '%s' len %zu", (char *) e->data.data8, sizeof(e->data.data8));

	msg[i] = ptr;

	while (ptr < end) {
		if (*ptr == ' ') {
			*ptr = '\0';
			msg[++i] = ptr + 1;
		}

		ptr++;
	}

	if (msg[0] && icon_) {
		free(icon_);
		icon_ = strdup(msg[0]);
		dd("ico '%s' len %u", icon_, ilen_);
	}

	if (msg[1]) {
		fg_ = strtol(msg[1], NULL, 16);
		ref.alpha = 0xffff;
		ref.red = (fg_ & 0xff0000) >> 8;
		ref.green = fg_ & 0xff00;
		ref.blue = (fg_ & 0xff) << 8;
		XftColorAllocValue(xdpy_, DefaultVisual(xdpy_, xscr_),
				   DefaultColormap(xdpy_, xscr_), &ref,
				   &fgx_);
		dd("rgb '%s' len %zu", msg[1], strlen(msg[1]));
	}

	if (msg[2] && text_) {
		free(text_);
		text_ = strdup(msg[2]);
		tlen_ = strlen(text_);
		dd("txt '%s' len %u", text_, tlen_);
	}

	show();
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
			show();
			break;
		}
		break;
	case XCB_EXPOSE:
		show();
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
	case XCB_CLIENT_MESSAGE:
		handle_message((xcb_client_message_event_t *) e);
		break;
	default:
		dd("got message type %d", type);
	}

	xcb_flush(dpy_);
	free(e);
	return 1;
}

static void init_info(XGlyphInfo *info)
{
	XftFont *font;
	XftChar8 *str;
	uint8_t len;

	if (info == &iinfo_) {
		str = (XftChar8 *) icon_;
		len = ilen_;
		font = ift_;
	} else {
		str = (XftChar8 *) text_;
		len = tlen_;
		font = tft_;
	}

	XftTextExtentsUtf8(xdpy_, font, (XftChar8 *) str, len, info);

	dd("info: %ux%u%+d%+d offs %+d%+d", info->width, info->height,
	   info->x, info->y, info->xOff, info->yOff);
	dd("font: asc %u desc %u aw %u", font->ascent, font->descent,
	   font->max_advance_width);
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

static void help(const char *prog)
{
	printf("Usage: %s <options>\n"
	       "Options:\n"
	       "-n, --name <str>          window name (%s)\n"
	       "-fni, --icon-font <font>  icon font (%s:%.1f)\n"
	       "-fnt, --text-font <font>  icon font (%s:%.1f)\n"
	       "-i, --icon <glyph>        icon to display\n"
	       "-t, --text <str>          text next to icon (%u chars)\n"
	       "-c, --cmd <str>           command to run on mouse click\n"
	       "-bg, --bgcolor <hex>      rgb color, default 0x%x\n"
	       "-fg, --fgcolor <hex>      rgb color, default 0x%x\n\n",
               prog, name_, ift_name_, ift_size_, tft_name_, tft_size_,
	       TEXT_MAXLEN, bg_, fg_);
}

int main(int argc, char *argv[])
{
	XRenderColor ref;
	const char *arg;

	if (argc < 2) {
		help(argv[0]);
		exit(0);
	}

	while (argc > 1) {
		arg = argv[--argc];
		if (opt(arg, "-fni", "--icon-font")) {
			ift_name_ = argv[argc + 1];
		} else if (opt(arg, "-fnt", "--text-font")) {
			tft_name_ = argv[argc + 1];
		} else if (opt(arg, "-i", "--icon")) {
			icon_ = strdup(argv[argc + 1]);
		} else if (opt(arg, "-t", "--text")) {
			text_ = strdup(argv[argc + 1]);
		} else if (opt(arg, "-c", "--cmd")) {
			cmd_ = argv[argc + 1];
		} else if (opt(arg, "-bg", "--bgcolor")) {
			bg_ = strtol(argv[argc + 1], NULL, 16);
		} else if (opt(arg, "-fg", "--fgcolor")) {
			fg_ = strtol(argv[argc + 1], NULL, 16);
		} else if (opt(arg, "-n", "--name")) {
			name_ = argv[argc + 1];
		}
	}

	if (!icon_ && !text_) {
		ii("missing text or icon\n");
		help(argv[0]);
		exit(0);
	}

	if (!(xdpy_ = XOpenDisplay(NULL))) {
		ee("XOpenDisplay() failed\n");
		return 1;
	}

	xscr_ = DefaultScreen(xdpy_);

	if (!(dpy_ = XGetXCBConnection(xdpy_))) {
		ee("xcb_connect() failed\n");
		return 1;
	}

	if (icon_) {
		if (!(ift_ = load_font(ift_name_, ift_size_)))
			return 1;

		ilen_ = strlen(icon_);
		init_info(&iinfo_);
	}

	if (text_) {
		if (!(tft_ = load_font(tft_name_, ift_size_)))
			return 1;

		tlen_ = strlen(text_);
		init_info(&tinfo_);
	}

	if (tinfo_.width > iinfo_.width)
		w_ = tinfo_.width;
	else
		w_ = iinfo_.width;

	if (tinfo_.height > iinfo_.height)
		h_ = tft_->ascent + tft_->descent;
	else
		h_ = ift_->ascent + ift_->descent;

	adjust_rects(w_, h_);

	ref.alpha = 0xffff;
	ref.red = (fg_ & 0xff0000) >> 8;
	ref.green = fg_ & 0xff00;
	ref.blue = (fg_ & 0xff) << 8;
	XftColorAllocValue(xdpy_, DefaultVisual(xdpy_, xscr_),
			DefaultColormap(xdpy_, xscr_), &ref,
			&fgx_);

	scr_ = xcb_setup_roots_iterator(xcb_get_setup(dpy_)).data;

	if (create_window() < 0)
		return 1;

	while (1)
		events();

	xcb_destroy_window(dpy_, win_);

	if (ift_)
		XftFontClose(xdpy_, ift_);

	if (tft_)
		XftFontClose(xdpy_, tft_);

	return 0;
}
