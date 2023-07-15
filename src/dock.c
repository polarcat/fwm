/* dock.c: run command by clicking icon
 *
 * Copyright (c) 2017, Aliaksei Katovich <aliaksei.katovich at gmail.com>
 *
 * Released under the GNU General Public License, version 2
 */

#include "misc.h"
#include "text.h"

#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <poll.h>

#include <xcb/xcb.h>
#include <xcb/xcb_util.h>

#define DEFAULT_FONT_SIZE 10.5
#define DEFAULT_DPI 96
#define TEXT_MAXLEN 16

struct text_info {
	fontid_t font_id;
	struct text *txt;
	char *str;
	uint8_t len;
	uint8_t x;
};

struct ctx {
	xcb_connection_t *dpy;
	xcb_screen_t *scr;
	pid_t pid;
	const char *name;
	const char *cmd;
	xcb_drawable_t win;
	xcb_gcontext_t gc;
	uint16_t w;
	uint16_t h;
	uint32_t fg;
	uint32_t bg;
	uint8_t done;
	float font_size;
	uint16_t hdpi;
	uint16_t vdpi;
	const char *text_font;
	const char *icon_font;
	uint8_t text_y;
	uint8_t text_h;
};

static struct ctx ctx_;
static struct text_info text_;
static struct text_info icon_;

static uint8_t update_text(struct text_info *text)
{
	if (!text->str)
		return 0;

	set_text_color(text->txt, ctx_.fg, ctx_.bg);
	set_text_pos(text->txt, text->x, ctx_.text_y);
	set_text_str(text->txt, text->str, text->len);
	return 1;
}

static inline void clear(void)
{
	xcb_rectangle_t rect = { 0, 0, ctx_.w, ctx_.h, };
	xcb_change_gc(ctx_.dpy, ctx_.gc, XCB_GC_FOREGROUND, &ctx_.bg);
	xcb_poly_fill_rectangle(ctx_.dpy, ctx_.win, ctx_.gc, 1, &rect);
}

static void show(void)
{
	struct xcb xcb = { ctx_.dpy, ctx_.win, ctx_.gc };

	clear();

	if (icon_.str && update_text(&icon_))
		draw_text_xcb(&xcb, icon_.txt);

	if (text_.str && update_text(&text_))
		draw_text_xcb(&xcb, text_.txt);
}

static void *task(void *arg)
{
	system((const char *) arg);
	dd("'%s' exited\n", (const char *) arg);
	return NULL;
}

static void spawn(const char *cmd)
{
	char *arg;
	size_t len = strlen(cmd) + sizeof("65535") + 1;
	pthread_t t;
	const char *home;

	if ((arg = calloc(1, len))) {
		snprintf(arg, len, "%s %u", cmd, ctx_.pid);
	} else {
		ww("calloc(%zu) failed\n", len);
		arg = (char *) cmd;
	}

	if ((home = getenv("HOME")))
		chdir(home);

	pthread_create(&t, NULL, task, (void *) arg);
}

static xcb_atom_t getatom(const char *str, uint8_t len)
{
	xcb_intern_atom_cookie_t c;
	xcb_intern_atom_reply_t *r;
	xcb_atom_t a;

	c = xcb_intern_atom(ctx_.dpy, 0, len, str);
	r = xcb_intern_atom_reply(ctx_.dpy, c, NULL);

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

	ctx_.win = xcb_generate_id(ctx_.dpy);
	ctx_.gc = xcb_generate_id(ctx_.dpy);

	mask = XCB_GC_FOREGROUND;
	val[0] = ctx_.fg;
	mask |= XCB_GC_GRAPHICS_EXPOSURES;
	val[1] = 0;
	xcb_create_gc(ctx_.dpy, ctx_.gc, ctx_.scr->root, mask, val);

	mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	val[0] = ctx_.bg;
	val[1] = XCB_EVENT_MASK_EXPOSURE;
	val[1] |= XCB_EVENT_MASK_VISIBILITY_CHANGE;
	val[1] |= XCB_EVENT_MASK_KEY_PRESS;
	val[1] |= XCB_EVENT_MASK_BUTTON_PRESS;
	val[1] |= XCB_EVENT_MASK_RESIZE_REDIRECT;

	xcb_create_window(ctx_.dpy, XCB_COPY_FROM_PARENT, ctx_.win,
	 ctx_.scr->root, 0, 0, ctx_.w, ctx_.h, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
	 ctx_.scr->root_visual, mask, val);

	name_len = strlen(ctx_.name);

	xcb_change_property(ctx_.dpy, XCB_PROP_MODE_REPLACE, ctx_.win,
	 XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, name_len, ctx_.name);

	xcb_change_property(ctx_.dpy, XCB_PROP_MODE_REPLACE, ctx_.win,
	 XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, name_len, ctx_.name);

	atom = getatom("_NET_WM_PID", sizeof("_NET_WM_PID") - 1);

	if (atom != XCB_ATOM_NONE) {
		ctx_.pid = getpid();
		xcb_change_property(ctx_.dpy, XCB_PROP_MODE_REPLACE, ctx_.win,
		 atom, XCB_ATOM_CARDINAL, 32, 1, &ctx_.pid);
		dd("pid %u win 0x%x\n", ctx_.pid, ctx_.win);
	}

	xcb_map_window(ctx_.dpy, ctx_.win);
	xcb_flush(ctx_.dpy);

	if (ctx_.text_h < ctx_.h)
		ctx_.text_y = (ctx_.h - ctx_.text_h) / 2;

	dd("win %#x wh (%u %u)\n", ctx_.win, ctx_.w, ctx_.h);
	return 0;
}

static void resize(xcb_resize_request_event_t *e)
{
	dd("win %#x wh (%u %u) --> (%u %u)\n", e->window, ctx_.w, ctx_.h,
	 e->width, e->height);

	if (e->height > ctx_.h) { /* have to re-create window to resize gc */
		ctx_.h = e->height;
		xcb_free_gc(ctx_.dpy, ctx_.gc);
		xcb_destroy_window(ctx_.dpy, ctx_.win);
		xcb_flush(ctx_.dpy);

		if (create_window() < 0) {
			ee("failed to create new window %ux%u\n", e->width, e->height);
			exit(1); /* consider it fatal */
		}
	}
}

static void handle_message(xcb_client_message_event_t *e)
{
	char *dat = (char *) e->data.data8;
	char *end = dat + sizeof(e->data.data8);
	char *ptr = dat;
	char *msg[3] = {0};
	uint8_t i = 0;

	dd("dat '%s' len %zu\n", (char *) e->data.data8, sizeof(e->data.data8));

	msg[i] = ptr;

	while (ptr < end) {
		if (*ptr == ' ') {
			*ptr = '\0';
			msg[++i] = ptr + 1;
		}

		ptr++;
	}

	if (msg[0] && icon_.str) {
		free(icon_.str);
		icon_.str = strdup(msg[0]);
		icon_.len = strlen(icon_.str);
		dd("ico '%s' len %u\n", icon_.str, icon_.len);
	}

	if (msg[1]) {
		ctx_.fg = strtol(msg[1], NULL, 16);
		dd("rgb '%s' len %zu\n", msg[1], strlen(msg[1]));
	}

	if (msg[2] && text_.str) {
		free(text_.str);
		text_.str = strdup(msg[2]);
		text_.len = strlen(text_.str);
		dd("txt '%s' len %u\n", text_.str, text_.len);
	}

	show();
}

static int events(void)
{
	uint8_t type;
	xcb_generic_event_t *e = xcb_wait_for_event(ctx_.dpy);

	if (!e)
		return 0;

	type = XCB_EVENT_RESPONSE_TYPE(e);

	switch (type) {
	case XCB_VISIBILITY_NOTIFY:
		switch (((xcb_visibility_notify_event_t *) e)->state) {
		case XCB_VISIBILITY_PARTIALLY_OBSCURED: /* fall through */
		case XCB_VISIBILITY_UNOBSCURED:
			dd("XCB_VISIBILITY_UNOBSCURED\n");
			show();
			break;
		}
		break;
	case XCB_EXPOSE:
		dd("XCB_EXPOSE\n");
		show();
		break;
	case XCB_KEY_PRESS:
		if (((xcb_key_press_event_t *)e)->detail == 9)
			ctx_.done = 1;
		break;
	case XCB_BUTTON_PRESS:
		if (ctx_.cmd)
			spawn(ctx_.cmd);
		break;
	case XCB_RESIZE_REQUEST:
		dd("XCB_RESIZE_REQUEST\n");
		resize((xcb_resize_request_event_t *) e);
		break;
	case XCB_CLIENT_MESSAGE:
		handle_message((xcb_client_message_event_t *) e);
		break;
	default:
		dd("got message type %d\n", type);
	}

	xcb_flush(ctx_.dpy);
	free(e);
	return 1;
}

static uint16_t get_space_width(fontid_t font_id)
{
	uint16_t w;
	uint16_t h;
	struct text *text = create_text();

	if (!text)
		return ctx_.font_size;

	set_text_font(text, font_id, ctx_.font_size);
	set_text_str(text, "=", sizeof("="));
	get_text_size(text, &w, &h);
	destroy_text(&text);
	return w;
}

static uint8_t init_text(struct text_info *text)
{
	uint16_t w;

	if (!(text->txt = create_text()))
		return 0;

	set_text_font(text->txt, text->font_id, ctx_.font_size);
	set_text_pos(text->txt, text->x, 0);
	set_text_color(text->txt, ctx_.fg, ctx_.bg);
	set_text_str(text->txt, text->str, text->len);
	get_text_size(text->txt, &w, &ctx_.h);
	ctx_.w += w;
	return 1;
}

static int opt(const char *arg, const char *args, const char *argl)
{
	return (strcmp(arg, args) == 0 || strcmp(arg, argl) == 0);
}

static void help(const char *prog)
{
	printf("Usage: %s <options>\n"
	 "\nOptions:\n"
	 "  -n, --name <str>          window name (%s)\n"
	 "  -i, --icon <glyph>        icon to display\n"
	 "  -t, --text <str>          text next to icon (%u chars)\n"
	 "  -c, --cmd <str>           command to run on mouse click\n"
	 "  -bg, --bgcolor <hex>      rgb color, default 0x%x\n"
	 "  -fg, --fgcolor <hex>      rgb color, default 0x%x\n"
	 "\nEnvironment:\n"
	 "  FWM_ICONS=%s\n"
	 "  FWM_FONT=%s\n"
	 "  FWM_FONT_SIZE=%f\n"
	 "  FWM_HDPI=%u\n"
	 "  FWM_VDPI=%u\n\n",
	 prog, ctx_.name, TEXT_MAXLEN, ctx_.bg, ctx_.fg, ctx_.icon_font,
	 ctx_.text_font, ctx_.font_size, ctx_.hdpi, ctx_.vdpi);
}

int main(int argc, char *argv[])
{
	const char *hdpi_str;
	const char *vdpi_str;
	const char *font_size_str;
	const char *arg;

	 /* init these defaults before checking args */

	ctx_.name = "dock";
	ctx_.fg = 0x808080;
	ctx_.bg = 0x202020;

	if ((hdpi_str = getenv("FWM_HDPI")))
		ctx_.hdpi = atoi(hdpi_str);
	if (ctx_.hdpi == 0)
		ctx_.hdpi = DEFAULT_DPI;

	if ((vdpi_str = getenv("FWM_VDPI")))
		ctx_.vdpi = atoi(vdpi_str);
	if (ctx_.vdpi == 0)
		ctx_.vdpi = DEFAULT_DPI;

	if ((font_size_str = getenv("FWM_FONT_SIZE")))
		ctx_.font_size = strtof(font_size_str, NULL);
	else
		ctx_.font_size = DEFAULT_FONT_SIZE;

	ctx_.icon_font = getenv("FWM_ICONS");
	ctx_.text_font = getenv("FWM_FONT");

	if (argc < 2) {
		help(argv[0]);
		return 0;
	}

	while (argc > 1) {
		arg = argv[--argc];
		if (opt(arg, "-i", "--icon")) {
			icon_.str = strdup(argv[argc + 1]);
			icon_.len = strlen(icon_.str);
		} else if (opt(arg, "-t", "--text")) {
			text_.str = strdup(argv[argc + 1]);
			text_.len = strlen(text_.str);
		} else if (opt(arg, "-c", "--cmd")) {
			ctx_.cmd = argv[argc + 1];
		} else if (opt(arg, "-bg", "--bgcolor")) {
			ctx_.bg = strtol(argv[argc + 1], NULL, 16);
		} else if (opt(arg, "-fg", "--fgcolor")) {
			ctx_.fg = strtol(argv[argc + 1], NULL, 16);
		} else if (opt(arg, "-n", "--name")) {
			ctx_.name = argv[argc + 1];
		}
	}

	if (!icon_.str && !text_.str) {
		ww("missing text or icon\n");
		text_.str = "error";
		text_.len = strlen(text_.str);
	}

	if (icon_.str) {
		icon_.font_id = open_font(ctx_.icon_font, ctx_.hdpi, ctx_.vdpi);
		if (invalid_font_id(icon_.font_id))
			return 1;
		else if (!init_text(&icon_))
			return 1;

		text_.x = ctx_.w;
	}

	if (text_.str) {
		text_.font_id = open_font(ctx_.text_font, ctx_.hdpi, ctx_.vdpi);
		if (invalid_font_id(text_.font_id))
			return 1;
		else if (!init_text(&text_))
			return 1;
		else if (icon_.str) {
			uint8_t space = get_space_width(text_.font_id);
			text_.x += space;
			ctx_.w += space;
		}
	}
	ctx_.text_h = ctx_.h;

	ctx_.dpy = xcb_connect(NULL, NULL);
	if (!ctx_.dpy) {
		ee("xcb_connect() failed\n");
		return 1;
	}

	ctx_.scr = xcb_setup_roots_iterator(xcb_get_setup(ctx_.dpy)).data;

	if (create_window() < 0)
		return 1;

	while (1)
		events();

	free(icon_.str);
	free(text_.str);
	destroy_text(&icon_.txt);
	destroy_text(&text_.txt);
	xcb_destroy_window(ctx_.dpy, ctx_.win);
	xcb_disconnect(ctx_.dpy);
	close_font(icon_.font_id);
	close_font(text_.font_id);
	return 0;
}
