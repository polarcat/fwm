/* yawm.c: yet another window manager
 *
 * Copyright (c) 2015, Aliaksei Katovich <aliaksei.katovich at gmail.com>
 *
 * Released under the GNU General Public License, version 2
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <fcntl.h>
#include <time.h>

#include <xcb/xcb.h>
#include <xcb/randr.h>
#include <xcb/xproto.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xcb_atom.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_util.h>

#include <X11/Xlib-xcb.h>
#include <X11/Xft/Xft.h>
#include <X11/keysym.h>

#include "list.h"

#define DEBUG
#define TRACE
#define TRACE_EVENTS

#define ee(fmt, ...) {\
	int errno_save__ = errno;\
	fprintf(stderr, "(ee) %s[%d]:%s: " fmt, __FILE__, __LINE__,\
		__func__, ##__VA_ARGS__);\
	if (errno_save__ != 0)\
		fprintf(stderr, "(ee) %s: %s, errno=%d\n", __func__,\
		     strerror(errno_save__), errno_save__);\
	errno = errno_save__;\
}

#ifdef DEBUG
#define dd(fmt, ...) printf("(dd) %s: " fmt, __func__, ##__VA_ARGS__)
#else
#define dd(fmt, ...) do {} while(0)
#endif

#define mm(fmt, ...) printf("(==) " fmt, ##__VA_ARGS__)
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

#define sslen(str) (sizeof(str) - 1)

/* defaults */

static char *basedir = "/tmp/yawm";

static unsigned int border_active = 0x5050ff;
static unsigned int border_normal = 0x005000;
static unsigned int panel_bg = 0x101010;
static unsigned int panel_height = 24; /* need to adjust with font height */

/* defines */

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define BORDER_WIDTH 1
#define WINDOW_PAD (BORDER_WIDTH * 2)

#define FONT_SIZE_FT 10.
#define FONT_NAME_FT "Monospace"
#define FONT_COLOR_NORMAL { 0x7000, 0x7000, 0x7000, 0xffff, }
#define FONT_COLOR_ACTIVE { 0x7800, 0x9900, 0x4c00, 0xffff, }

#define WIN_WIDTH_MIN 100
#define WIN_HEIGHT_MIN 100

#define PROP_MAX 1024

#define CLI_FLG_FIXED (1 << 0)
#define CLI_FLG_FLOAT (1 << 1)
#define CLI_FLG_URGENT (1 << 2)
#define CLI_FLG_NOFOCUS (1 << 3)
#define CLI_FLG_FULLSCR (1 << 4)

#define SCR_FLG_PANEL (1 << 0)
#define SCR_FLG_PANEL_TOP (1 << 1)

#define TAG_FLG_ACTIVE (1 << 0)

#define STATUS_BUF_MAX 255
#define TAGS_LINE_MAX 128
#define TITLE_LINE_MAX 128

#define TAG_NAME_MAX 32
#define BASE_PATH_MAX 255
#define TAGS_MAX 8 /* let's be sane here */

#define MOUSE_BTN_LEFT 1
#define MOUSE_BTN_MID 2
#define MOUSE_BTN_RIGHT 3

#define MENU_ICON "::"

#define TIME_STR_FMT "%Y-%m-%d/%V %H:%M"
#define TIME_STR_DEF "0000-00-00/00 00:00"
#define TIME_STR_MAX sizeof(TIME_STR_DEF)

#define MODKEY XCB_MOD_MASK_4
#define SHIFT XCB_MOD_MASK_SHIFT
#define CONTROL XCB_MOD_MASK_CONTROL

/* data structures */

struct screen {
	uint16_t idx;
	struct list_head head;
	struct list_head tags;

	struct tag *tag; /* current tag */
	xcb_screen_t *ptr;
	xcb_randr_output_t id;

	int16_t by; /* bar geometry */
	int16_t x, y, wx, wy, mx, my;
	uint16_t w, h;

	uint32_t flags; /* SCR_FLG */

	xcb_gcontext_t gc;
	xcb_drawable_t panel;

	char menustr[16];

	/* text drawing related stuff */
	XftDraw *draw;
	xcb_visualtype_t *visual;
};

#define list2screen(item) list_entry(item, struct screen, head)

static struct screen *screen;
static struct list_head screens;

struct tag {
	struct list_head head;
	struct list_head clients;

	xcb_window_t win; /* active window */
	struct screen *scr;

	xcb_gcontext_t gc;
	int16_t x;
	uint16_t w;
	int flags;

	char *name; /* visible name */
	int nlen; /* name length */
};

#define list2tag(item) list_entry(item, struct tag, head)

struct client {
	struct list_head head;
	struct tag *tag;
	int16_t x, y;
	uint16_t w, h;
	int hidden;
	struct screen *scr;
	xcb_window_t win;
};

#define list2client(item) list_entry(item, struct client, head)

struct winhash {
	uint32_t hash;
	struct client *cli;
};

struct arg {
	const void *ptr;
};

struct keymap {
	uint16_t mod;
	xcb_keysym_t sym;
	xcb_keycode_t key;
	void (*action)(void *);
	void *arg;
};

/* config */

enum winpos {
	WIN_POS_FILL,
	WIN_POS_TOP_LEFT,
	WIN_POS_TOP_RIGHT,
	WIN_POS_BOTTOM_LEFT,
	WIN_POS_BOTTOM_RIGHT,
	WIN_POS_LEFT_FILL,
	WIN_POS_RIGHT_FILL,
	WIN_POS_TOP_FILL,
	WIN_POS_BOTTOM_FILL,
};

static void next_window(void *);
static void prev_window(void *);
static void raise_window(void *);
static void place_window(void *);

static void spawn(const char **argv);

static char *term[] = { "xterm", NULL };
static char *lock[] = { "xscreensaver-command", "--lock", NULL };

static struct keymap kmap[] = {
	{ MODKEY, XK_Tab, 0, next_window, NULL, },
	{ MODKEY, XK_BackSpace, 0, prev_window, NULL, },
	{ MODKEY, XK_Return, 0, raise_window, NULL, },
	{ MODKEY, XK_t, 0, spawn, (void *) term, },
	{ SHIFT, XK_F5, 0, place_window, (void *) WIN_POS_TOP_LEFT, },
	{ SHIFT, XK_F6, 0, place_window, (void *) WIN_POS_TOP_RIGHT, },
	{ SHIFT, XK_F7, 0, place_window, (void *) WIN_POS_BOTTOM_LEFT, },
	{ SHIFT, XK_F8, 0, place_window, (void *) WIN_POS_BOTTOM_RIGHT, },
	{ MODKEY, XK_F5, 0, place_window, (void *) WIN_POS_LEFT_FILL, },
	{ MODKEY, XK_F6, 0, place_window, (void *) WIN_POS_RIGHT_FILL, },
	{ MODKEY, XK_F7, 0, place_window, (void *) WIN_POS_TOP_FILL, },
	{ MODKEY, XK_F8, 0, place_window, (void *) WIN_POS_BOTTOM_FILL, },
	{ MODKEY, XK_F9, 0, place_window, (void *) WIN_POS_FILL, },
	{ 0, 0, 0, NULL, NULL, },
};

/* globals */

static int16_t mouse_x, mouse_y;
static int mouse_button; /* current mouse button */

static XftColor normal_color;
static XftColor active_color;
static XftFont *font;
static XftDraw *normal_font;
static XftDraw *active_font;

static int xscr;
static Display *xdpy;
static xcb_connection_t *dpy;

static xcb_atom_t a_name;
static xcb_atom_t a_desktop;
static xcb_atom_t a_client_list;

static time_t prev_time;

/* <panel stuff> */

enum panel_area { /* in order of appearance */
	PANEL_AREA_TAGS,
	PANEL_AREA_MENU,
	PANEL_AREA_TITLE,
	PANEL_AREA_DOCK,
	PANEL_AREA_TIME,
	PANEL_AREA_MAX,
};

struct panel_item {
	int16_t x;
	uint16_t w;
	const char *name; /* FIXME: temporary */
};

static struct panel_item panel_items[PANEL_AREA_MAX];
static uint16_t panel_vpad;
static uint16_t title_max;
/* </panel stuff> */

/* ... and the mess begins */

static const char *utf8chr(const char *str, char c, int *len, int *bytes)
{
	int i = 0, ii = 0, n = 0;

	while (str[i] > 0) {
ascii:
		if (str[i] == c)
			return &str[i];
		i++;
	}

	n += i - ii;
	while (str[i]) {
		if (str[i] > 0) {
			ii = i;
			goto ascii;
		} else {
			switch (0xf0 & str[i]) {
			case 0xe0:
				i += 3;
				break;
			case 0xf0:
				i += 4;
				break;
			default:
				i += 2;
				break;
			}
		}
		n++;
	}

	if (bytes)
		*bytes = i;
	if (len)
		*len = n;

	return NULL;
}

static void text_exts(const char *text, int len, uint16_t *w, uint16_t *h)
{
	XGlyphInfo ext;

	XftTextExtentsUtf8(xdpy, font, (XftChar8 *) text, len, &ext);
#ifdef VERBOSE
	ii("text: %s\n"
	   "  x = %d\n"
	   "  y = %d\n"
	   "  width = %d\n"
	   "  height = %d\n"
	   "  xOff = %d\n"
	   "  yOff = %d\n",
	   text,
	   ext.x,
	   ext.y,
	   ext.width,
	   ext.height,
	   ext.xOff,
	   ext.yOff);
#endif
	*w = ext.width;
	*h = ext.height;
}

static void fill_rect(xcb_window_t win, xcb_gcontext_t gc,
		      int16_t x, int16_t y, uint16_t w, uint16_t h)
{
	xcb_rectangle_t rect = { x, y, w, h, };
	xcb_poly_fill_rectangle(dpy, win, gc, 1, &rect);
}

static void draw_panel_text(struct screen *scr, XftColor *color, int16_t x,
			    uint16_t w, const char *text, int len, int flush)
{
	fill_rect(scr->panel, scr->gc, x, 0, w, panel_height);
	XftDrawStringUtf8(scr->draw, color, font, x, panel_vpad,
			  (XftChar8 *) text, len);

	if (flush) {
		/* this makes text displayed, do not ask.. */
		xcb_set_input_focus(dpy, XCB_NONE, XCB_INPUT_FOCUS_POINTER_ROOT,
				    XCB_CURRENT_TIME);
		xcb_flush(dpy);
	}
}

static void spawn(const char **argv)
{
	if (fork() == 0) {
		if (dpy)
			close(xcb_get_file_descriptor(dpy));
		if (setsid() < 0) {
			tt("setsid() failed\n");
			exit(1);
		}
		tt("execvp %s\n", argv[0]);
		execvp(argv[0], argv);
		exit(0);
	}
}

static void clean(void)
{
	xcb_disconnect(dpy);

	if (font)
		XftFontClose(xdpy, font);
}

#define panic(fmt, ...) {\
	int errno_save__ = errno;\
	fprintf(stderr, "(ee) %s[%d]:%s: " fmt, __FILE__, __LINE__,\
		__func__, ##__VA_ARGS__);\
	if (errno_save__ != 0)\
		fprintf(stderr, "(ee) %s: %s, errno=%d\n", __func__,\
		     strerror(errno_save__), errno_save__);\
	clean();\
	exit(errno_save__);\
}

#define xcb_eval(cookie, func) {\
	cookie = func;\
	xcb_generic_error_t *error__ = xcb_request_check(dpy, cookie);\
        if (error__)\
		panic("%s, err=%d\n", #func, error__->error_code);\
}

#define PROP_TYPE_ANY XCB_GET_PROPERTY_TYPE_ANY

static xcb_get_property_reply_t *get_prop_any(xcb_window_t w, xcb_atom_t a)
{
	xcb_get_property_cookie_t c;

	c = xcb_get_property(dpy, 0, w, a, PROP_TYPE_ANY, 0, PROP_MAX);
	return xcb_get_property_reply(dpy, c, NULL);
}

#ifndef VERBOSE
#define panel_items_stat(void) ;
#else
static void panel_items_stat(void)
{
	int i;

	for (i = 0; i < PANEL_AREA_MAX; i++) {
		switch (i) {
		case PANEL_AREA_TAGS:
			panel_items[i].name = "PANEL_AREA_TAGS";
			break;
		case PANEL_AREA_MENU:
			panel_items[i].name = "PANEL_AREA_MENU";
			break;
		case PANEL_AREA_TITLE:
			panel_items[i].name = "PANEL_AREA_TITLE";
			break;
		case PANEL_AREA_DOCK:
			panel_items[i].name = "PANEL_AREA_DOCK";
			break;
		case PANEL_AREA_TIME:
			panel_items[i].name = "PANEL_AREA_TIME";
			break;
		}

		mm("%s: %d,%d (%d)\n", panel_items[i].name, panel_items[i].x,
		   panel_items[i].x + panel_items[i].w,
		   panel_items[i].w);
	}
}
#endif

static void print_title(xcb_window_t win)
{
	uint16_t len;
	uint16_t w, h;
	xcb_get_property_cookie_t c;
	xcb_icccm_get_text_property_reply_t info;

	c = xcb_icccm_get_wm_name(dpy, win);
	if (!xcb_icccm_get_wm_name_reply(dpy, c, &info, NULL)) {
		ww("xcb_icccm_get_wm_name_reply() failed\n");
		return;
	}

	if (!info.name)
		return;

	panel_items_stat();

	if (info.name_len <= title_max) {
		len = info.name_len;
	} else {
		len = title_max;
		info.name[len - 2] = '.';
		info.name[len - 1] = '.';
		info.name[len] = '.';
	}

	/* FIXME: need to match window to screen */
#if 0
	mm("menu %d, range %d,%d (%d), name %s\n",
	   panel_items[PANEL_AREA_MENU].x + panel_items[PANEL_AREA_MENU].w,
	   panel_items[PANEL_AREA_TITLE].x,
	   panel_items[PANEL_AREA_TITLE].w,
	   panel_items[PANEL_AREA_TITLE].x + panel_items[PANEL_AREA_TITLE].w,
	   info.name);
#endif
	draw_panel_text(screen, &active_color, panel_items[PANEL_AREA_TITLE].x,
			panel_items[PANEL_AREA_TITLE].w,
			(XftChar8 *) info.name, len, 1);
}

static void update_clients_list(void)
{
	struct list_head *ccur, *tcur;

	tt("NET_CLIENT_LIST %d\n", a_client_list);

	xcb_delete_property(dpy, screen->ptr->root, a_client_list);
	list_walk(tcur, &screen->tags) {
		struct tag *tag = list2tag(tcur);
		list_walk(ccur, &tag->clients) {
			struct client *cli = list2client(ccur);
			dd("append client %p, window %p\n", cli, cli->win);
			xcb_change_property(dpy, XCB_PROP_MODE_APPEND,
					    screen->ptr->root, a_client_list,
					    XCB_ATOM_WINDOW, 32, 1, &cli->win);
		}
	}
	xcb_flush(dpy);
}

static void match_client_class(struct client *cli, const char *class,
			       const char *instance)
{
	xcb_get_property_cookie_t c;
	xcb_icccm_get_wm_class_reply_t *r;
	xcb_icccm_get_wm_class_reply_t info;

	c = xcb_icccm_get_wm_class(dpy, cli->win);
	if (!xcb_icccm_get_wm_class_reply(dpy, c, &info, NULL)) {
		ww("xcb_icccm_get_wm_class_reply() failed\n");
		return;
	}

	mm("class %s, instance %s\n", info.class_name, info.instance_name);
}

static struct screen *root2screen(xcb_window_t root)
{
	struct list_head *cur;

	list_walk(cur, &screens) {
		struct screen *scr = list2screen(cur);
		if (scr->ptr->root == root)
			return scr;
	}
	return NULL;
}

static struct client *win2client(struct screen *scr, xcb_window_t win)
{
	struct list_head *cur;

	list_walk(cur, &scr->tag->clients) {
		struct client *cli = list2client(cur);
		if (cli->win == win)
			return cli;
	}
	return NULL;
}

static void *window_exists(xcb_window_t win)
{
	xcb_get_window_attributes_cookie_t c;

	c = xcb_get_window_attributes(dpy, win);
	return (void *) xcb_get_window_attributes_reply(dpy, c, NULL);
}

static void window_lower(xcb_window_t win)
{
	uint32_t val[1] = { XCB_STACK_MODE_BELOW, };
	xcb_configure_window(dpy, win, XCB_CONFIG_WINDOW_STACK_MODE, val);
}

static void window_raise(xcb_window_t win)
{
	uint32_t val[1] = { XCB_STACK_MODE_ABOVE, };
	xcb_configure_window(dpy, win, XCB_CONFIG_WINDOW_STACK_MODE, val);
}

static void window_focus(struct screen *scr, xcb_window_t win, int focus)
{
	uint32_t val[1];
	struct client *cli;

	tt("win %p, focus %d\n", win, focus);

	if (!window_exists(win))
		return;

	if (!focus) {
		val[0] = border_normal;
	} else {
		val[0] = border_active;
		print_title(win);
		scr->tag->win = win;
	}

	xcb_change_window_attributes(dpy, win, XCB_CW_BORDER_PIXEL, val);
        xcb_set_input_focus(dpy, XCB_NONE, win, XCB_CURRENT_TIME);
}

static void switch_window(xcb_key_press_event_t *e, int next)
{
	struct screen *scr;
	struct client *cli;

	scr = root2screen(e->root);
	cli = win2client(scr, scr->tag->win);

	if (next) {
		if (cli->head.next == &scr->tag->clients) /* end of list */
			cli = list2client(scr->tag->clients.next);
		else
			cli = list2client(cli->head.next);
	} else {
		if (cli->head.prev == &scr->tag->clients) /* head of list */
			cli = list2client(scr->tag->clients.prev);
		else
			cli = list2client(cli->head.prev);
	}

	window_raise(cli->win);
	window_focus(scr, scr->tag->win, 0);
	window_focus(scr, cli->win, 1);
	xcb_flush(dpy);
}

static void print_tag(struct screen *scr, struct tag *tag)
{
	XftColor *color;
	struct list_head *cur;
	struct tag *prev;

#if 0
	if (scr->tag) { /* deselect current instantly */
		scr->tag->flags &= ~TAG_FLG_ACTIVE;
		color = &normal_color;
		tag_refresh(scr, scr->tag, color);
	}

	prev = tag;

	list_walk(cur, &scr->tags) {
		struct tag *tag = list2tag(cur);

		if (tag == scr->tag) {
			continue;
		} else if (!tag_clicked(tag, x)) {
			tag->flags &= ~TAG_FLG_ACTIVE;
			color = &normal_color;
			tag_refresh(scr, tag, color);
		} else {
			tag->flags |= TAG_FLG_ACTIVE;
			color = &active_color;
			scr->tag = tag;
			tag_refresh(scr, tag, color);
			break;
		}
	}

	if (scr->tag && scr->tag != prev) {
		if (prev) {
			/* hide windows on previous tag */
			list_walk(cur, &prev->clients) {
				struct client *cli = list2client(cur);
				xcb_unmap_window(dpy, cli->win);
			}
		}

		/* show windows on current tag */
		list_walk(cur, &scr->tag->clients) {
			struct client *cli = list2client(cur);
			xcb_map_window(dpy, cli->win);
		}
	}

        xcb_set_input_focus(dpy, XCB_NONE, XCB_INPUT_FOCUS_POINTER_ROOT,
                            XCB_CURRENT_TIME);
	xcb_flush(dpy);
#endif
}

static void switch_tag(xcb_key_press_event_t *e, int next)
{
	struct screen *scr;
	struct tag *tag;

	scr = root2screen(e->root);
	tag = scr->tag;

	if (next) {
		if (tag->head.next == &scr->tags) /* end of list */
			tag = list2tag(scr->tags.next);
		else
			tag = list2tag(tag->head.next);
	} else {
		if (tag->head.prev == &scr->tags) /* head of list */
			tag = list2tag(scr->tags.prev);
		else
			tag = list2tag(tag->head.prev);
	}

	scr->tag = tag;
}

static void client_moveresize(struct client *cli, int x, int y, int w, int h)
{
	uint32_t val[4], mask;

	cli->x = x;
	cli->y = y;
	cli->w = w;
	cli->h = h;

	/* fit into monitor space */
	if (cli->w > cli->scr->w)
		cli->w = cli->scr->w - BORDER_WIDTH;
	else if (cli->w < WIN_WIDTH_MIN)
		cli->w = cli->scr->w / 2 - BORDER_WIDTH;
	if (cli->h > cli->scr->h)
		cli->h = cli->scr->h - BORDER_WIDTH;
	else if (cli->h < WIN_HEIGHT_MIN)
		cli->h = cli->scr->h / 2 - BORDER_WIDTH;

	if (cli->h + cli->y >= cli->scr->h)
		cli->h = cli->scr->h - cli->y - WINDOW_PAD;

	if (cli->x > cli->scr->w)
		cli->x = 0;
	if (cli->y > cli->scr->h)
		cli->y = 0;

	if (screen->flags & SCR_FLG_PANEL_TOP)
		cli->y += panel_height;

	val[0] = cli->x;
	val[1] = cli->y;
	val[2] = cli->w;
	val[3] = cli->h;
	mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
	mask |= XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
	xcb_configure_window(dpy, cli->win, mask, val);

	dd("cli %p, win %p, geo %dx%d+%d+%d\n", cli, cli->win,
	   cli->w, cli->h, cli->x, cli->y);
}

static void place_window(void *arg)
{
	enum winpos pos = (enum winpos) arg;
	int16_t x, y;
	uint16_t w, h;
	struct client *cli;

	cli = win2client(screen, screen->tag->win);
	if (!cli)
		return;

	switch (pos) {
	case WIN_POS_FILL:
		dd("WIN_POS_FILL\n");
		x = y = 0;
		w = screen->w - WINDOW_PAD;
		h = screen->h - BORDER_WIDTH;
		break;
	case WIN_POS_LEFT_FILL:
		dd("WIN_POS_LEFT_FILL\n");
		x = y = 0;
		goto halfw;
	case WIN_POS_RIGHT_FILL:
		dd("WIN_POS_RIGHT_FILL\n");
		x = screen->w / 2;
		y = 0;
		goto halfw;
	case WIN_POS_TOP_FILL:
		dd("WIN_POS_TOP_FILL\n");
		x = y = 0;
		goto halfh;
	case WIN_POS_BOTTOM_FILL:
		dd("WIN_POS_BOTTOM_FILL\n");
		x = 0;
		y = screen->h / 2;
		goto halfh;
	case WIN_POS_TOP_LEFT:
		dd("WIN_POS_TOP_LEFT\n");
		x = y = 0;
		goto halfwh;
	case WIN_POS_TOP_RIGHT:
		dd("WIN_POS_TOP_RIGHT\n");
		x = screen->w / 2;
		y = 0;
		goto halfwh;
	case WIN_POS_BOTTOM_LEFT:
		dd("WIN_POS_BOTTOM_LEFT\n");
		x = 0;
		y = screen->h / 2;
		goto halfwh;
	case WIN_POS_BOTTOM_RIGHT:
		dd("WIN_POS_BOTTOM_RIGHT\n");
		x = screen->w / 2;
		y = screen->h / 2;
		goto halfwh;
	default:
		return;
	}

out:
	client_moveresize(cli, x, y, w, h);
	xcb_flush(dpy);
	return;
halfwh:
	w = screen->w / 2 - WINDOW_PAD;
	h = screen->h / 2 - WINDOW_PAD;
	goto out;
halfw:
	w = screen->w / 2 - WINDOW_PAD;
	h = screen->h - WINDOW_PAD;
	goto out;
halfh:
	w = screen->w - WINDOW_PAD;
	h = screen->h / 2 - WINDOW_PAD;
	goto out;
}

static void hide_window(struct screen *scr, xcb_window_t win)
{
	uint32_t val[1] = { scr->h, };
	uint32_t mask = XCB_CONFIG_WINDOW_Y;

	xcb_configure_window(dpy, win, mask, val);
}

static void next_window(void *arg)
{
	switch_window((xcb_key_press_event_t *) arg, 1);
}

static void prev_window(void *arg)
{
	switch_window((xcb_key_press_event_t *) arg, 0);
}

static void raise_window(void *arg)
{
	xcb_key_press_event_t *e = (xcb_key_press_event_t *) arg;
	struct screen *scr;
	struct client *cli;

	scr = root2screen(e->root);
	cli = win2client(scr, scr->tag->win);

	tt("cur win %p\n", cli->win);
	/* FIXME: prev window must be unfocused */
	window_raise(cli->win);
	xcb_flush(dpy);
}

static void panel_raise(xcb_window_t root)
{
	struct screen *scr;

	scr = root2screen(root);
	if (scr->panel)
		window_raise(scr->panel);
}

static void next_tag(void *arg)
{

}

static void prev_tag(void *arg)
{

}

#if 0
static void client_resize(struct client *cli, int w, int h)
{
	int val[2], mask;

	cli->w = w;
	cli->h = h;

	/* fit into monitor space */
	if (cli->w > cli->scr->w)
		cli->w = cli->scr->w;
	if (cli->h > cli->scr->h)
		cli->h = cli->scr->h;

	mask = XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
	val[0] = cli->w;
	val[1] = cli->h;
	xcb_configure_window(dpy, cli->win, mask, val);
	xcb_flush(dpy);

	dd("cli %p geo %dx%d+%d+%d, name %s\n", cli, cli->w, cli->h,
	   cli->x, cli->y, cli->name);
}
#endif

static void client_add(xcb_window_t win)
{
	struct tag *tag;
	struct client *cli;
	uint32_t val[2], mask;
	int dy, dh;
	xcb_get_property_cookie_t c;
	xcb_get_geometry_reply_t *g;
	xcb_get_window_attributes_cookie_t ac;
	xcb_get_window_attributes_reply_t *a;

	tt("win %p\n", win);

	if (win == screen->ptr->root)
		return;

#if 0
	if (!cli) {
		cli = win2client(win);
		if (!cli) {
			ee("window %p is not managed\n", win);
			return;
		}
	}
#endif

	ac = xcb_get_window_attributes(dpy, win);
	a = xcb_get_window_attributes_reply(dpy, ac, NULL);
	if (!a) {
		ee("xcb_get_window_attributes() failed\n");
		return;
	}

	g = NULL;
	if (a->override_redirect) {
		ww("ignore window %p");
		goto out;
	}

	/* tell x server to restore window upon our sudden exit */
	xcb_void_cookie_t cc;
	xcb_eval(cc, xcb_change_save_set(dpy, XCB_SET_MODE_INSERT, win));

	val[0] = BORDER_WIDTH;
	mask = XCB_CONFIG_WINDOW_BORDER_WIDTH;

	/* subscribe events */
	val[1] = XCB_EVENT_MASK_ENTER_WINDOW |
	      XCB_EVENT_MASK_LEAVE_WINDOW |
	      XCB_EVENT_MASK_PROPERTY_CHANGE;
	mask |= XCB_CW_EVENT_MASK;
	xcb_change_window_attributes(dpy, win, mask, val);

	/* get initial geometry */
	g = xcb_get_geometry_reply(dpy, xcb_get_geometry(dpy, win), NULL);
	if (!g) {
		ee("xcb_get_geometry() failed\n");
		goto out;
	}

	cli = calloc(1, sizeof(*cli));
	if (!cli) {
		ee("calloc(%lu) failed\n", sizeof(*cli));
		return;
	}
	cli->scr = screen;
	cli->win = win;

	list_add(&screen->tag->clients, &cli->head);

	client_moveresize(cli, g->x, g->y, g->width, g->height);

	window_raise(cli->win);

	xcb_map_window(dpy, cli->win);
	xcb_flush(dpy);

	dd("cli %p, win %p, geo %dx%d+%d+%d\n", cli, cli->win,
	   cli->w, cli->h, cli->x, cli->y);

	update_clients_list();
out:
	free(a);
	free(g);
}

static void client_del(xcb_window_t root, xcb_window_t win)
{
	struct client *cli;
	struct screen *scr;

	scr = root2screen(root);
	cli = win2client(scr, win);
	if (!cli) {
		ww("window %p was not managed\n", win);
		return;
	}

	list_del(&cli->head);
	free(cli);
}

static void clients_scan(void)
{
	int i, n;
	xcb_query_tree_cookie_t c;
	xcb_query_tree_reply_t *tree;
	xcb_window_t *wins;

	/* walk through windows tree */
	c = xcb_query_tree(dpy, screen->ptr->root);
	tree = xcb_query_tree_reply(dpy, c, 0);
	if (!tree)
		panic("xcb_query_tree_reply() failed\n");

	n = xcb_query_tree_children_length(tree);
	wins = xcb_query_tree_children(tree);

	/* map clients onto the current screen */
	mm("%d clients found\n", n);
	for (i = 0; i < n; i++) {
		mm("++ handle win %p\n", wins[i]);
		if (wins[i] == screen->panel)
			continue;
		client_add(wins[i]);
	}

	if (!list_empty(&screen->tag->clients))
		window_focus(screen, wins[i], 1);

	free(tree);
	update_clients_list();
}

static void print_menu(struct screen *scr)
{
	uint16_t x, w;

	x = panel_items[PANEL_AREA_MENU].x;
	w = panel_items[PANEL_AREA_MENU].w;

	draw_panel_text(scr, &normal_color, x, w, (XftChar8 *) MENU_ICON,
		        sizeof(MENU_ICON) - 1, 0);
}

static void print_time(struct screen *scr)
{
	time_t t;
	struct tm *tm;
	char str[TIME_STR_MAX];
	int16_t x;
	uint16_t w;

	t = time(NULL);
	if (t == prev_time)
		return;

	tm = localtime(&t);
	if (!tm)
		return;

	if (strftime(str, TIME_STR_MAX, TIME_STR_FMT, tm) == 0)
		strncpy(str, TIME_STR_DEF, TIME_STR_MAX);

	x = panel_items[PANEL_AREA_TIME].x;
	w = panel_items[PANEL_AREA_TIME].w;

	draw_panel_text(scr, &normal_color, x, w, (XftChar8 *) str,
			sizeof(str) - 1, 1);
	prev_time = t;
}

static void tag_refresh(struct screen *scr, struct tag *tag, XftColor *color)
{
	draw_panel_text(scr, color, tag->x, tag->w, (XftChar8 *) tag->name,
		        tag->nlen, 0);
}

static int tag_clicked(struct tag *tag, int16_t x)
{
	if (x >= tag->x && x <= tag->x + tag->w)
		return 1;
	return 0;
}

static void tag_select(struct screen *scr, int x)
{
	XftColor *color;
	struct list_head *cur;
	struct tag *prev;

	if (scr->tag && tag_clicked(scr->tag, x)) {
		return;
	} else if (scr->tag) { /* deselect current instantly */
		scr->tag->flags &= ~TAG_FLG_ACTIVE;
		color = &normal_color;
		tag_refresh(scr, scr->tag, color);
	}

	prev = scr->tag;

	list_walk(cur, &scr->tags) {
		struct tag *tag = list2tag(cur);

		if (tag == scr->tag) {
			continue;
		} else if (!tag_clicked(tag, x)) {
			tag->flags &= ~TAG_FLG_ACTIVE;
			color = &normal_color;
			tag_refresh(scr, tag, color);
		} else {
			tag->flags |= TAG_FLG_ACTIVE;
			color = &active_color;
			scr->tag = tag;
			tag_refresh(scr, tag, color);
			break;
		}
	}

	if (scr->tag && scr->tag != prev) {
		if (prev) {
			/* hide windows on previous tag */
			list_walk(cur, &prev->clients) {
				struct client *cli = list2client(cur);
				xcb_unmap_window(dpy, cli->win);
			}
		}

		/* show windows on current tag */
		list_walk(cur, &scr->tag->clients) {
			struct client *cli = list2client(cur);
			xcb_map_window(dpy, cli->win);
		}
	}

        xcb_set_input_focus(dpy, XCB_NONE, XCB_INPUT_FOCUS_POINTER_ROOT,
                            XCB_CURRENT_TIME);
	xcb_flush(dpy);
}

static int tag_add(struct screen *scr, const char *name, int pos)
{
	XftColor *color;
	struct tag *tag;
	uint16_t h;

	tag = calloc(1, sizeof(*tag));
	if (!tag) {
		ee("calloc() failed\n");
		return;
	}

	tag->nlen = strlen(name);
	tag->name = calloc(1, tag->nlen);
	if (!tag->name) {
		ee("calloc(%d) failed\n", tag->nlen);
		free(tag);
		return;
	}

	memcpy(tag->name, name, tag->nlen);
	list_init(&tag->clients);
	list_add(&scr->tags, &tag->head);

	text_exts(name, tag->nlen, &tag->w, &h);

	if (pos != panel_items[PANEL_AREA_TAGS].x) {
		color = &normal_color;
		tag->flags &= ~TAG_FLG_ACTIVE;
	} else {
		color = &active_color;
		tag->flags |= TAG_FLG_ACTIVE;
		scr->tag = tag;
	}

	draw_panel_text(scr, color, pos, tag->w, (XftChar8 *) tag->name,
			tag->nlen, 0);

	tag->x = pos;
	tag->w += FONT_SIZE_FT; /* spacing */
	return pos + tag->w;
}

/*
 * Tags dir structure:
 *
 * /<basedir>/<screennumber>/<tagnumber>
 *
 * where each <tagnumber> file contains tag name
 *
 * Example:
 *
 * $HOME/.yawm/twomonitors/
 *			 0/
 *			   0 --> "main"
 *			   1 --> "work"
 *			   2 --> "compile"
 *			   3 --> "chat"
 *			 1/
 *			   0 --> "main"
 *			   1 --> "work"
 *			   2 --> "browser"
 */
static int init_tags(struct screen *scr)
{
	struct list_head *cur;
	int fd, i, pos;
	char path[BASE_PATH_MAX], name[TAG_NAME_MAX + 2], *ptr;

	pos = panel_items[PANEL_AREA_TAGS].x;
	if (!basedir) {
		ww("no user defined tags\n");
		goto out;
	}

	snprintf(path, sizeof(path), "%s/%d/", basedir, scr->idx);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		ww("no user defined tags for screen %d\n", scr->idx);
		goto out;
	}
	close(fd);
	ptr = path + strlen(path);

	memset(name, 0, sizeof(name));
	for (i = 0; i < TAGS_MAX; i++) {
		sprintf(ptr, "%d", i);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		read(fd, name, TAG_NAME_MAX);
		mm("scr%d, tag%d: %s\n", scr->idx, i, name);
		close(fd);
		pos = tag_add(scr, name, pos);
		memset(name, 0, TAG_NAME_MAX);
	}

out:
	if (pos == panel_items[PANEL_AREA_TAGS].x) /* add default tag */
		pos = tag_add(scr, "*", pos);

	return pos - panel_vpad / 2;
}

static int calc_title_max(void)
{
	int16_t w, h, i;
	char *tmp;

	i = 1;
	w = 0;
	while (w < panel_items[PANEL_AREA_TITLE].w) {
		tmp = calloc(1, i + 1);
		memset(tmp, 'w', i);
		text_exts(tmp, strlen(tmp), &w, &h);
		free(tmp);
		i++;
	}
	title_max = i - 2;
	mm("title_max=%u\n", title_max);
}

static int update_panel_items(struct screen *scr)
{
	int16_t x;
	uint16_t h, w;

	text_exts(TIME_STR_DEF, TIME_STR_MAX, &w, &h);
	panel_items[PANEL_AREA_TIME].x = scr->w - w;
	panel_items[PANEL_AREA_TIME].w = w;

	panel_vpad = panel_height - (panel_height - FONT_SIZE_FT) / 2;

	x = FONT_SIZE_FT;
	panel_items[PANEL_AREA_TAGS].x = x;
	panel_items[PANEL_AREA_TAGS].w = init_tags(scr);
	x += panel_items[PANEL_AREA_TAGS].w + 1;

	text_exts(MENU_ICON, sizeof(MENU_ICON) - 1, &w, &h);
	panel_items[PANEL_AREA_MENU].x = x;
	panel_items[PANEL_AREA_MENU].w = w + panel_vpad;
	x += panel_items[PANEL_AREA_MENU].w + 1;

	w = panel_items[PANEL_AREA_TIME].x - 1 - x - panel_vpad;
	panel_items[PANEL_AREA_TITLE].w = w;
	panel_items[PANEL_AREA_TITLE].x = x;
	calc_title_max();

	print_menu(scr);
	print_time(scr); /* last element on panel */

	panel_items_stat();
}

static void init_panel(struct screen *scr)
{
	int y;
	uint32_t val[2], mask;

	scr->panel = xcb_generate_id(dpy);
	dd("panel window %x\n", scr->panel);

	mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	val[0] = panel_bg;
	val[1] = XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_VISIBILITY_CHANGE;

	if (scr->flags & SCR_FLG_PANEL_TOP)
		y = 0;
	else
		y = scr->h - panel_height;

	xcb_create_window(dpy, XCB_COPY_FROM_PARENT, scr->panel, scr->ptr->root,
			  0, y, scr->w, panel_height, 0,
			  XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  scr->ptr->root_visual, mask, val);

        xcb_change_property(dpy, XCB_PROP_MODE_REPLACE, scr->panel,
			    XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                            sizeof("yawmpanel") - 1, "yawmpanel");

        xcb_change_property(dpy, XCB_PROP_MODE_REPLACE, scr->panel,
			    XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
                            sizeof("yawmpanel") - 1, "yawmpanel");

	scr->gc = xcb_generate_id(dpy);
	mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND;
	val[0] = val[1] = panel_bg;
        xcb_create_gc(dpy, scr->gc, scr->panel, mask, val);

	xcb_map_window(dpy, scr->panel);

	/* now correct screen height */
	scr->h -= panel_height;

	scr->draw = XftDrawCreate(xdpy, scr->panel,
				  DefaultVisual(xdpy, xscr),
				  DefaultColormap(xdpy, xscr));

	update_panel_items(scr);
}

#define pointer_inside(area, ex)\
	(ex >= panel_items[area].x &&\
	 ex <= panel_items[area].x + panel_items[area].w)

#ifndef VERBOSE
#define dump_coords(x) ;
#else
static void dump_coords(int x)
{
	int i;

	for (i = 0; i < PANEL_AREA_MAX; i++) {
		mm("%d: %d >= %d <= %d (w = %d)\n", i, panel_items[i].x, x,
		   panel_items[i].w + panel_items[i].x, panel_items[i].w);
		if pointer_inside(i, x)
			mm("^^^ inside %s\n", panel_items[i].name);
	}
}
#endif

static void motion_clean(void)
{
	mouse_x = mouse_y = mouse_button = 0;
	xcb_ungrab_pointer(dpy, XCB_CURRENT_TIME);
	xcb_flush(dpy);
}

static void handle_motion_notify(xcb_motion_notify_event_t *e)
{
	uint32_t mask;
	uint32_t val[2];
	int16_t dx, dy;
	struct client *cli;
	struct screen *scr;

	if (!e->child) {
		motion_clean();
		return;
	}

	scr = root2screen(e->root);

	if (e->child == scr->panel)
		return;

	cli = win2client(scr, e->child);

	dx = e->root_x - mouse_x;
	dy = e->root_y - mouse_y;

	mouse_x = e->root_x;
	mouse_y = e->root_y;

	cli->x += dx;
	cli->y += dy;

	mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
	val[0] = cli->x;
	val[1] = cli->y;
	xcb_configure_window(dpy, cli->win, mask, val);
	xcb_flush(dpy);
}

static void handle_button_press(xcb_button_press_event_t *e)
{
	switch (e->detail) {
	case MOUSE_BTN_LEFT:
		dd("MOUSE_BTN_LEFT\n");
		if (screen->panel == e->event) {
			dump_coords(e->event_x);
			if pointer_inside(PANEL_AREA_TAGS, e->event_x)
				tag_select(screen, e->event_x);
			else if pointer_inside(PANEL_AREA_MENU, e->event_x)
				mm("menu\n");
			else if pointer_inside(PANEL_AREA_TITLE, e->event_x)
				mm("title\n");
			else if pointer_inside(PANEL_AREA_DOCK, e->event_x)
				mm("dock\n");
			else if pointer_inside(PANEL_AREA_TIME, e->event_x)
				mm("time\n");
		}
		break;
	case MOUSE_BTN_MID:
		dd("MOUSE_BTN_MID\n");
		break;
	case MOUSE_BTN_RIGHT:
		dd("MOUSE_BTN_RIGHT\n");
		break;
	default:
		break;
	}

	/* prepare for motion event handling */

	dd("root %p, event %p, child %p\n", e->root, e->event, e->child);
	if (e->event != e->root || !e->child)
		return;

	window_raise(e->child);
	panel_raise(e->root);
	/* subscribe to motion events */
	xcb_grab_pointer(dpy, 0, e->root,
			 XCB_EVENT_MASK_BUTTON_MOTION |
			 XCB_EVENT_MASK_BUTTON_RELEASE,
			 XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
			 e->root, XCB_NONE, XCB_CURRENT_TIME);
	xcb_flush(dpy);
}

static void handle_key_press(xcb_key_press_event_t *e)
{
	struct keymap *ptr = kmap;

	mm("key %p, state %p\n", e->detail, e->state);

	while (ptr->mod) {
		if (ptr->key == e->detail && ptr->mod == e->state) {
			mm("%p pressed\n", ptr->key);
			if (ptr->action && !ptr->arg)
				ptr->action(e);
			else
				ptr->action(ptr->arg);
			return;
		}
		ptr++;
	}
}

static void handle_visibility(xcb_visibility_notify_event_t *e)
{
	struct list_head *cur;

	list_walk(cur, &screens) {
		struct screen *scr = list2screen(cur);
		if (scr->panel == e->window) {
			window_raise(scr->panel);
//			update_panel_items(scr);
			return;
		}
	}
}

static void handle_unmap_notify(xcb_unmap_notify_event_t *e)
{
	if (!window_exists(e->window)) {
		dd("window is gone %p\n", e->window);
		client_del(e->event, e->window);
		return;
	}
}

static void handle_enter_notify(xcb_enter_notify_event_t *e)
{
	struct screen *scr;

	scr = root2screen(e->root);
	if (scr->tag->win)
		window_focus(scr, scr->tag->win, 0);

	window_focus(scr, e->event, 1);

	if (e->mode == MODKEY)
		window_raise(e->event);

	xcb_flush(dpy);
}

static void handle_leave_notify(xcb_leave_notify_event_t *e)
{
#if 0
	struct screen *scr;

	scr = root2screen(e->root);
	window_focus(scr, e->event, 0);
	xcb_flush(dpy);
#endif
}

static void screen_keys(struct screen *scr)
{
	xcb_get_modifier_mapping_cookie_t c;
	xcb_get_modifier_mapping_reply_t *r;
	xcb_keycode_t *mod;
	struct keymap *ptr = kmap;
	int i;

	c = xcb_get_modifier_mapping(dpy);
	r = xcb_get_modifier_mapping_reply(dpy, c, NULL);
	if (!r)
		panic("xcb_get_modifier_mapping_reply() failed\n");

	mod = xcb_get_modifier_mapping_keycodes(r);
	if (!mod)
		panic("\nxcb_get_modifier_mapping_keycodes() failed\n");

	for (i = 0; i < r->keycodes_per_modifier; i++) {
		mm("%d: key code %x ? %x\n", i, mod[i], SHIFT);
	}

	free(r);

	xcb_ungrab_key(dpy, XCB_GRAB_ANY, scr->ptr->root, XCB_MOD_MASK_ANY);

	while (ptr->mod) {
		mm("mod %x, key %x\n", ptr->mod, ptr->key);
		xcb_grab_key(dpy, 1, scr->ptr->root, ptr->mod, ptr->key,
			     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
		ptr++;
	}
}

static void screen_add(xcb_screen_t *ptr, int idx)
{
	struct screen *scr;

	if (!ptr)
		panic("xcb_screen_t pointer is null\n")

	scr = calloc(1, sizeof(*scr));
	if (!scr)
		panic("calloc(%lu) failed\n", sizeof(*scr));

	/* initialize monitor */
	scr->idx = idx;
	scr->ptr = ptr;
	scr->x = 0;
	scr->y = 0;
	scr->w = ptr->width_in_pixels;
	scr->h = ptr->height_in_pixels;
	scr->flags |= SCR_FLG_PANEL;
	strncpy(scr->menustr, "::", sizeof("::") - 1);

	list_init(&scr->tags);

	ii("screen %p, size %dx%d\n", scr, scr->w, scr->h);

	if (scr->flags & SCR_FLG_PANEL) {
		init_panel(scr);
		if (!list_empty(&scr->tags)) {
			ii("current tag %p %s\n", list2tag(scr->tags.next),
			   (list2tag(scr->tags.next))->name);
		}
	}

	screen_keys(scr);
	list_add(&screens, &scr->head);
}

static void print_configure_mask(uint32_t mask)
{
	if (mask & XCB_CONFIG_WINDOW_X)
		printf("XCB_CONFIG_WINDOW_X | ");
	if (mask & XCB_CONFIG_WINDOW_Y)
		printf("XCB_CONFIG_WINDOW_Y | ");
	if (mask & XCB_CONFIG_WINDOW_WIDTH)
		printf("XCB_CONFIG_WINDOW_WIDTH | ");
	if (mask & XCB_CONFIG_WINDOW_HEIGHT)
		printf("XCB_CONFIG_WINDOW_HEIGHT | ");
	if (mask & XCB_CONFIG_WINDOW_BORDER_WIDTH)
		printf("XCB_CONFIG_WINDOW_BORDER_WIDTH | ");
	if (mask & XCB_CONFIG_WINDOW_SIBLING)
		printf("XCB_CONFIG_WINDOW_SIBLING | ");
	if (mask & XCB_CONFIG_WINDOW_STACK_MODE)
		printf("XCB_CONFIG_WINDOW_STACK_MODE | ");
}

#ifndef VERBOSE
#define print_configure_request(e) ;
#else
static void print_configure_request(xcb_configure_request_event_t *e)
{
	dd("prop:\n"
	   " response_type=%u\n"
	   " stack_mode=%u\n"
	   " sequence=%u\n"
	   " parent=%p\n"
	   " window=%p\n"
	   " sibling=%p\n"
	   " x=%d\n"
	   " y=%d\n"
	   " width=%u\n"
	   " height=%u\n"
	   " border_width=%u\n"
	   " value_mask=%u ",
	   e->response_type,
	   e->stack_mode,
	   e->sequence,
	   e->parent,
	   e->window,
	   e->sibling,
	   e->x,
	   e->y,
	   e->width,
	   e->height,
	   e->border_width,
	   e->value_mask);
	print_configure_mask(e->value_mask);
	printf("\n");
}
#endif

static void window_configure(void *arg)
{
	uint32_t val[7] = { 0 };
	int i = 0;
	xcb_configure_request_event_t *req = arg;

	if (req->value_mask & XCB_CONFIG_WINDOW_STACK_MODE)
		val[i++] = req->stack_mode;
	if (req->value_mask & XCB_CONFIG_WINDOW_SIBLING)
		val[i++] = req->sibling;
	if (req->value_mask & XCB_CONFIG_WINDOW_X)
		val[i++] = req->x;
	if (req->value_mask & XCB_CONFIG_WINDOW_Y)
		val[i++] = req->y;
	if (req->value_mask & XCB_CONFIG_WINDOW_WIDTH)
		val[i++] = req->width;
	if (req->value_mask & XCB_CONFIG_WINDOW_HEIGHT)
		val[i++] = req->height;
	if (req->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH)
		val[i++] = req->border_width;

        xcb_configure_window(dpy, req->window, req->value_mask, val);
        xcb_flush(dpy);
}

static void handle_events(void)
{
	xcb_generic_event_t *e;
	uint8_t type;

	e = xcb_wait_for_event(dpy);

	if (xcb_connection_has_error(dpy))
		panic("failed to get event\n");

	if (!e)
		return;

	te("got event %d (%d)\n", e->response_type, XCB_EVENT_RESPONSE_TYPE(e));

	switch (e->response_type & ~0x80) {
	case 0: break; /* NO EVENT */
	case XCB_VISIBILITY_NOTIFY:
		te("XCB_VISIBILITY_NOTIFY: win %p\n",
		   ((xcb_visibility_notify_event_t *) e)->window);
#if 0
                switch (((xcb_visibility_notify_event_t *) e)->state) {
                case XCB_VISIBILITY_FULLY_OBSCURED:
                case XCB_VISIBILITY_PARTIALLY_OBSCURED: /* fall through */
			handle_visibility((xcb_visibility_notify_event_t *) e);
			break;
                }
#endif
		break;
	case XCB_CREATE_NOTIFY:
		te("XCB_CREATE_NOTIFY: win %p\n",
		   ((xcb_create_notify_event_t *) e)->window);
		break;
	case XCB_BUTTON_PRESS:
		mouse_x = ((xcb_button_press_event_t *) e)->root_x;
		mouse_y = ((xcb_button_press_event_t *) e)->root_y;
		mouse_button = ((xcb_button_press_event_t *) e)->detail;
		te("XCB_BUTTON_PRESS: root %p, event %p, child %p\n",
		   ((xcb_button_press_event_t *) e)->root,
		   ((xcb_button_press_event_t *) e)->event,
		   ((xcb_button_press_event_t *) e)->child);
		handle_button_press((xcb_button_press_event_t *) e);
		break;
	case XCB_BUTTON_RELEASE:
		te("XCB_BUTTON_RELEASE\n");
		motion_clean();
		break;
	case XCB_MOTION_NOTIFY:
		te("XCB_MOTION_NOTIFY\n");
		handle_motion_notify((xcb_motion_notify_event_t *) e);
		break;
	case XCB_CLIENT_MESSAGE:
		tt("XCB_CLIENT_MESSAGE\n");
		break;
	case XCB_CONFIGURE_REQUEST:
		te("XCB_CONFIGURE_REQUEST: win %p\n",
		   ((xcb_configure_request_event_t *) e)->window);
		print_configure_request(e);
		window_configure(e);
		break;
	case XCB_CONFIGURE_NOTIFY:
		te("XCB_CONFIGURE_NOTIFY: event %p, window %p, above %p\n",
		   ((xcb_configure_notify_event_t *) e)->event,
		   ((xcb_configure_notify_event_t *) e)->window,
		   ((xcb_configure_notify_event_t *) e)->above_sibling);
		break;
	case XCB_DESTROY_NOTIFY:
		te("XCB_DESTROY_NOTIFY: event %p, win %p\n",
		   ((xcb_destroy_notify_event_t *) e)->event,
		   ((xcb_destroy_notify_event_t *) e)->window);
		client_del(((xcb_destroy_notify_event_t *) e)->event,
			   ((xcb_destroy_notify_event_t *) e)->window);
		break;
	case XCB_ENTER_NOTIFY:
		te("XCB_ENTER_NOTIFY: root %p, event %p, child %p\n",
		   ((xcb_enter_notify_event_t *) e)->root,
		   ((xcb_enter_notify_event_t *) e)->event,
		   ((xcb_enter_notify_event_t *) e)->child);
		te("detail %p, state %p, mode %p\n",
		   ((xcb_enter_notify_event_t *) e)->detail,
		   ((xcb_enter_notify_event_t *) e)->state,
		   ((xcb_enter_notify_event_t *) e)->mode);
		handle_enter_notify((xcb_enter_notify_event_t *) e);
		break;
	case XCB_LEAVE_NOTIFY:
		te("XCB_LEAVE_NOTIFY: root %p, event %p, child %p\n",
		   ((xcb_leave_notify_event_t *) e)->root,
		   ((xcb_leave_notify_event_t *) e)->event,
		   ((xcb_leave_notify_event_t *) e)->child);
		te("detail %p, state %p, mode %p\n",
		   ((xcb_leave_notify_event_t *) e)->detail,
		   ((xcb_leave_notify_event_t *) e)->state,
		   ((xcb_leave_notify_event_t *) e)->mode);
		handle_leave_notify((xcb_leave_notify_event_t *) e);
		break;
	case XCB_EXPOSE:
		te("XCB_EXPOSE: win %p\n", ((xcb_expose_event_t *) e)->window);
		break;
	case XCB_FOCUS_IN:
		te("XCB_FOCUS_IN\n");
		break;
	case XCB_KEY_PRESS:
		te("XCB_KEY_PRESS: root %p, win %p, child %p\n",
		   ((xcb_key_press_event_t *) e)->root,
		   ((xcb_key_press_event_t *) e)->event,
		   ((xcb_key_press_event_t *) e)->child);
		handle_key_press((xcb_key_press_event_t *) e);
		break;
	case XCB_MAPPING_NOTIFY:
		te("XCB_MAPPING_NOTIFY\n");
		break;
	case XCB_MAP_NOTIFY:
		te("XCB_MAP_NOTIFY: win %p\n",
		   ((xcb_map_notify_event_t *) e)->window);
		break;
	case XCB_MAP_REQUEST:
		te("XCB_MAP_REQUEST: parent %p, win %p\n",
		   ((xcb_map_request_event_t *) e)->parent,
		   ((xcb_map_request_event_t *) e)->window);
		client_add(((xcb_map_notify_event_t *) e)->window);
		break;
	case XCB_PROPERTY_NOTIFY:
		te("XCB_PROPERTY_NOTIFY: win %p\n",
		   ((xcb_property_notify_event_t *) e)->window);
		switch (((xcb_property_notify_event_t *) e)->atom) {
		case XCB_ATOM_WM_NAME:
			print_title(((xcb_property_notify_event_t *) e)->window);
			break;
		}
		break;
	case XCB_UNMAP_NOTIFY:
		te("XCB_UNMAP_NOTIFY: event %p, window %p\n",
		   ((xcb_unmap_notify_event_t *) e)->event,
		   ((xcb_unmap_notify_event_t *) e)->window);
		handle_unmap_notify((xcb_unmap_notify_event_t *) e);
		break;
	default:
		te("unhandled event type %d\n", type);
		break;
	}

	free(e);
	print_time(screen);
}

static xcb_atom_t get_atom_by_name(const char *str, int len)
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

static void init_root(xcb_window_t win)
{
	xcb_void_cookie_t c;
	uint32_t val = XCB_EVENT_MASK_BUTTON_PRESS;

	xcb_eval(c, xcb_grab_button(dpy, 0, win, val, XCB_GRAB_MODE_ASYNC,
			XCB_GRAB_MODE_ASYNC, win, XCB_NONE, MOUSE_BTN_LEFT,
			MODKEY));
	xcb_eval(c, xcb_grab_button(dpy, 0, win, val, XCB_GRAB_MODE_ASYNC,
			XCB_GRAB_MODE_ASYNC, win, XCB_NONE, MOUSE_BTN_MID,
			MODKEY));
	xcb_eval(c, xcb_grab_button(dpy, 0, win, val, XCB_GRAB_MODE_ASYNC,
			XCB_GRAB_MODE_ASYNC, win, XCB_NONE, MOUSE_BTN_RIGHT,
			MODKEY));

	/* subscribe events */
	val = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
	      XCB_EVENT_MASK_STRUCTURE_NOTIFY |
	      XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
	xcb_change_window_attributes(dpy, win, XCB_CW_EVENT_MASK, &val);
	xcb_flush(dpy);
}

#define atom_by_name(name) get_atom_by_name(name, sizeof(name) - 1)

static void init_font(void)
{
	XRenderColor normal = FONT_COLOR_NORMAL;
	XRenderColor active = FONT_COLOR_ACTIVE;

	font = XftFontOpen(xdpy, xscr, XFT_FAMILY, XftTypeString,
			   FONT_NAME_FT, XFT_SIZE, XftTypeDouble,
			   FONT_SIZE_FT, NULL);
	if (!font)
		panic("XftFontOpen(%s)\n", FONT_NAME_FT);

        XftColorAllocValue(xdpy, DefaultVisual(xdpy, xscr),
			   DefaultColormap(xdpy, xscr),
			   &normal, &normal_color);
        XftColorAllocValue(xdpy, DefaultVisual(xdpy, xscr),
			   DefaultColormap(xdpy, xscr),
			   &active, &active_color);
}

static void init_keys(void)
{
	xcb_key_symbols_t *syms;
	xcb_keycode_t *key;
	struct keymap *ptr = kmap;

	syms = xcb_key_symbols_alloc(dpy);
	if (!syms)
		panic("xcb_key_symbols_alloc() failed\n");

	while (ptr->mod) {
		key = xcb_key_symbols_get_keycode(syms, ptr->sym);
		if (!key)
			panic("xcb_key_symbols_get_keycode(sym=%p) failed\n",
			      ptr->sym);
		ptr->key = *key;
		mm("grab mod %p + key %p\n", ptr->mod, ptr->key);
		ptr++;
	}

	xcb_key_symbols_free(syms);
}

int main()
{
	xcb_screen_t *scr;

	xdpy = XOpenDisplay(NULL);
	if (!xdpy) {
		ee("XOpenDisplay() failed\n");
		return 1;
	}
	xscr = DefaultScreen(xdpy);
	init_font();
#if 0
	dpy = xcb_connect(NULL, NULL);
#else
	dpy = XGetXCBConnection(xdpy);
#endif
	if (!dpy) {
		ee("xcb_connect() failed\n");
		return 1;
	}

	init_keys();

	a_name = atom_by_name("_NEW_WM_NAME");
	a_desktop = atom_by_name("_NET_WM_DESKTOP");
	a_client_list = atom_by_name("_NET_CLIENT_LIST");

	list_init(&screens);

	screen_add(xcb_setup_roots_iterator(xcb_get_setup(dpy)).data, 0);
	screen = list2screen(screens.next);

	init_root(screen->ptr->root);

	clients_scan();

	mm("enter events loop\n");
	while (1)
		handle_events();

	xcb_set_input_focus(dpy, XCB_NONE, XCB_INPUT_FOCUS_POINTER_ROOT,
			    XCB_CURRENT_TIME);
	xcb_flush(dpy);

	clean();
	return 0;
}
