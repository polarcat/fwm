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
#include <fcntl.h>
#include <time.h>
#include <poll.h>

#include <sys/types.h>
#include <dirent.h>

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

#define sslen(str) (sizeof(str) - 1)

/* defaults */

static char *basedir;

static uint32_t border_docked = 0x505050;
static uint32_t border_active = 0x905030;
static uint32_t border_normal = 0x303030;
static uint32_t panel_bg = 0x101010;
static uint32_t panel_height = 24; /* need to adjust with font height */

/* defines */

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define BORDER_WIDTH 1
#define WINDOW_PAD BORDER_WIDTH

#define FONT_SIZE_FT 10.5
#define FONT_NAME_FT "Monospace"
#define FONT_COLOR_NORMAL { 0x7000, 0x7000, 0x7000, 0xffff, }
//#define FONT_COLOR_NORMAL { 0x1000, 0x1000, 0x1000, 0xffff, }
//#define FONT_COLOR_ACTIVE { 0x7800, 0x9900, 0x4c00, 0xffff, }
#define FONT_COLOR_ACTIVE { 0xff00, 0xff00, 0xff00, 0xffff, }

#define WIN_WIDTH_MIN 2
#define WIN_HEIGHT_MIN 2

#define PROP_MAX 1024

#define CLI_FLG_FIXED (1 << 0)
#define CLI_FLG_FLOAT (1 << 1)
#define CLI_FLG_URGENT (1 << 2)
#define CLI_FLG_NOFOCUS (1 << 3)
#define CLI_FLG_FULLSCR (1 << 4)
#define CLI_FLG_DOCK (1 << 5)

#define SCR_FLG_MAIN (1 << 0)
#define SCR_FLG_PANEL_TOP (1 << 1)

#define TAG_FLG_ACTIVE (1 << 0)

#define STATUS_BUF_MAX 255
#define TAGS_LINE_MAX 128
#define TITLE_LINE_MAX 128

#define TAG_NAME_MAX 32
#define BASE_PATH_MAX 255
#define BASE_PATH_MIN (sizeof("/0/0xffffffffffffffff") - 1)
#define TAGS_MAX 8 /* let's be sane here */

#define MOUSE_BTN_LEFT 1
#define MOUSE_BTN_MID 2
#define MOUSE_BTN_RIGHT 3

#define MENU_ICON "::"

#define TIME_STR_FMT "%Y-%m-%d/%V %H:%M"
#define TIME_STR_DEF "0000-00-00/00 00:00"
#define TIME_STR_MAX sizeof(TIME_STR_DEF)
#define TIME_REFRESH_INTERVAL 30000 /* ms */

#define MODKEY XCB_MOD_MASK_4
#define SHIFT XCB_MOD_MASK_SHIFT
#define CONTROL XCB_MOD_MASK_CONTROL

/* data structures */

static void spawn(const char **argv);

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
	void (*action)(void *);
	void *arg;
};

static uint16_t panel_vpad;
static uint16_t title_max;

struct screen { /* per output abstraction */
	uint8_t id;

	struct list_head head;
	struct list_head tags;
	struct list_head dock;

	struct tag *tag; /* current tag */
	uint8_t ntags; /* number of tags */

	int16_t by; /* bar geometry */
	int16_t x, y;
	uint16_t w, h;

	uint32_t flags; /* SCR_FLG */

	xcb_gcontext_t gc;
	xcb_drawable_t panel;

	/* text drawing related stuff */
	XftDraw *draw;
	xcb_visualtype_t *visual;
	struct panel_item items[PANEL_AREA_MAX];
};

#define list2screen(item) list_entry(item, struct screen, head)

struct list_head screens;
struct screen *curscr;
struct screen *defscr;
static xcb_screen_t *rootscr; /* root window details */

struct tag {
	struct list_head head;
	struct list_head clients;

	uint8_t id;
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
	struct list_head head; /* local list */
	struct list_head list; /* global list */
	int16_t x, y;
	uint16_t w, h;
	xcb_window_t win;
	struct screen *scr;
	uint32_t flags;
};

#define list2client(item) list_entry(item, struct client, head)
#define glob2client(item) list_entry(item, struct client, list)

struct list_head clients; /* keep track of all clients */

/* config */

enum winpos {
	WIN_POS_FILL = 1,
	WIN_POS_CENTER,
	WIN_POS_TOP_LEFT,
	WIN_POS_TOP_RIGHT,
	WIN_POS_BOTTOM_LEFT,
	WIN_POS_BOTTOM_RIGHT,
	WIN_POS_LEFT_FILL,
	WIN_POS_RIGHT_FILL,
	WIN_POS_TOP_FILL,
	WIN_POS_BOTTOM_FILL,
};

enum dir {
	DIR_NEXT = 1,
	DIR_PREV,
};

static void walk_tags(void *);
static void retag_window(void *);
static void next_window(void *);
static void prev_window(void *);
static void raise_window(void *);
static void place_window(void *);

static const char *xrun[] = { "xfrun4", NULL };
static const char *term[] = { "xterm", NULL };
static const char *lock[] = { "xscreensaver-command", "--lock", NULL };

struct keymap {
	uint16_t mod;
	xcb_keysym_t sym;
	xcb_keycode_t key;
	void (*action)(void *);
	void *arg;
};

static struct keymap kmap[] = {
	{ MODKEY, XK_Tab, 0, next_window, NULL, },
	{ MODKEY, XK_BackSpace, 0, prev_window, NULL, },
	{ MODKEY, XK_Return, 0, raise_window, NULL, },
	{ MODKEY, XK_r, 0, spawn, (void *) xrun, },
	{ MODKEY, XK_t, 0, spawn, (void *) term, },
	{ SHIFT, XK_F5, 0, place_window, (void *) WIN_POS_TOP_LEFT, },
	{ SHIFT, XK_F6, 0, place_window, (void *) WIN_POS_TOP_RIGHT, },
	{ SHIFT, XK_F7, 0, place_window, (void *) WIN_POS_BOTTOM_LEFT, },
	{ SHIFT, XK_F8, 0, place_window, (void *) WIN_POS_BOTTOM_RIGHT, },
	{ SHIFT, XK_F10, 0, place_window, (void *) WIN_POS_CENTER, },
	{ MODKEY, XK_F5, 0, place_window, (void *) WIN_POS_LEFT_FILL, },
	{ MODKEY, XK_F6, 0, place_window, (void *) WIN_POS_RIGHT_FILL, },
	{ MODKEY, XK_F7, 0, place_window, (void *) WIN_POS_TOP_FILL, },
	{ MODKEY, XK_F8, 0, place_window, (void *) WIN_POS_BOTTOM_FILL, },
	{ MODKEY, XK_F9, 0, place_window, (void *) WIN_POS_FILL, },
	{ MODKEY, XK_Home, 0, retag_window, (void *) DIR_NEXT, },
	{ MODKEY, XK_End, 0, retag_window, (void *) DIR_PREV, },
	{ MODKEY, XK_Page_Up, 0, walk_tags, (void *) DIR_NEXT, },
	{ MODKEY, XK_Page_Down, 0, walk_tags, (void *) DIR_PREV, },
	{ 0, 0, 0, NULL, NULL, },
};

/* globals */

enum wintype {
	WIN_TYPE_NORMAL,
	WIN_TYPE_DOCK,
	WIN_TYPE_ACTIVE,
};

enum winstatus {
	WIN_STATUS_UNKNOWN,
	WIN_STATUS_HIDDEN,
	WIN_STATUS_VISIBLE,
};

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
static xcb_atom_t a_state;
static xcb_atom_t a_desktop;
static xcb_atom_t a_client_list;
static xcb_atom_t a_systray;

static time_t prev_time;

static char tray_class[16];
static char dock_class[16];

/* ... and the mess begins */

static void text_exts(const char *text, int len, uint16_t *w, uint16_t *h)
{
	XGlyphInfo ext;

	XftTextExtentsUtf8(xdpy, font, (XftChar8 *) text, len, &ext);

	mm("text: %s\n  x = %d\n  y = %d\n  width = %d\n  height = %d\n"
	   "  xOff = %d\n  yOff = %d\n",
	   text, ext.x, ext.y, ext.width, ext.height, ext.xOff, ext.yOff);

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
			    uint16_t w, const char *text, int len)
{
	tt("win=%p, text=%s, len=%d\n", scr->panel, text, len);

	fill_rect(scr->panel, scr->gc, x, 0, w, panel_height);
	if (text && len) {
		XftDrawStringUtf8(scr->draw, color, font, x, panel_vpad,
				  (XftChar8 *) text, len);
		XSync(xdpy, 0);
	}
}

static void spawn_cleanup(int sig)
{
	while (waitpid(-1, NULL, WNOHANG) < 0) {
		if (errno != EINTR)
			break;
	}
}

static void spawn(const char **argv)
{
	if (fork() != 0)
		return;

	tt("execvp %s %s\n", argv[0], argv[1]);
	close(xcb_get_file_descriptor(dpy));
	close(ConnectionNumber(xdpy));
	setsid();
	execvp(argv[0], argv);
	exit(0);
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
		ee("%s, err=%d\n", #func, error__->error_code);\
}

static int get_prop_str(xcb_window_t win, enum xcb_atom_enum_t atom,
			char *ret, int max)
{
	xcb_get_property_cookie_t c;
	xcb_get_property_reply_t *r;
	int len;
	char *tmp;

	c = xcb_get_property(dpy, 0, win, atom, XCB_ATOM_STRING, 0, max);
	r = xcb_get_property_reply(dpy, c, NULL);
	if (!r) {
		ret[0] = '\0';
		return 0;
	}
	tmp = xcb_get_property_value(r);
	len = xcb_get_property_value_length(r);
	max--;
	if (len == 0 || !tmp) {
		ret[0] = '\0';
	} else if (len >= max) {
		ret[max] = '\0';
		len = max - 1;
		memcpy(ret, tmp, len);
	} else if (len < max) {
		ret[len] = '\0';
		memcpy(ret, tmp, len);
	}
	dd("len=%d, max=%d, str=%s\n", len, max, tmp);
	free(r);
	return len;
}

#ifndef VERBOSE
#define panel_items_stat(scr) ;
#else
static void panel_items_stat(struct screen *scr)
{
	const char *name;
	int i;

	for (i = 0; i < PANEL_AREA_MAX; i++) {
		switch (i) {
		case PANEL_AREA_TAGS:
			name = "PANEL_AREA_TAGS";
			break;
		case PANEL_AREA_MENU:
			name = "PANEL_AREA_MENU";
			break;
		case PANEL_AREA_TITLE:
			name = "PANEL_AREA_TITLE";
			break;
		case PANEL_AREA_DOCK:
			name = "PANEL_AREA_DOCK";
			break;
		case PANEL_AREA_TIME:
			name = "PANEL_AREA_TIME";
			break;
		}

		ii("%s: %d,%d (%d)\n", name, scr->items[i].x,
		   scr->items[i].x + scr->items[i].w,
		   scr->items[i].w);
	}
}
#endif

#define TITLE_STR_MAX 128

static void print_title(struct screen *scr, xcb_window_t win)
{
	char title[TITLE_STR_MAX];
	uint16_t len;
	uint16_t w, h;

	if (!win) {
		draw_panel_text(scr, NULL, scr->items[PANEL_AREA_TITLE].x,
				scr->items[PANEL_AREA_TITLE].w, NULL, 0);
	}

	len = get_prop_str(win, XCB_ATOM_WM_NAME, title, sizeof(title));
	if (!len || title[0] == '\0')
		return;

	if (len >= title_max) {
		len = title_max;
		title[len - 2] = '.';
		title[len - 1] = '.';
		title[len] = '.';
	}

	draw_panel_text(scr, &active_color, scr->items[PANEL_AREA_TITLE].x,
			scr->items[PANEL_AREA_TITLE].w,
			(XftChar8 *) title, len);
}

static enum winstatus window_status(xcb_window_t win)
{
	enum winstatus status;
	xcb_get_window_attributes_cookie_t c;
	xcb_get_window_attributes_reply_t *a;

	c = xcb_get_window_attributes(dpy, win);
	a = xcb_get_window_attributes_reply(dpy, c, NULL);
	if (!a)
		status = WIN_STATUS_UNKNOWN;
	else if (a->map_state != XCB_MAP_STATE_VIEWABLE)
		status = WIN_STATUS_HIDDEN;
	else
		status = WIN_STATUS_VISIBLE;

	free(a);
	return status;
}

/* FIXME: check how many times it is being called ... */
static void update_clients_list(void)
{
	struct list_head *cur;

	ii("NET_CLIENT_LIST %d\n", a_client_list);

	xcb_delete_property(dpy, rootscr->root, a_client_list);
	list_walk(cur, &clients) {
		struct client *cli = glob2client(cur);
		if (window_status(cli->win) == WIN_STATUS_UNKNOWN)
			continue;
		ii("append client %p, window %p\n", cli, cli->win);
		xcb_change_property(dpy, XCB_PROP_MODE_APPEND, rootscr->root,
				    a_client_list, XCB_ATOM_WINDOW, 32, 1,
				    &cli->win);
	}
	xcb_flush(dpy);
}

#define ROOT_STR_MAX 64

static void refresh_rules(void)
{
	char name[ROOT_STR_MAX];

	get_prop_str(rootscr->root, XCB_ATOM_WM_NAME, name, sizeof(name));
	if (name[0] == '\0')
		return;

	if (strncmp(name, "tray", sizeof("tray") - 1) == 0) {
		const char *arg = &name[sizeof("tray")];
		if (arg) {
			int len = strlen(arg);
			if (len > 0 && len < sizeof(tray_class)) {
				strncpy(tray_class, arg, strlen(arg));
				tray_class[len] = '\0';
			}
		}
	} else if (strncmp(name, "dock", sizeof("dock") - 1) == 0) {
		const char *arg = &name[sizeof("dock")];
		if (arg) {
			int len = strlen(arg);
			if (len > 0 && len < sizeof(dock_class)) {
				strncpy(dock_class, arg, strlen(arg));
				dock_class[len] = '\0';
			}
		}
	}

	dd("root %s\n", name);
}

#define match_class(a, b) strncmp(a, b, sizeof(b) - 1) == 0

#define CLASS_STR_MAX 32

static int window_docked(xcb_window_t win)
{
	char class[CLASS_STR_MAX];

	get_prop_str(win, XCB_ATOM_WM_CLASS, class, sizeof(class));
	if (class[0] == '\0')
		return 0;

	dd("class %s\n", class);
	if (match_class(class, tray_class)) {
		dd("got tray window\n");
		return 1;
	} else if (match_class(class, dock_class)) {
		dd("got dock window\n");
		return 1;
	} else if (match_class(class, dock_class)) {
		dd("got dock window\n");
		return 1;
	}

	return 0;
}

#define trace_screen_metrics(scr)\
	ii("%s: screen %d geo %dx%d+%d+%d\n", __func__, scr->id, scr->w,\
	   scr->h, scr->x, scr->y)

static struct screen *coord2screen(int16_t x, int16_t y)
{
	struct list_head *cur;

	list_walk(cur, &screens) {
		struct screen *scr = list2screen(cur);
		if (scr->x <= x && x <= (scr->x + scr->w - 1) &&
		    scr->y <= y && y <= (scr->y + scr->h + panel_height - 1)) {
			return scr;
		}
	}
	return NULL;
}

static struct client *scr2client(struct screen *scr, xcb_window_t win,
				 enum wintype type)
{
	struct list_head *cur;
	struct list_head *head;

	if (type == WIN_TYPE_DOCK)
		head = &scr->dock;
	else
		head = &scr->tag->clients;

	list_walk(cur, head) {
		struct client *cli = list2client(cur);
		if (cli->win == win)
			return cli;
	}
	return NULL;
}

static struct client *win2client(xcb_window_t win)
{
	struct list_head *cur;

	list_walk(cur, &clients) {
		struct client *cli = glob2client(cur);
		if (cli->win == win)
			return cli;
	}

	return NULL;
}

static int panel_window(xcb_window_t win)
{
	struct list_head *cur;

	list_walk(cur, &screens) {
		struct screen *scr = list2screen(cur);
		if (scr->panel == win)
			return 1;
	}
	return 0;
}

#if 0
static struct client *find_window(xcb_window_t win, int16_t x, int16_t y)
{
	struct screen *scr;

	scr = coord2screen(x, y);
	if (!scr) {
		return win2client(win);
	} else {
		scr2client(scr, win, WIN_TYPE_DOCK);
	}
}
#endif

#if 0
static void trace_attrs(xcb_window_t win)
{
	xcb_get_window_attributes_cookie_t c;
	xcb_get_window_attributes_reply_t *a;

	c = xcb_get_window_attributes(dpy, win);
	a = xcb_get_window_attributes_reply(dpy, c, NULL);
	if (!a) {
		ee("xcb_get_window_attributes() failed\n");
		return;
	}

	if (a->override_redirect)
		ww("win %p, override_redirect\n", win);
	else if (a->map_state != XCB_MAP_STATE_VIEWABLE)
		ww("win %p, non-viewable\n", win);

	free(a);
}
#endif

static void window_state(xcb_window_t win, uint8_t state)
{
	uint32_t data[] = { state, XCB_NONE };
	xcb_change_property_checked(dpy, XCB_PROP_MODE_REPLACE, win,
				    a_state, a_state, 32, 2, data);
}

static void window_lower(xcb_window_t win)
{
	uint32_t val[1] = { XCB_STACK_MODE_BELOW, };
	xcb_configure_window(dpy, win, XCB_CONFIG_WINDOW_STACK_MODE, val);
}

static void window_raise(xcb_window_t win)
{
	uint32_t val[1] = { XCB_STACK_MODE_ABOVE, };
	uint16_t mask = XCB_CONFIG_WINDOW_STACK_MODE;
	xcb_configure_window_checked(dpy, win, mask, val);
}

static void window_border_color(xcb_window_t win, uint32_t color)
{
	uint32_t val[1] = { color, };
	uint16_t mask = XCB_CW_BORDER_PIXEL;
	xcb_change_window_attributes_checked(dpy, win, mask, val);
}

static void window_border_width(xcb_window_t win, uint16_t width)
{
	uint32_t val[1] = { width, };
	uint16_t mask = XCB_CONFIG_WINDOW_BORDER_WIDTH;
	xcb_configure_window_checked(dpy, win, mask, val);
}

static void window_focus(struct screen *scr, xcb_window_t win, int focus)
{
	struct client *cli;

	tt("win %p, focus %d\n", win, focus);

	if (!focus) {
		window_border_color(win, border_normal);
	} else {
		window_border_color(win, border_active);
		print_title(scr, win);
		scr->tag->win = win;
	}

	xcb_set_input_focus_checked(dpy, XCB_NONE, win, XCB_CURRENT_TIME);
}

static void switch_window(struct screen *scr, enum dir dir)
{
	struct list_head *cur;
	struct client *cli;
	uint8_t found;

	if (list_empty(&scr->tag->clients))
		return;
	else if (list_single(&scr->tag->clients))
		return;

	tt("tag %s, win %p\n", scr->tag->name, scr->tag->win);

	cli = scr2client(scr, scr->tag->win, WIN_TYPE_NORMAL);
	if (!cli)
		return;

	found = 0;
	if (dir == DIR_NEXT) {
		list_walk(cur, &cli->head) {
			cli = list2client(cur);
			if (window_status(cli->win) == WIN_STATUS_VISIBLE) {
				found = 1;
				break;
			}
		}

		if (found)
			goto out;

		list_walk(cur, &scr->tag->clients) {
			cli = list2client(cur);
			if (window_status(cli->win) == WIN_STATUS_VISIBLE) {
				found = 1;
				break;
			}
		}
	} else {
		list_back(cur, &cli->head) {
			cli = list2client(cur);
			if (window_status(cli->win) == WIN_STATUS_VISIBLE) {
				found = 1;
				break;
			}
		}

		if (found)
			goto out;

		list_back(cur, &scr->tag->clients) {
			cli = list2client(cur);
			if (window_status(cli->win) == WIN_STATUS_VISIBLE) {
				found = 1;
				break;
			}
		}
	}

	if (!found)
		return;

out:
	window_raise(cli->win);
	window_focus(scr, scr->tag->win, 0);
	window_focus(scr, cli->win, 1);
	xcb_warp_pointer(dpy, XCB_NONE, cli->win, 0, 0, 0, 0, cli->w / 2,
			 cli->h / 2);
	xcb_flush(dpy);
}

static void client_moveresize(struct client *cli, int16_t x, int16_t y,
			      uint16_t w, uint16_t h)
{
	uint32_t val[4];
	uint16_t mask;

	if (!(cli->flags & CLI_FLG_DOCK)) {
		/* correct window location */
		if (x < cli->scr->x || x > cli->scr->x + cli->scr->w)
			x = cli->scr->x;
		if (y < cli->scr->y || y > cli->scr->y + cli->scr->h)
			y = cli->scr->y;

		/* fit into monitor space */
		if (w > cli->scr->w)
			w = cli->scr->w - 2 * BORDER_WIDTH;
		else if (w < WIN_WIDTH_MIN)
			w = cli->scr->w / 2 - 2 * BORDER_WIDTH;
		if (h > cli->scr->h)
			h = cli->scr->h - 2 * BORDER_WIDTH;
		else if (h < WIN_HEIGHT_MIN)
			h = cli->scr->h / 2 - 2 * BORDER_WIDTH;

		if (cli->scr->flags & SCR_FLG_PANEL_TOP)
			y += panel_height;
	}

	val[0] = cli->x = x;
	val[1] = cli->y = y;
	val[2] = cli->w = w;
	val[3] = cli->h = h;
	mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
	mask |= XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
	xcb_configure_window(dpy, cli->win, mask, val);

	tt("cli %p, win %p, geo %ux%u+%d+%d\n", cli, cli->win,
	   cli->w, cli->h, cli->x, cli->y);
}

static void place_window(void *arg)
{
	int16_t x, y;
	uint16_t w, h;
	struct client *cli;

	ii("%s: screen %d, win %p, where %d\n", __func__, curscr->id,
	   curscr->tag->win, (enum winpos) arg);

	cli = scr2client(curscr, curscr->tag->win, WIN_TYPE_NORMAL);
	if (!cli)
		return;

	switch ((enum winpos) arg) {
	case WIN_POS_FILL:
		dd("WIN_POS_FILL\n");
		x = curscr->x;
		y = curscr->y;
		w = curscr->w - 2 * BORDER_WIDTH - WINDOW_PAD;
		h = curscr->h - 2 * BORDER_WIDTH - WINDOW_PAD;
		break;
	case WIN_POS_CENTER:
		x = curscr->x + curscr->w / 2 - curscr->w / 4;
		y = curscr->y + curscr->h / 2 - curscr->h / 4;
		goto halfwh;
	case WIN_POS_LEFT_FILL:
		dd("WIN_POS_LEFT_FILL\n");
		x = curscr->x;
		y = curscr->y;
		goto halfw;
	case WIN_POS_RIGHT_FILL:
		dd("WIN_POS_RIGHT_FILL\n");
		x = curscr->x + curscr->w / 2;
		y = curscr->x;
		goto halfw;
	case WIN_POS_TOP_FILL:
		dd("WIN_POS_TOP_FILL\n");
		x = curscr->x;
		y = curscr->y;
		goto halfh;
	case WIN_POS_BOTTOM_FILL:
		dd("WIN_POS_BOTTOM_FILL\n");
		x = curscr->x;
		y = curscr->y + curscr->h / 2;
		goto halfh;
	case WIN_POS_TOP_LEFT:
		dd("WIN_POS_TOP_LEFT\n");
		x = curscr->x;
		y = curscr->y;
		goto halfwh;
	case WIN_POS_TOP_RIGHT:
		dd("WIN_POS_TOP_RIGHT\n");
		x = curscr->x + curscr->w / 2;
		y = curscr->y;
		goto halfwh;
	case WIN_POS_BOTTOM_LEFT:
		dd("WIN_POS_BOTTOM_LEFT\n");
		x = curscr->x;
		y = curscr->y + curscr->h / 2;
		goto halfwh;
	case WIN_POS_BOTTOM_RIGHT:
		dd("WIN_POS_BOTTOM_RIGHT\n");
		x = curscr->x + curscr->w / 2;
		y = curscr->y + curscr->h / 2;
		goto halfwh;
	default:
		return;
	}

out:
	client_moveresize(cli, x, y, w, h);
	xcb_flush(dpy);
	return;
halfwh:
	w = curscr->w / 2 - 2 * BORDER_WIDTH - WINDOW_PAD;
	h = curscr->h / 2 - 2 * BORDER_WIDTH - WINDOW_PAD;
	goto out;
halfw:
	w = curscr->w / 2 - 2 * BORDER_WIDTH;
	h = curscr->h - 2 * BORDER_WIDTH;
	goto out;
halfh:
	w = curscr->w - 2 * BORDER_WIDTH - WINDOW_PAD;
	h = curscr->h / 2 - 2 * BORDER_WIDTH - WINDOW_PAD;
	goto out;
}

static void next_window(void *arg)
{
	struct screen *scr;
	int16_t x, y;

	x = ((xcb_key_press_event_t *) arg)->root_x;
	y = ((xcb_key_press_event_t *) arg)->root_y;
	scr = coord2screen(x, y);
	switch_window(scr, DIR_NEXT);
}

static void prev_window(void *arg)
{
	struct screen *scr;
	int16_t x, y;

	x = ((xcb_key_press_event_t *) arg)->root_x;
	y = ((xcb_key_press_event_t *) arg)->root_y;
	scr = coord2screen(x, y);
	switch_window(scr, DIR_PREV);
}

static void raise_window(void *arg)
{
	xcb_key_press_event_t *e = (xcb_key_press_event_t *) arg;
	struct screen *scr;
	struct client *cli;

	tt("\n");

	scr = coord2screen(e->root_x, e->root_y);

	if (list_empty(&scr->tag->clients))
		return;

	cli = scr2client(scr, scr->tag->win, WIN_TYPE_NORMAL);
	window_raise(cli->win);
	xcb_flush(dpy);
}

static void panel_raise(struct screen *scr)
{
	if (scr && scr->panel) {
		window_raise(scr->panel);
		struct list_head *cur;
		list_walk(cur, &scr->dock) {
			struct client *cli = list2client(cur);
			window_raise(cli->win);
		}
	}
}

static void print_tag(struct screen *scr, struct tag *tag, XftColor *color,
			int flush)
{
	draw_panel_text(scr, color, tag->x, tag->w, (XftChar8 *) tag->name,
		        tag->nlen);

	if (flush) {
		xcb_set_input_focus(dpy, XCB_NONE,
				    XCB_INPUT_FOCUS_POINTER_ROOT,
				    XCB_CURRENT_TIME);
		xcb_flush(dpy);
	}
}

static void show_windows(struct screen *scr)
{
	struct list_head *cur;

	list_walk(cur, &scr->tag->clients) {
		struct client *cli = list2client(cur);
		window_state(cli->win, XCB_ICCCM_WM_STATE_NORMAL);
		xcb_map_window_checked(dpy, cli->win);
	}

	if (scr->tag->win) {
		tt("tag->win=%p\n", scr->tag->win);
		window_focus(scr, scr->tag->win, 1);
	}
	xcb_flush(dpy);
}

static void hide_windows(struct tag *tag)
{
	struct list_head *cur;

	list_walk(cur, &tag->clients) {
		struct client *cli = list2client(cur);
		window_state(cli->win, XCB_ICCCM_WM_STATE_ICONIC);
		xcb_unmap_window_checked(dpy, cli->win);
	}

	xcb_flush(dpy);
}

static void switch_tag(struct screen *scr, enum dir dir)
{
	struct tag *tag;

	if (dir == DIR_NEXT) {
		if (scr->tag->head.next == &scr->tags) /* end of list */
			tag = list2tag(scr->tags.next);
		else
			tag = list2tag(scr->tag->head.next);
	} else {
		if (scr->tag->head.prev == &scr->tags) /* head of list */
			tag = list2tag(scr->tags.prev);
		else
			tag = list2tag(scr->tag->head.prev);
	}

	scr->tag->win = 0;
	scr->tag->flags &= ~TAG_FLG_ACTIVE;
	print_tag(scr, scr->tag, &normal_color, 0);
	hide_windows(scr->tag);

	tag->flags |= TAG_FLG_ACTIVE;
	print_tag(scr, tag, &active_color, 1);
	show_windows(scr);
	scr->tag = tag;

}

static void walk_tags(void *arg)
{
	tt("\n");

	if (list_single(&curscr->tags))
		return;

	switch_tag(curscr, (enum dir) arg);
}

#if 0
static xcb_window_t trace_hints(struct screen *scr, xcb_window_t win)
{
	xcb_window_t trans = XCB_WINDOW_NONE;
	xcb_icccm_wm_hints_t h = { 0 };

	xcb_icccm_get_wm_transient_for_reply(dpy,
		xcb_icccm_get_wm_transient_for(dpy, win), &trans, NULL);
	xcb_icccm_get_wm_hints_reply(dpy, xcb_icccm_get_wm_hints(dpy, win),
				     &h, NULL);

	tt("win %p, group %p, transient for %p\n", win, h.window_group, trans);
	return h.window_group;
}
#endif

static void trace_screens(void)
{
	struct list_head *cur;

	list_walk(cur, &screens) {
		struct screen *scr = list2screen(cur);
		ii("screen %d, geo %ux%u+%d+%d, pos %d,%d\n", scr->id,
		   scr->w, scr->h, scr->x, scr->y, scr->x + scr->w,
		   scr->y + scr->h);
	}
}

static void trace_windows(struct tag *tag)
{
	struct list_head *cur;

	list_walk(cur, &tag->clients)
		dd("tag %s, win %p\n", tag->name, (list2client(cur))->win);
}

static void retag_window(void *arg)
{
	struct client *cli;

	tt("\n");

	if (list_single(&curscr->tags))
		return;

	cli = scr2client(curscr, curscr->tag->win, WIN_TYPE_NORMAL);
	if (!cli)
		return;

	list_del(&cli->head);
	walk_tags(arg);
	list_add(&curscr->tag->clients, &cli->head);
	curscr->tag->win = cli->win;
	window_focus(curscr, curscr->tag->win, 1);
	window_raise(curscr->tag->win);
	xcb_flush(dpy);
}

#if 0
static void resize_window(xcb_window_t win, uint16_t w, uint16_t h)
{
	int val[2], mask;

	mask = XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
	val[0] = w;
	val[1] = h;
	xcb_configure_window(dpy, win, mask, val);
	xcb_flush(dpy);
}

static void move_window(xcb_window_t win, int16_t x, int16_t y)
{
	int val[2], mask;

	mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
	val[0] = x;
	val[1] = y;
	xcb_configure_window(dpy, win, mask, val);
	xcb_flush(dpy);
	ii("window %p moved to %d,%d\n", win, x, y);
}
#endif

static struct tag *id2tag(struct screen *scr, uint8_t id)
{
	struct list_head *cur;

	list_walk(cur, &scr->tags) {
		struct tag *tag = list2tag(cur);
		if (tag->id == id)
			return tag;
	}
	return NULL;
}

static struct tag *client_tag(struct screen *scr, xcb_window_t win)
{
	int len;
	uint8_t id;
	char class[CLASS_STR_MAX];
	char path[BASE_PATH_MAX], *ptr;
	struct stat st;

	get_prop_str(win, XCB_ATOM_WM_CLASS, class, sizeof(class));
	if (class[0] == '\0')
		return NULL;

	dd("class: %s\n", class);

	snprintf(path, sizeof(path), "%s/%d/", basedir, scr->id);
	if (stat(path, &st) < 0)
		return NULL;

	len = strlen(path);
	ptr = path + len;
	len = sizeof(path) - 1 - len;

	for (id = 0; id < scr->ntags; id++) {
		snprintf(ptr, len, "%d/%s", id, class);
		if (stat(path, &st) < 0)
			continue;

		dd("class %s, tag %d\n", class, id);
		return id2tag(scr, id);
	}

	return NULL;
}

static int calc_title_max(struct screen *scr)
{
	int16_t w, h, i;
	char *tmp;

	i = 1;
	w = 0;
	while (w < scr->items[PANEL_AREA_TITLE].w) {
		tmp = calloc(1, i + 1);
		memset(tmp, 'w', i);
		text_exts(tmp, strlen(tmp), &w, &h);
		free(tmp);
		i++;
	}
	title_max = i - 2;
	dd("title_max=%u\n", title_max);
}

#define DOCKWIN_GAP (BORDER_WIDTH * 2)
#define DOCKWIN_OFFSET (6 * DOCKWIN_GAP)

static void dock_del(struct client *cli)
{
	struct list_head *cur;
	struct screen *scr = cli->scr;
	int16_t x;

	list_del(&cli->head);
	free(cli);
	x = scr->items[PANEL_AREA_TIME].x - DOCKWIN_OFFSET;
	list_walk(cur, &cli->scr->dock) {
		cli = list2client(cur);
		x -= (cli->w + DOCKWIN_GAP + BORDER_WIDTH);
		client_moveresize(cli, x, cli->y, cli->w, cli->h);
	}

	scr->items[PANEL_AREA_TITLE].w = x - scr->items[PANEL_AREA_TITLE].x;
	calc_title_max(scr);
}

static void dock_add(struct screen *scr, struct client *cli)
{
	uint32_t mask, val[1] = { BORDER_WIDTH, };
	struct list_head *cur;
	int16_t x, y;

	list_add(&scr->dock, &cli->head);
	list_add(&clients, &cli->list);

	cli->scr = defscr;
	cli->flags |= CLI_FLG_DOCK;
	cli->w = panel_height + panel_height / 3;
	cli->h = panel_height - 3 * DOCKWIN_GAP - 1;

	x = scr->items[PANEL_AREA_TIME].x - DOCKWIN_OFFSET;

	if (scr->flags & SCR_FLG_PANEL_TOP)
		y = DOCKWIN_GAP;
	else
		y = scr->h + DOCKWIN_GAP;

	list_walk(cur, &scr->dock) {
		struct client *cli = list2client(cur);
		x -= (cli->w + DOCKWIN_GAP + BORDER_WIDTH);
		client_moveresize(cli, x, y, cli->w, cli->h);
	}

	scr->items[PANEL_AREA_TITLE].w = x - scr->items[PANEL_AREA_TITLE].x;
	calc_title_max(scr);
	window_state(cli->win, XCB_ICCCM_WM_STATE_NORMAL);
	window_border_color(cli->win, border_docked);
	xcb_map_window(dpy, cli->win);
	xcb_flush(dpy);
}

static struct screen *pointer2screen(void)
{
	struct screen *scr;
	xcb_query_pointer_cookie_t c;
	xcb_query_pointer_reply_t *r;

	scr = NULL;
	c = xcb_query_pointer(dpy, rootscr->root);
        r = xcb_query_pointer_reply(dpy, c, NULL);
        if (r) {
		scr = coord2screen(r->root_x, r->root_y);
		free(r);
	}

        return scr;
}

static void client_add(xcb_window_t win)
{
	uint8_t ntag;
	struct tag *tag;
	struct screen *scr;
	struct client *cli;
	uint32_t val[1];
	int dy, dh;
	xcb_get_geometry_reply_t *g;
	xcb_get_window_attributes_cookie_t c;
	xcb_get_window_attributes_reply_t *a;

	tt("win %p\n", win);

	if (win == rootscr->root)
		return;

	a = NULL;
	/* get initial geometry */
	g = xcb_get_geometry_reply(dpy, xcb_get_geometry(dpy, win), NULL);
	if (!g) {
		ee("xcb_get_geometry() failed\n");
		goto out;
	}

	scr = coord2screen(g->x, g->y);
	if (!scr)
		scr = pointer2screen();
	if (!scr) {
		if (panel_window(win)) {
			goto out;
		} else if (win2client(win)) {
			xcb_map_window(dpy, win);
			xcb_flush(dpy);
			goto out; /* window is already added */
		}
		scr = defscr;
	} else {
		if (scr->panel == win) {
			goto out; /* don't handle it here */
		} else if (scr2client(scr, win, WIN_TYPE_DOCK)) {
			ii("already on dock list\n");
			goto out; /* already added */
		} else if (scr2client(scr, win, WIN_TYPE_NORMAL)) {
			ii("already on clients list\n");
			xcb_map_window(dpy, win);
			xcb_flush(dpy);
			goto out; /* already added */
		}
	}

	refresh_rules();

	c = xcb_get_window_attributes(dpy, win);
	a = xcb_get_window_attributes_reply(dpy, c, NULL);
	if (!a) {
		ee("xcb_get_window_attributes() failed\n");
		return;
	}

	if (a->override_redirect) {
		dd("ignore redirected window %p\n", win);
		goto out;
	}

	/* tell x server to restore window upon our sudden exit */
	xcb_change_save_set(dpy, XCB_SET_MODE_INSERT, win);

	cli = calloc(1, sizeof(*cli));
	if (!cli) {
		ee("calloc(%lu) failed\n", sizeof(*cli));
		goto out;
	}

	cli->scr = scr;
	cli->win = win;
	window_raise(win);
	window_border_width(cli->win, BORDER_WIDTH);
	window_border_color(cli->win, border_normal);

	if (window_docked(win)) {
		dock_add(scr, cli);
		goto out;
	}

	/* subscribe events */
	val[0] = XCB_EVENT_MASK_ENTER_WINDOW |
		 XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_change_window_attributes(dpy, win, XCB_CW_EVENT_MASK, val);

	if (!g->depth && !a->colormap) {
		dd("win %p, root %p, colormap=%p, class=%u, depth=%u\n",
		   win, g->root, a->colormap, a->_class, g->depth);
		xcb_destroy_window(dpy, win);
		ww("zombie window %p destroyed\n", win);
		goto out;
	}

	tag = client_tag(scr, win);
	if (!tag)
		tag = scr->tag;

	if (!tag->win) {
		list_add(&tag->clients, &cli->head);
	} else { /* attempt to list windows in order of appearance */
		struct client *cur = scr2client(scr, tag->win, 0);
		if (cur)
			list_add(&cur->head, &cli->head);
		else
			list_add(&tag->clients, &cli->head);
	}
	/* also add to global list of clients */
	list_add(&clients, &cli->list);

	client_moveresize(cli, g->x, g->y, g->width, g->height);

	if (scr->tag == tag) {
		if (tag->win)
			window_focus(scr, scr->tag->win, 0);

		window_state(cli->win, XCB_ICCCM_WM_STATE_NORMAL);
		xcb_map_window(dpy, cli->win);
		window_focus(scr, cli->win, 1);
		print_title(scr, cli->win);
	} else {
		window_state(cli->win, XCB_ICCCM_WM_STATE_ICONIC);
		xcb_unmap_window(dpy, cli->win);
	}

	dd("screen %d, tag %s, cli %p, win %p, geo %ux%u+%d+%d\n", scr->id,
	   scr->tag->name, cli, cli->win, cli->w, cli->h, cli->x, cli->y);

	update_clients_list();
out:
	xcb_flush(dpy);
	free(a);
	free(g);
}

static void client_del(xcb_window_t win)
{
	struct client *cli;

	cli = win2client(win);
	if (!cli) {
		tt("win %p was not managed\n", win);
		xcb_unmap_subwindows_checked(dpy, win);
		goto out;
	}

	if (cli->flags & CLI_FLG_DOCK) {
		dock_del(cli);
		return;
	}

	switch_window(cli->scr, DIR_NEXT);
	list_del(&cli->head);
	list_del(&cli->list);

	if (list_empty(&cli->scr->tag->clients))
		print_title(cli->scr, 0);

	free(cli);
out:
	tt("pid %d, deleted win %p\n", getpid(), win);
	update_clients_list();
}

static int screen_panel(xcb_window_t win)
{
	struct list_head *cur;

	list_walk(cur, &screens) {
		struct screen *scr = list2screen(cur);
		if (scr->panel == win)
			return 1;
	}
	return 0;
}

static void clients_scan(void)
{
	int i, n;
	xcb_query_tree_cookie_t c;
	xcb_query_tree_reply_t *tree;
	xcb_window_t *wins;

	list_init(&clients);

	/* walk through windows tree */
	c = xcb_query_tree(dpy, rootscr->root);
	tree = xcb_query_tree_reply(dpy, c, 0);
	if (!tree)
		panic("xcb_query_tree_reply() failed\n");

	n = xcb_query_tree_children_length(tree);
	wins = xcb_query_tree_children(tree);

	/* map clients onto the current screen */
	ii("%d clients found\n", n);
	for (i = 0; i < n; i++) {
		dd("++ handle win %p\n", wins[i]);
		if (screen_panel(wins[i]))
			continue;
		client_add(wins[i]);
	}

	free(tree);
	update_clients_list();
}

static void print_menu(struct screen *scr)
{
	uint16_t x, w;

	x = scr->items[PANEL_AREA_MENU].x;
	w = scr->items[PANEL_AREA_MENU].w;

	draw_panel_text(scr, &normal_color, x, w, (XftChar8 *) MENU_ICON,
		        sizeof(MENU_ICON) - 1);
}

static void print_time(struct screen *scr, int force)
{
	time_t t;
	struct tm *tm;
	char str[TIME_STR_MAX];
	int16_t x;
	uint16_t w;

	ii("print_time screen %d, %p ? %p\n", scr->id, scr, defscr);

	if (scr != defscr)
		return;

	t = time(NULL);
	if (t == prev_time && !force)
		return;

	tm = localtime(&t);
	if (!tm)
		return;

	if (strftime(str, TIME_STR_MAX, TIME_STR_FMT, tm) == 0)
		strncpy(str, TIME_STR_DEF, TIME_STR_MAX);

	x = scr->items[PANEL_AREA_TIME].x;
	w = scr->items[PANEL_AREA_TIME].w;

	draw_panel_text(scr, &normal_color, x, w, (XftChar8 *) str,
			sizeof(str) - 1);
	prev_time = t;
}

static int tag_clicked(struct tag *tag, int16_t x)
{
	if (x >= tag->x && x <= tag->x + tag->w)
		return 1;
	return 0;
}

static void select_tag(struct screen *scr, int x)
{
	struct list_head *cur;
	struct tag *prev;

	if (scr->tag && tag_clicked(scr->tag, x)) {
		return;
	} else if (scr->tag) { /* deselect current instantly */
		scr->tag->flags &= ~TAG_FLG_ACTIVE;
		print_tag(scr, scr->tag, &normal_color, 0);
	}

	prev = scr->tag;

	list_walk(cur, &scr->tags) { /* refresh labels */
		struct tag *tag = list2tag(cur);

		if (tag == scr->tag) {
			continue;
		} else if (!tag_clicked(tag, x)) {
			tag->flags &= ~TAG_FLG_ACTIVE;
			print_tag(scr, tag, &normal_color, 0);
		} else {
			tag->flags |= TAG_FLG_ACTIVE;
			print_tag(scr, tag, &active_color, 1);
			scr->tag = tag;
			break;
		}
	}

	if (scr->tag && scr->tag != prev) {
		if (prev)
			hide_windows(prev);
		show_windows(scr);
	}
}

static int tag_add(struct screen *scr, const char *name, uint8_t id, uint16_t pos)
{
	XftColor *color;
	struct tag *tag;
	uint16_t h;

	tag = calloc(1, sizeof(*tag));
	if (!tag) {
		ee("calloc() failed\n");
		return;
	}

	scr->ntags++;
	tag->scr = scr;
	tag->id = id;
	tag->nlen = strlen(name);
	tag->name = strdup(name);
	if (!tag->name) {
		ee("strdup(%s) failed\n", name);
		free(tag);
		return;
	}

	list_init(&tag->clients);
	list_add(&scr->tags, &tag->head);

	text_exts(name, tag->nlen, &tag->w, &h);

	if (pos != scr->items[PANEL_AREA_TAGS].x) {
		color = &normal_color;
		tag->flags &= ~TAG_FLG_ACTIVE;
	} else {
		color = &active_color;
		tag->flags |= TAG_FLG_ACTIVE;
		scr->tag = tag;
	}

	draw_panel_text(scr, color, pos, tag->w, (XftChar8 *) tag->name,
			tag->nlen);

	tag->x = pos;
	tag->w += FONT_SIZE_FT; /* spacing */
	return pos + tag->w;
}

/*
 * Tags dir structure:
 *
 * /<basedir>/<screennumber>/tags/<tagname>/<winclass>
 */
static int init_tags(struct screen *scr)
{
	struct list_head *cur;
	int16_t i;
	uint16_t pos;
	char path[BASE_PATH_MAX];
	DIR *dir;
	struct dirent *ent;

	i = 0;
	pos = scr->items[PANEL_AREA_TAGS].x;
	if (!basedir) {
		ww("no user defined tags\n");
		goto out;
	}

	snprintf(path, sizeof(path), "%s/%d/tags", basedir, scr->id);
	dir = opendir(path);
	if (!dir) {
		ii("no user defined tags for screen %d\n", scr->id);
		goto out;
	}

	while ((ent = readdir(dir))) {
		if (ent->d_name[0] == '.' || ent->d_name[1] == '.')
			continue;
		ii("tag %s, offset %d\n", ent->d_name, pos);
		pos = tag_add(scr, ent->d_name, i++, pos);
	}

	closedir(dir);
out:
	if (i == 0) /* add default tag */
		pos = tag_add(scr, "*", 0, scr->items[PANEL_AREA_TAGS].x);

	return pos - panel_vpad / 2;
}

static void redraw_panel_items(struct screen *scr)
{
	XftColor *color;
	struct list_head *cur;

	list_walk(cur, &scr->tags) {
		struct tag *tag = list2tag(cur);
		if (scr->tag == tag)
			color = &active_color;
		else
			color = &normal_color;

		draw_panel_text(scr, color, tag->x, tag->w,
				(XftChar8 *) tag->name, tag->nlen);
	}

	print_menu(scr);
	print_title(scr, scr->tag->win);
	print_time(scr, 1);
}

static int update_panel_items(struct screen *scr)
{
	int16_t x;
	uint16_t h, w;

	text_exts(TIME_STR_DEF, TIME_STR_MAX, &w, &h);
	scr->items[PANEL_AREA_TIME].x = scr->w - w;
	scr->items[PANEL_AREA_TIME].w = w;

	panel_vpad = panel_height - (panel_height - FONT_SIZE_FT) / 2;

	x = FONT_SIZE_FT;
	scr->items[PANEL_AREA_TAGS].x = x;
	scr->items[PANEL_AREA_TAGS].w = init_tags(scr);
	x += scr->items[PANEL_AREA_TAGS].w + 1;

	text_exts(MENU_ICON, sizeof(MENU_ICON) - 1, &w, &h);
	scr->items[PANEL_AREA_MENU].x = x;
	scr->items[PANEL_AREA_MENU].w = w + panel_vpad;
	x += scr->items[PANEL_AREA_MENU].w + 1;

	w = scr->items[PANEL_AREA_TIME].x - 1 - x - panel_vpad;
	scr->items[PANEL_AREA_TITLE].w = w;
	scr->items[PANEL_AREA_TITLE].x = x;
	calc_title_max(scr);

	print_menu(scr);
	print_time(scr, 1); /* last element on panel */
}

#if 0
/*
 * Panel items:
 *
 * /<basedir>/<screennumber>/{title,menu,clock}/action
 * 'action' --> any executable, args: <cur_win_id> <cur_win_x> <cur_win_y>
 */

static void init_panel_items(struct screen *scr)
{
	char path[BASE_PATH_MAX];
	DIR *dir;
	struct dirent *ent;
	int i;

	snprintf(path, sizeof(path), "%s/%d/tags", basedir, scr->id);
	dir = opendir(path);
	if (!dir) {
		ee("opendir(%s) failed\n", path);
		return;
	}

	while ((ent = readdir(dir))) {
		if (strncmp("tags", sizeof("tags") - 1, ent->d_name) == 0) {

		}
	}

	closedir(dir);
}
#endif

static void init_panel(struct screen *scr)
{
	int y;
	uint32_t val[2], mask;

	scr->panel = xcb_generate_id(dpy);

	mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	val[0] = panel_bg;
	val[1] = XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_VISIBILITY_CHANGE;

	if (scr->flags & SCR_FLG_PANEL_TOP)
		y = scr->y;
	else
		y = (scr->h + scr->y) - panel_height;

	xcb_create_window(dpy, XCB_COPY_FROM_PARENT, scr->panel, rootscr->root,
			  scr->x, y, scr->w, panel_height, 0,
			  XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  rootscr->root_visual, mask, val);
	xcb_flush(dpy); /* flush this operation otherwise panel will be
			   misplaced in multiscreen setup */

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

	ii("screen %d, panel %p geo %ux%u+%d+%d\n", scr->id, scr->panel,
	   scr->w, panel_height, scr->x, y);
}

static void update_panel_title(xcb_window_t win)
{
	struct list_head *cur;

	list_walk(cur, &screens) {
		struct screen *scr = list2screen(cur);
		struct client *cli;

		if (scr->tag->win != win)
			continue;

		cli = list2client(scr->tag->clients.next);
		if (cli) {
			scr->tag->win = cli->win;
			print_title(scr, cli->win);
		} else {
			scr->tag->win = 0;
			print_title(scr, 0);
		}
	}
}

#define pointer_inside(scr, area, ex)\
	(ex >= scr->items[area].x &&\
	 ex <= scr->items[area].x + scr->items[area].w)

#ifndef VERBOSE
#define dump_coords(scr, x) ;
#else
static void dump_coords(struct screen *scr, int x)
{
	int i;

	for (i = 0; i < PANEL_AREA_MAX; i++) {
		ii("%d: %d >= %d <= %d (w = %d)\n", i, scr->items[i].x, x,
		   scr->items[i].w + scr->items[i].x, scr->items[i].w);
		if pointer_inside(scr, i, x)
			ii("inside element %d\n", i);
	}
}
#endif

static void handle_panel_press(xcb_button_press_event_t *e)
{
	curscr = coord2screen(e->root_x, e->root_y);

	ii("screen %d, press at %d,%d\n", curscr->id, e->event_x, e->event_y);

	dump_coords(curscr, e->event_x);
	if pointer_inside(curscr, PANEL_AREA_TAGS, e->event_x) {
		select_tag(curscr, e->event_x);
	} else if pointer_inside(curscr, PANEL_AREA_MENU, e->event_x) {
		struct list_head *cur;
		ii("menu, tag %s\n", curscr->tag->name);
		list_walk(cur, &curscr->tag->clients)
			ii("  win %p, geo %ux%u+%d+%d\n",
			   list2client(cur)->win, list2client(cur)->w,
			   list2client(cur)->h, list2client(cur)->x,
			   list2client(cur)->y);
	} else if pointer_inside(curscr, PANEL_AREA_TITLE, e->event_x) {
		ii("title\n");
	} else if pointer_inside(curscr, PANEL_AREA_DOCK, e->event_x) {
		ii("dock\n");
	} else if (pointer_inside(curscr, PANEL_AREA_TIME, e->event_x)) {
		ii("clock\n");
		if (curscr->items[PANEL_AREA_TIME].action) {
			void *arg = curscr->items[PANEL_AREA_TIME].arg;
			curscr->items[PANEL_AREA_TIME].action(arg);
		}
	}
}

static void handle_button_release(xcb_button_release_event_t *e)
{
	mouse_x = mouse_y = mouse_button = 0;
	xcb_ungrab_pointer(dpy, XCB_CURRENT_TIME);
	xcb_flush(dpy);
}

static void handle_button_press(xcb_button_press_event_t *e)
{
	te("XCB_BUTTON_PRESS: root %p, pos %d,%d; event %p, pos %d,%d; "
	   "child %p\n", e->root, e->root_x, e->root_y, e->event, e->event_x,
	   e->event_y, e->child);

	curscr = coord2screen(e->root_x, e->root_y);
	trace_screen_metrics(curscr);

	switch (e->detail) {
	case MOUSE_BTN_LEFT:
		dd("MOUSE_BTN_LEFT\n");
		if (curscr && curscr->panel == e->event) {
			handle_panel_press(e);
			return;
		} else if (curscr) {
			print_title(curscr, e->event);
		}
		break;
	case MOUSE_BTN_MID:
		dd("MOUSE_BTN_MID\n");
		break;
	case MOUSE_BTN_RIGHT:
		dd("MOUSE_BTN_RIGHT\n");
		panel_items_stat(curscr);
		break;
	default:
		break;
	}

	/* prepare for motion event handling */

	if (e->event != e->root || !e->child)
		return;

	window_raise(e->child);
	panel_raise(curscr);

	/* subscribe to motion events */

	xcb_grab_pointer(dpy, 0, e->root,
			 XCB_EVENT_MASK_BUTTON_MOTION |
			 XCB_EVENT_MASK_BUTTON_RELEASE,
			 XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
			 e->root, XCB_NONE, XCB_CURRENT_TIME);
	xcb_flush(dpy);
}

static void handle_motion_notify(xcb_motion_notify_event_t *e)
{
	uint16_t mask;
	uint32_t val[2];
	int16_t dx, dy;
	struct client *cli;

	curscr = coord2screen(e->root_x, e->root_y);
	if (!curscr)
		return;

	trace_screen_metrics(curscr);
	if (curscr && curscr->panel == e->child) {
		return;
	} else if (!e->child) {
		handle_button_release(NULL);
		return;
	}

	/* window is being moved so search in global list */
	cli = win2client(e->child);
	if (!cli) {
		ww("window %p is not managed\n", e->child);
		return;
	}

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

	ii("client's screen %d\n", cli->scr->id);
	if (cli->scr != curscr || curscr->tag->win != cli->win) { /* retag */
		{
		struct client *tmp = scr2client(cli->scr, e->child, WIN_TYPE_NORMAL);
		if (!tmp)
			ee("client for win %p not found\n", e->child);
		}
		list_del(&cli->head);
		list_add(&curscr->tag->clients, &cli->head);
		cli->scr = curscr;
		curscr->tag->win = cli->win;
		ii("win %p now on tag %s screen %d\n", e->child,
		   curscr->tag->name, curscr->id);
		{
		struct client *tmp = scr2client(curscr, e->child, WIN_TYPE_NORMAL);
		if (!tmp)
			ee("client for win %p not found\n", e->child);
		}
	}
}

static void handle_key_press(xcb_key_press_event_t *e)
{
	struct keymap *ptr = kmap;

	curscr = coord2screen(e->root_x, e->root_y);

	ii("screen %d, key %p, state %p, pos %d,%d\n", curscr->id, e->detail,
	   e->state, e->root_x, e->root_y);

	while (ptr->mod) {
		if (ptr->key == e->detail && ptr->mod == e->state) {
			ii("%p pressed\n", ptr->key);
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
			panel_raise(scr);
			redraw_panel_items(scr);
			return;
		}
	}
}

#if 0
static void handle_create_notify(xcb_create_notify_event_t *e)
{
}
#endif

static void handle_unmap_notify(xcb_unmap_notify_event_t *e)
{
	int16_t x, y;

	if (window_status(e->window) == WIN_STATUS_UNKNOWN) {
		dd("window is gone %p\n", e->window);
		client_del(e->window);
		return;
	}
}

static void handle_enter_notify(xcb_enter_notify_event_t *e)
{
	struct list_head *cur;
	struct client *cli;
	struct screen *scr;

	scr = coord2screen(e->root_x, e->root_y);

	tt("cur screen %d, tag %s, win %p ? %p\n", curscr->id, curscr->tag->name,
	   curscr->tag->win, e->event);

	if (curscr && curscr->panel == e->event)
		return;
	else if (curscr && curscr != scr)
		update_panel_title(e->event);

	curscr = scr;

	if (curscr->tag->win)
		window_focus(curscr, curscr->tag->win, 0);

	window_focus(curscr, e->event, 1);

	if (e->mode == MODKEY)
		window_raise(e->event);

	xcb_flush(dpy);
}

static void handle_property_notify(xcb_property_notify_event_t *e)
{
	ii("XCB_PROPERTY_NOTIFY: win %p, atom %d\n", e->window, e->atom);

	switch (((xcb_property_notify_event_t *) e)->atom) {
	case XCB_ATOM_WM_NAME:
		ii("screen %d\n", curscr ? curscr->id : -1);
		if (curscr)
			print_title(curscr, e->window);
		break;
	}
}

#if 0
static int override_redirect(xcb_window_t win)
{
	xcb_get_window_attributes_cookie_t c;
	xcb_get_window_attributes_reply_t *a;
	int rc;

	c = xcb_get_window_attributes(dpy, win);
	a = xcb_get_window_attributes_reply(dpy, c, NULL);
	if (!a) {
		ee("xcb_get_window_attributes() failed\n");
		return;
	}

	rc = a->override_redirect;
	free(a);
	return rc;
}

static void handle_configure_notify(xcb_configure_notify_event_t *e)
{
	struct screen *scr;

	if (override_redirect(e->window)) {
		ii("override redirect window %p\n", e->window);
		return;
	}

	scr = root2screen(e->event);
	if (!scr)
		scr = curscr;

	ii("win %p above %p\n", e->window, e->above_sibling);
	if (e->above_sibling) {
		struct client *cli = win2client(scr, e->above_sibling, 0);
		if (!cli) {
			ii("window %p is not managed\n", e->above_sibling);
			if (override_redirect(e->above_sibling)) {
				ii("override redirect window %p\n", e->above_sibling);
			}
		}
	}
}
#endif

#if 0
static void print_atom_name(xcb_atom_t atom)
{
        char *name;
        xcb_get_atom_name_reply_t *r;
	xcb_get_atom_name_cookie_t c;

	c = xcb_get_atom_name(dpy, atom);
        r = xcb_get_atom_name_reply(dpy, c, NULL);
        if (r) {
                if (xcb_get_atom_name_name_length(r) > 0) {
			name = xcb_get_atom_name_name(r);
			tt("atom %d, name %s\n", atom, name);
		}
                free(r);
        }
}

#define SYSTEM_TRAY_REQUEST_DOCK 0
#define SYSTEM_TRAY_BEGIN_MESSAGE 1
#define SYSTEM_TRAY_CANCEL_MESSAGE 2

static void handle_client_message(xcb_client_message_event_t *e)
{
	print_atom_name(e->type);

	ii("win %p, action %d\n", e->window, e->data.data32[0]);
	print_atom_name(e->data.data32[1]);

	if (e->type == a_systray && e->format == 32 &&
	    e->data.data32[1] == SYSTEM_TRAY_REQUEST_DOCK) {
		xcb_window_t win = e->data.data32[2];
		ii("win %x\n", win);
	}
}
#endif

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

static void handle_configure_request(xcb_configure_request_event_t *e)
{
	uint32_t val[7] = { 0 };
	int i = 0;
	uint16_t mask = 0;

	/* the order has to correspond to the order value_mask bits */
	if (e->value_mask & XCB_CONFIG_WINDOW_X) {
		val[i++] = e->x;
		mask |= XCB_CONFIG_WINDOW_X;
	}
	if (e->value_mask & XCB_CONFIG_WINDOW_Y) {
		val[i++] = e->y;
		mask |= XCB_CONFIG_WINDOW_Y;
	}
	if (e->value_mask & XCB_CONFIG_WINDOW_WIDTH) {
		val[i++] = e->width;
		mask |= XCB_CONFIG_WINDOW_WIDTH;
	}
	if (e->value_mask & XCB_CONFIG_WINDOW_HEIGHT) {
		val[i++] = e->height;
		mask |= XCB_CONFIG_WINDOW_HEIGHT;
	}
#if 0
	if (e->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) {
		val[i++] = BORDER_WIDTH; //e->border_width;
		mask |= XCB_CONFIG_WINDOW_BORDER_WIDTH;
	}
#endif
	if (e->value_mask & XCB_CONFIG_WINDOW_SIBLING) {
		val[i++] = e->sibling;
		mask |= XCB_CONFIG_WINDOW_SIBLING;
	}
	if (e->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) {
		val[i++] = e->stack_mode;
		mask |= XCB_CONFIG_WINDOW_STACK_MODE;
	}

        xcb_configure_window(dpy, e->window, mask, val);
        xcb_flush(dpy);
}

static int handle_events(void)
{
	xcb_generic_event_t *e;
	uint8_t type;

	e = xcb_poll_for_event(dpy);

	if (xcb_connection_has_error(dpy))
		panic("failed to get event\n");

	if (!e)
		return 0;

	mm("got event %d (%d)\n", e->response_type, XCB_EVENT_RESPONSE_TYPE(e));

	switch (e->response_type & ~0x80) {
	case 0: break; /* NO EVENT */
	case XCB_VISIBILITY_NOTIFY:
		te("XCB_VISIBILITY_NOTIFY: win %p\n",
		   ((xcb_visibility_notify_event_t *) e)->window);
                switch (((xcb_visibility_notify_event_t *) e)->state) {
                case XCB_VISIBILITY_FULLY_OBSCURED:
                case XCB_VISIBILITY_PARTIALLY_OBSCURED: /* fall through */
			handle_visibility((xcb_visibility_notify_event_t *) e);
			break;
                }
		break;
	case XCB_CREATE_NOTIFY:
		te("XCB_CREATE_NOTIFY: parent %p, window %p\n",
		   ((xcb_create_notify_event_t *) e)->parent,
		   ((xcb_create_notify_event_t *) e)->window);
#if 0
		handle_create_notify((xcb_create_notify_event_t *) e);
#endif
		break;
	case XCB_BUTTON_PRESS:
		mouse_x = ((xcb_button_press_event_t *) e)->root_x;
		mouse_y = ((xcb_button_press_event_t *) e)->root_y;
		mouse_button = ((xcb_button_press_event_t *) e)->detail;
		handle_button_press((xcb_button_press_event_t *) e);
		break;
	case XCB_BUTTON_RELEASE:
		te("XCB_BUTTON_RELEASE: pos %d,%d, event %p, child %p\n",
		   ((xcb_button_press_event_t *) e)->root_x,
		   ((xcb_button_press_event_t *) e)->root_y,
		   ((xcb_button_press_event_t *) e)->event,
		   ((xcb_button_press_event_t *) e)->child);
		handle_button_release((xcb_button_release_event_t *) e);
		break;
	case XCB_MOTION_NOTIFY:
		mm("XCB_MOTION_NOTIFY\n");
		handle_motion_notify((xcb_motion_notify_event_t *) e);
		break;
	case XCB_CLIENT_MESSAGE:
		te("XCB_CLIENT_MESSAGE: win %p, type %d\n",
		   ((xcb_client_message_event_t *) e)->window,
		   ((xcb_client_message_event_t *) e)->type);
#if 0
		handle_client_message((xcb_client_message_event_t *) e);
#endif
		break;
	case XCB_CONFIGURE_REQUEST:
		te("XCB_CONFIGURE_REQUEST: win %p\n",
		   ((xcb_configure_request_event_t *) e)->window);
		print_configure_request((xcb_configure_request_event_t *) e);
		handle_configure_request((xcb_configure_request_event_t *) e);
		break;
	case XCB_CONFIGURE_NOTIFY:
		mm("XCB_CONFIGURE_NOTIFY: event %p, window %p, above %p\n",
		   ((xcb_configure_notify_event_t *) e)->event,
		   ((xcb_configure_notify_event_t *) e)->window,
		   ((xcb_configure_notify_event_t *) e)->above_sibling);
#if 0
		handle_configure_notify((xcb_configure_notify_event_t *) e);
#endif
		break;
	case XCB_DESTROY_NOTIFY:
		te("XCB_DESTROY_NOTIFY: event %p, win %p\n",
		   ((xcb_destroy_notify_event_t *) e)->event,
		   ((xcb_destroy_notify_event_t *) e)->window);
		client_del(((xcb_destroy_notify_event_t *) e)->window);
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
		handle_property_notify((xcb_property_notify_event_t *) e);
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
	return 1;
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
	xcb_get_modifier_mapping_cookie_t c;
	xcb_get_modifier_mapping_reply_t *r;
	xcb_keycode_t *mod;
	xcb_key_symbols_t *syms;
	xcb_keycode_t *key;
	struct keymap *ptr;
	int i;

	c = xcb_get_modifier_mapping(dpy);
	r = xcb_get_modifier_mapping_reply(dpy, c, NULL);
	if (!r)
		panic("xcb_get_modifier_mapping_reply() failed\n");

	mod = xcb_get_modifier_mapping_keycodes(r);
	if (!mod)
		panic("\nxcb_get_modifier_mapping_keycodes() failed\n");

#ifdef DEBUG
	for (i = 0; i < r->keycodes_per_modifier; i++)
		dd("%d: key code %x ? %x\n", i, mod[i], SHIFT);
#endif

	free(r);

	xcb_ungrab_key(dpy, XCB_GRAB_ANY, rootscr->root, XCB_MOD_MASK_ANY);

	syms = xcb_key_symbols_alloc(dpy);
	if (!syms)
		panic("xcb_key_symbols_alloc() failed\n");

	ptr = kmap;
	while (ptr->mod) {
		key = xcb_key_symbols_get_keycode(syms, ptr->sym);
		if (!key)
			panic("xcb_key_symbols_get_keycode(sym=%p) failed\n",
			      ptr->sym);
		ptr->key = *key;
		xcb_grab_key(dpy, 1, rootscr->root, ptr->mod, ptr->key,
			     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
		dd("grab mod %p + key %p (%p)\n", ptr->mod, ptr->key, ptr->sym);
		ptr++;
	}

	xcb_key_symbols_free(syms);
}

static void screen_add(uint8_t id, int16_t x, int16_t y, uint16_t w, uint16_t h)
{
	struct screen *scr;

	scr = calloc(1, sizeof(*scr));
	if (!scr)
		panic("calloc(%lu) failed\n", sizeof(*scr));

	scr->id = id;
	scr->x = x;
	scr->y = y;
	scr->w = w;
	scr->h = h;

	list_init(&scr->dock);
	list_init(&scr->tags);
	init_panel(scr);

	if (!list_empty(&scr->tags)) {
		ii("current tag %p %s\n", list2tag(scr->tags.next),
			(list2tag(scr->tags.next))->name);
	}

	list_add(&screens, &scr->head);

	if (scr->x == 0)
		defscr = scr; /* make such screen default */

	ii("screen %d (%p), size %dx%d+%d+%d, %u tags\n", id, scr, scr->w,
	   scr->h, scr->x, scr->y, scr->ntags);
}

static void init_crtc(uint8_t id, xcb_randr_get_output_info_reply_t *out,
		      xcb_timestamp_t ts)
{
	xcb_randr_get_crtc_info_cookie_t c;
	xcb_randr_get_crtc_info_reply_t *r;

	c = xcb_randr_get_crtc_info(dpy, out->crtc, ts);
	r = xcb_randr_get_crtc_info_reply(dpy, c, NULL);
	if (!r)
		return;

	ii("crtc%d geo %ux%u+%d+%d\n", id, r->width, r->height, r->x, r->y);
	/* one screen per output; share same root window via common
	 * xcb_screen_t structure
	 */
	screen_add(id, r->x, r->y, r->width, r->height);

	free(r);
}

static void init_output(int id, xcb_randr_output_t out, xcb_timestamp_t ts)
{
	char name[1024];
	int len;
	xcb_randr_get_output_info_cookie_t c;
	xcb_randr_get_output_info_reply_t *r;

	c = xcb_randr_get_output_info(dpy, out, ts);
	r = xcb_randr_get_output_info_reply(dpy, c, NULL);
	if (!r)
		return;

	len = xcb_randr_get_output_info_name_length(r);
	if (len > sizeof(name))
		len = sizeof(name);
	snprintf(name, len, "%s", xcb_randr_get_output_info_name(r));
	ii("output %s%d, %dx%d\n", name, id, r->mm_width, r->mm_height);
	ii("connection %d\n", r->connection);

	if (r->connection != XCB_RANDR_CONNECTION_CONNECTED) {
		ii("not connected\n");
		goto out;
	}

	init_crtc(id, r, ts);
out:
	free(r);
}

static void init_outputs(void)
{
	xcb_randr_get_screen_resources_current_cookie_t c;
	xcb_randr_get_screen_resources_current_reply_t *r;
	xcb_randr_output_t *out;
	int i, len;

	list_init(&screens);

	c = xcb_randr_get_screen_resources_current(dpy, rootscr->root);
	r = xcb_randr_get_screen_resources_current_reply(dpy, c, NULL);
	if (!r) {
		ii("RandR extension is not present\n");
		return;
	}

	len = xcb_randr_get_screen_resources_current_outputs_length(r);
	out = xcb_randr_get_screen_resources_current_outputs(r);
	ii("found %d screens\n", len);

	/* FIXME: need to implement clone detection */
	for (i = 0; i < len; i++)
		init_output(i, out[i], r->config_timestamp);

	free(r);
}

static void init_rootwin(void)
{
	xcb_window_t win = rootscr->root;
	xcb_void_cookie_t c;
	uint32_t val = XCB_EVENT_MASK_BUTTON_PRESS;

	xcb_eval(c, xcb_grab_button(dpy, 0, win, val, XCB_GRAB_MODE_ASYNC,
		 XCB_GRAB_MODE_ASYNC, win, XCB_NONE, MOUSE_BTN_LEFT, MODKEY));
	xcb_eval(c, xcb_grab_button(dpy, 0, win, val, XCB_GRAB_MODE_ASYNC,
		 XCB_GRAB_MODE_ASYNC, win, XCB_NONE, MOUSE_BTN_MID, MODKEY));
	xcb_eval(c, xcb_grab_button(dpy, 0, win, val, XCB_GRAB_MODE_ASYNC,
		 XCB_GRAB_MODE_ASYNC, win, XCB_NONE, MOUSE_BTN_RIGHT, MODKEY));

	/* subscribe events */
	val = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
	      XCB_EVENT_MASK_STRUCTURE_NOTIFY |
	      XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
	xcb_change_window_attributes(dpy, win, XCB_CW_EVENT_MASK, &val);
	xcb_flush(dpy);
}

int main()
{
	struct pollfd pfd;
	const char *logfile;

	logfile = getenv("YAWM_LOG");
	if (logfile) {
		if (!freopen(logfile, "a+", stdout)) {
			ee("failed to reopen %s as stdout\n", logfile);
		} else {
			if (!freopen(logfile, "a+", stderr)) {
				ee("failed to reopen %s as stderr\n", logfile);
			}
		}
		ii("logfile: %s\n", logfile);
	}

	basedir = getenv("YAWM_HOME");
	if (!basedir)
		basedir = "/tmp/yawm";

	ii("basedir: %s\n", basedir);

	if (signal(SIGCHLD, spawn_cleanup) == SIG_ERR)
		panic("SIGCHLD handler failed\n");

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

	rootscr = xcb_setup_roots_iterator(xcb_get_setup(dpy)).data;
	if (!rootscr)
		panic("no screens found\n");

	ii("root %p, size %dx%d\n", rootscr->root, rootscr->width_in_pixels,
	   rootscr->height_in_pixels);

	init_keys();
	init_rootwin();
	init_outputs();

	if (list_empty(&screens)) { /*randr failed or not supported */
		screen_add(0, 0, 0, rootscr->width_in_pixels,
			   rootscr->height_in_pixels);
	}

	if (!defscr)
		defscr = list2screen(screens.next); /* use first one */
	if (!curscr)
		curscr = defscr;

	trace_screens();

	a_name = atom_by_name("_NEW_WM_NAME");
	a_state = atom_by_name("WM_STATE");
	a_desktop = atom_by_name("_NET_WM_DESKTOP");
	a_client_list = atom_by_name("_NET_CLIENT_LIST");
	a_systray = atom_by_name("_NET_SYSTEM_TRAY_OPCODE");

	strncpy(tray_class, "yawmtray", sizeof(tray_class) - 1);
	strncpy(dock_class, "yawmdock", sizeof(tray_class) - 1);
	ii("default tray class: %s, dock class: %s\n", tray_class, dock_class);

	clients_scan();

	pfd.fd = xcb_get_file_descriptor(dpy);
	pfd.events = POLLIN;
	pfd.revents = 0;

	ii("defscr %d, curscr %d\n", defscr->id, curscr->id);
	ii("enter events loop\n");

	while (1) {
		int rc = poll(&pfd, 1, TIME_REFRESH_INTERVAL);
		if (rc == 0) { /* timeout */
			print_time(defscr, 0);
		} else if (rc < 0) {
			if (errno == EINTR)
				continue;
			/* something weird happened, but relax and try again */
			sleep(1);
			continue;
		}

		if (pfd.revents & POLLIN)
			while (handle_events()) {} /* read all events */
		if (logfile) {
			fflush(stdout);
			fflush(stderr);
		}
	}

	xcb_set_input_focus(dpy, XCB_NONE, XCB_INPUT_FOCUS_POINTER_ROOT,
			    XCB_CURRENT_TIME);
	xcb_flush(dpy);

	clean();
	return 0;
}
