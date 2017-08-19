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
#include <sys/wait.h>

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

#define USE_CRC32 /* use crc32 function from misc header */

#include "misc.h"
#include "yawm.h"
#include "list.h"

/* defines */

#ifndef TAG_LONG_PRESS
#define TAG_LONG_PRESS  300 /* ms */
#endif

#ifndef _NET_WM_OPAQUE_REGION
#define _NET_WM_OPAQUE_REGION 355
#endif

#define slen(str) (sizeof(str) - 1)

typedef uint8_t strlen_t;

#define PANEL_SCREEN_GAP 0
#define ITEM_V_MARGIN 2
#define ITEM_H_MARGIN 6
#define TOOLBAR_ITEM_XPAD 2
#define TAG_GAP 2

#define BORDER_WIDTH 1
#define WINDOW_PAD 0

#ifndef WIN_WIDTH_MIN
#define WIN_WIDTH_MIN 10
#endif

#ifndef WIN_HEIGHT_MIN
#define WIN_HEIGHT_MIN 10
#endif

#define CLI_FLG_DOCK (1 << 0)
#define CLI_FLG_TRAY (1 << 1)
#define CLI_FLG_BORDER (1 << 2)
#define CLI_FLG_CENTER (1 << 3)
#define CLI_FLG_TOPLEFT (1 << 4)
#define CLI_FLG_TOPRIGHT (1 << 5)
#define CLI_FLG_BOTLEFT (1 << 6)
#define CLI_FLG_BOTRIGHT (1 << 7)
#define CLI_FLG_EXCLUSIVE (1 << 8)
#define CLI_FLG_MOVE (1 << 9)
#define CLI_FLG_FULLSCREEN (1 << 10)
#define CLI_FLG_POPUP (1 << 11)
#define CLI_FLG_IGNORED (1 << 12)

#define SCR_FLG_SWITCH_WINDOW (1 << 1)
#define SCR_FLG_SWITCH_WINDOW_NOWARP (1 << 2)

#define TAG_NAME_MAX 32

#define MOUSE_BTN_LEFT 1
#define MOUSE_BTN_MID 2
#define MOUSE_BTN_RIGHT 3

#define DIV_ICON "::"

#define ALT XCB_MOD_MASK_1
#ifndef MOD
#define MOD XCB_MOD_MASK_1
#endif
#define SHIFT XCB_MOD_MASK_SHIFT
#define CTRL XCB_MOD_MASK_CONTROL

#define ITEM_FLG_NORMAL (1 << 0)
#define ITEM_FLG_FOCUSED (1 << 1)
#define ITEM_FLG_ACTIVE (1 << 2)
#define ITEM_FLG_ALERT (1 << 3)
#define ITEM_FLG_LOCKED (1 << 4)

/* data structures */

struct color {
	const char *fname;
	void *val;
	uint32_t def;
	uint8_t type;
};

struct sprop { /* string property */
	char *str;
	strlen_t len;
	xcb_get_property_reply_t *ptr; /* to free */
};

enum panel_area { /* in order of appearance */
	PANEL_AREA_MENU,
	PANEL_AREA_TAGS,
	PANEL_AREA_DIV,
	PANEL_AREA_TITLE,
	PANEL_AREA_DOCK,
	PANEL_AREA_MAX,
};

struct panel_item {
	int16_t x;
	uint16_t w;
	void (*action)(void *);
	void *arg;
};

struct panel {
	int16_t y;
	uint16_t w;
	uint16_t h;
	xcb_gcontext_t gc;
	xcb_drawable_t win;
	XftDraw *draw;
};

//#define LEFT_BTN ""
//#define RIGHT_BTN ""
//#define TOP_BTN ""
//#define BOTTOM_BTN ""
//#define CENTER_BTN ""

#define LEFT_BTN ""
#define RIGHT_BTN ""
#define TOP_BTN ""
#define BOTTOM_BTN ""

#define BTN_MOUSE ""
#define BTN_CLOSE ""
#define BTN_MOVE ""
#define BTN_RESIZE ""
#define BTN_GRID ""
#define BTN_LEFT ""
#define BTN_RIGHT ""
#define BTN_TOP ""
#define BTN_BOTTOM ""
#define BTN_CENTER ""
#define BTN_EXPAND ""
//#define BTN_PIN ""
#define BTN_FLAG ""
#define BTN_TOOLS ""

struct toolbar_item {
	const char *str;
	uint8_t len;
	uint16_t x;
	uint16_t w;
	uint8_t flags;
};

static struct toolbar_item toolbar_items[] = {
	{ BTN_MOUSE, slen(BTN_MOUSE), },
	{ BTN_CLOSE, slen(BTN_CLOSE), },
	{ BTN_MOVE, slen(BTN_MOVE), },
	{ BTN_CENTER, slen(BTN_CENTER), },
	{ BTN_LEFT, slen(BTN_LEFT), },
	{ BTN_RIGHT, slen(BTN_RIGHT), },
	{ BTN_TOP, slen(BTN_TOP), },
	{ BTN_BOTTOM, slen(BTN_BOTTOM), },
	{ BTN_EXPAND, slen(BTN_EXPAND), },
	{ BTN_FLAG, slen(BTN_FLAG), },
};

struct toolbar {
	int16_t x;
	int16_t y;
	struct panel panel;
	struct screen *scr;
	struct client *cli;
	xcb_keycode_t knext; /* item's navigation */
	xcb_keycode_t kprev;
	xcb_keycode_t kenter;
	xcb_keycode_t kclose; /* hide toolbar */
	uint16_t title_x;
};

struct toolbox {
	xcb_drawable_t win;
	xcb_gcontext_t gc;
	XftDraw *draw;
	struct client *cli;
	uint8_t skip;
	uint8_t size;
};

static struct toolbox toolbox;

static uint16_t text_yoffs;

struct rect {
	int16_t x;
	int16_t y;
	uint16_t w;
	uint16_t h;
};

struct screen { /* per output abstraction */
	uint8_t id;
	xcb_randr_output_t out;
	char *name;

	struct list_head head;
	struct list_head tags;
	struct list_head dock;

	struct tag *tag; /* current tag */

	int16_t top; /* y position relative to panel */
	int16_t x, y;
	uint16_t w, h;

	uint8_t flags; /* SCR_FLG */

	struct panel panel; /* screen panel */
	struct panel_item items[PANEL_AREA_MAX];
};

#define list2screen(item) list_entry(item, struct screen, head)

struct list_head screens;
struct screen *curscr;
struct screen *defscr;
static xcb_screen_t *rootscr; /* root window details */
static struct toolbar toolbar; /* window toolbar */
static uint8_t focus_root_;
static struct client *motion_cli;
static int16_t motion_init_x;
static int16_t motion_init_y;

struct tag {
	struct list_head head;
	struct list_head clients;
	xcb_window_t visited; /* last entered window */
	uint8_t id;
	int16_t x;
	uint16_t w;
	char *name; /* visible name */
	strlen_t nlen; /* name length */
	struct rect space;
	uint8_t grid2v; /* toggle vertical/horizontal split of 2-cells grid */
	struct client *anchor;
	uint8_t flags;
};

#define list2tag(item) list_entry(item, struct tag, head)

#ifndef POS_DIV_MAX
#define POS_DIV_MAX 9
#endif

struct client {
	struct list_head head; /* local list */
	struct list_head list; /* global list */
	int16_t x, y;
	uint16_t w, h;
	uint8_t div; /* for position calculation */
	xcb_window_t win;
	pid_t pid;
	struct screen *scr;
	struct tag *tag;
	uint16_t flags;
	uint32_t crc; /* based on class name */
	uint8_t busy;
	uint8_t pos; /* enum winpos */
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

static uint8_t last_winpos;

enum dir {
	DIR_NEXT = 1,
	DIR_PREV,
};

static void walk_tags(void *);
static void retag_client(void *);
static void next_window(void *);
static void prev_window(void *);
static void raise_client(void *);
static void place_window(void *);
static void make_grid(void *);
static void toggle_toolbar(void *);
static void flag_window(void *);

struct keymap {
	uint16_t mod;
	xcb_keysym_t sym;
	xcb_keycode_t key;
	char *keyname;
	const char *actname;
	void (*action)(void *);
	uint32_t arg;
	struct list_head head;
	uint16_t alloc;
};

/* built-in default actions */
static struct keymap kmap_def[] = {
	{ MOD, XK_Tab, 0, "mod_tab", "next window",
	  next_window, },
	{ MOD, XK_BackSpace, 0, "mod_backspace", "prev window",
	  prev_window, },
	{ MOD, XK_Return, 0, "mod_return", "raise client",
	  raise_client, },
	{ MOD, XK_Home, 0, "mod_home", "retag next",
	  retag_client, DIR_NEXT, },
	{ MOD, XK_End, 0, "mod_end", "retag prev",
	  retag_client, DIR_PREV, },
	{ MOD, XK_Page_Up, 0, "mod_pageup", "next tag",
	  walk_tags, DIR_NEXT, },
	{ MOD, XK_Page_Down, 0, "mod_pagedown", "prev tag",
	  walk_tags, DIR_PREV, },
	{ SHIFT, XK_F5, 0, "shift_f5", "top-left",
	  place_window, WIN_POS_TOP_LEFT, },
	{ SHIFT, XK_F6, 0, "shift_f6", "top-right",
	  place_window, WIN_POS_TOP_RIGHT, },
	{ SHIFT, XK_F7, 0, "shift_f7", "bottom-left",
	  place_window, WIN_POS_BOTTOM_LEFT, },
	{ SHIFT, XK_F8, 0, "shift_f8", "bottom-right",
	  place_window, WIN_POS_BOTTOM_RIGHT, },
	{ SHIFT, XK_F10, 0, "shift_f10", "center",
	  place_window, WIN_POS_CENTER, },
	{ MOD, XK_F5, 0, "mod_f5", "fill left",
	  place_window, WIN_POS_LEFT_FILL, },
	{ MOD, XK_F6, 0, "mod_f6", "fill right",
	  place_window, WIN_POS_RIGHT_FILL, },
	{ MOD, XK_F7, 0, "mod_f7", "fill top",
	  place_window, WIN_POS_TOP_FILL, },
	{ MOD, XK_F8, 0, "mod_f8", "fill bottom",
	  place_window, WIN_POS_BOTTOM_FILL, },
	{ MOD, XK_F9, 0, "mod_f9", "full screen",
	  place_window, WIN_POS_FILL, },
	{ MOD, XK_F3, 0, "mod_f3", "make grid",
	  make_grid, },
	{ MOD, XK_F4, 0, "mod_f4", "toggle toolbar",
	  toggle_toolbar, },
	{ MOD, XK_F2, 0, "mod_f2", "flag window",
	  flag_window, },
};

#define list2keymap(item) list_entry(item, struct keymap, head)

static struct list_head keymap;

struct arg {
	int16_t x;
	int16_t y;
	uint32_t data;
	struct keymap *kmap;
};

/* defaults */

enum colortype {
	COLOR_TYPE_INT,
	COLOR_TYPE_XFT,
};

enum coloridx {
	NORMAL_FG,
	NORMAL_BG,
	ACTIVE_FG,
	ACTIVE_BG,
	ALERT_FG,
	ALERT_BG,
	NOTICE_FG,
	NOTICE_BG,
};

static XftColor normal_fg;
static uint32_t normal_bg;
static XftColor active_fg;
static uint32_t active_bg;
static XftColor alert_fg;
static uint32_t alert_bg;
static XftColor notice_fg;
static uint32_t notice_bg;

static struct color defcolors[] = {
	{ "normal-fg", &normal_fg, 0xa0a0a0, COLOR_TYPE_XFT, },
	{ "normal-bg", &normal_bg, 0x202020, COLOR_TYPE_INT, },
	{ "active-fg", &active_fg, 0xc0c0c0, COLOR_TYPE_XFT, },
	{ "active-bg", &active_bg, 0x41749c, COLOR_TYPE_INT, },
	{ "alert-fg", &alert_fg, 0xc32d2d, COLOR_TYPE_XFT, },
	{ "alert-bg", &alert_bg, 0x90ae2b, COLOR_TYPE_INT, },
	{ "notice-fg", &notice_fg, 0x101010, COLOR_TYPE_XFT, },
	{ "notice-bg", &notice_bg, 0x90ae2b, COLOR_TYPE_INT, },
	{ NULL, NULL, 0, 0, },
};

#define color2int(idx) *((uint32_t *) defcolors[idx].val)
#define color2xft(idx) *((XftColor *) defcolors[idx].val)
#define color2ptr(idx) &defcolors[idx]

#define MENU_ICON_DEF "="
#define MENU_ICON_DOWN ""
#define MENU_ICON_UP ""
//#define MENU_ICON_DEF " "

static XftFont *menu_font;
static char *menu_icon = MENU_ICON_DEF;
static uint8_t menu_icon_len = sizeof(MENU_ICON_DEF) - 1;
static uint16_t panel_height;
static uint8_t panel_top;
static xcb_timestamp_t tag_time;

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
	WIN_STATUS_MAX,
};

static XftFont *font1;
static XftFont *font2;

static int xscr;
static Display *xdpy;
static xcb_connection_t *dpy;

static xcb_atom_t a_state;
static xcb_atom_t a_client_list;
static xcb_atom_t a_systray;
static xcb_atom_t a_active_win;
static xcb_atom_t a_has_vt;
static xcb_atom_t a_embed_info;
static xcb_atom_t a_net_wm_name;
static xcb_atom_t a_net_wm_pid;
static xcb_atom_t a_leader;
static xcb_atom_t a_protocols;
static xcb_atom_t a_delete_win;
static xcb_atom_t a_maximize_win;
static xcb_atom_t a_net_wm_state;
static xcb_atom_t a_fullscreen;
static xcb_atom_t a_hidden;
static xcb_atom_t a_usertime;
static xcb_atom_t a_ping;

static strlen_t actname_max = UCHAR_MAX - 1;

static uint8_t baselen;
static char *basedir;
static char *homedir;
static struct toolbar_item *focused_item;

static uint8_t randrbase;

/* ... and the mess begins */

static void get_sprop(struct sprop *ret, xcb_window_t win,
		      enum xcb_atom_enum_t atom, uint32_t len)
{
	xcb_get_property_cookie_t c;

	c = xcb_get_property(dpy, 0, win, atom, XCB_GET_PROPERTY_TYPE_ANY, 0, len);
	ret->ptr = xcb_get_property_reply(dpy, c, NULL);
	if (!ret->ptr) {
		ret->str = NULL;
		ret->len = 0;
	} else {
		ret->str = xcb_get_property_value(ret->ptr);
		ret->len = xcb_get_property_value_length(ret->ptr);
	}
}

static pid_t win2pid(xcb_window_t win)
{
	pid_t pid;
	struct sprop prop;

	get_sprop(&prop, win, a_net_wm_pid, 1);
	if (!prop.ptr)
		return 0;

	pid = 0;
	if (prop.ptr->type == XCB_ATOM_CARDINAL && prop.ptr->format == 32)
		pid = *((pid_t *) xcb_get_property_value(prop.ptr));

	free(prop.ptr);
	return pid;
}

static struct client *win2cli(xcb_window_t win)
{
	struct list_head *cur;

	list_walk(cur, &clients) {
		struct client *cli = glob2client(cur);
		if (cli->win == win)
			return cli;
	}

	return NULL;
}

static struct client *front_client(struct tag *tag)
{
	if (list_empty(&tag->clients))
		return NULL;
	else if (list_single(&tag->clients))
		return list2client(tag->clients.next);

	return list2client(tag->clients.prev);
}

#define trace_tag_windows(tag) {\
	struct list_head *cur__;\
	struct client *front__ = front_client(tag);\
	ii("tag %s, clients %p | %s\n", tag->name, &tag->clients, __func__);\
	list_walk(cur__, &tag->clients) {\
		struct client *tmp__ = list2client(cur__);\
		if (tmp__ == front__)\
			ii("  cli %p win 0x%x *\n", tmp__, tmp__->win);\
		else\
			ii("  cli %p win 0x%x\n", tmp__, tmp__->win);\
	}\
}

static void text_exts(const char *text, int len, uint16_t *w, uint16_t *h,
		      XftFont *font)
{
	XGlyphInfo ext;

	XftTextExtentsUtf8(xdpy, font, (XftChar8 *) text, len, &ext);

	tt("text: %s\n  x = %d\n  y = %d\n  width = %d\n  height = %d\n"
	   "  xOff = %d\n  yOff = %d\n",
	   text, ext.x, ext.y, ext.width, ext.height, ext.xOff, ext.yOff);

	*w = ext.width;
	*h = ext.height;
}

static void fill_rect(xcb_window_t win, xcb_gcontext_t gc, struct color *color,
		      int16_t x, int16_t y, uint16_t w, uint16_t h)
{
	xcb_rectangle_t rect = { x, y, w, h, };
	xcb_change_gc(dpy, gc, XCB_GC_FOREGROUND, color->val);
	xcb_poly_fill_rectangle(dpy, win, gc, 1, &rect);
}

static void draw_panel_text(struct panel *panel, struct color *fg,
			    struct color *bg, int16_t x, uint16_t w,
			    const char *text, int len, XftFont *font,
			    uint8_t xpad)
{
	fill_rect(panel->win, panel->gc, bg, x, ITEM_V_MARGIN, w,
		  panel_height - 2 * ITEM_V_MARGIN);

	x += xpad;

	XftDrawStringUtf8(panel->draw, fg->val, font, x, text_yoffs,
			  (XftChar8 *) text, len);
	XSync(xdpy, 0);
}

static void title_width(struct screen *scr)
{
	int16_t end = scr->items[PANEL_AREA_DOCK].x;
	uint16_t x, h, i;
	char *tmp;
	struct panel_item *title = &scr->items[PANEL_AREA_TITLE];

	i = 1;
	x = 0;
	while (x + title->x < end) {
		tmp = calloc(1, i + 1);
		memset(tmp, 'w', i);
		text_exts(tmp, strlen(tmp), &x, &h, font1);
		free(tmp);
		i++;
	}

	title->w = x - ITEM_H_MARGIN;
}

static void print_title(struct screen *scr, xcb_window_t win)
{
	struct sprop title;
	uint16_t w, h;

	/* clean area */
	fill_rect(scr->panel.win, scr->panel.gc, color2ptr(NORMAL_BG),
		  scr->items[PANEL_AREA_TITLE].x, 0,
		  scr->items[PANEL_AREA_DOCK].x, panel_height);

	if (win == XCB_WINDOW_NONE)
		return;

	get_sprop(&title, win, a_net_wm_name, UINT_MAX);
	if (!title.ptr || !title.len) {
		get_sprop(&title, win, XCB_ATOM_WM_NAME, UCHAR_MAX);
		if (!title.ptr || !title.len)
			goto out;
	}
	text_exts(title.str, title.len, &w, &h, font1);

	tt("scr %d tag '%s' title '%s'\n", scr->id, scr->tag->name, title.str);

	if (w > scr->items[PANEL_AREA_TITLE].w) {
		do {
			text_exts(title.str, title.len--, &w, &h, font1);
		} while (w > scr->items[PANEL_AREA_TITLE].w);
		title.str[title.len - 3] = '.';
		title.str[title.len - 2] = '.';
		title.str[title.len - 1] = '.';
	}

	draw_panel_text(&scr->panel, color2ptr(NORMAL_FG),
			color2ptr(NORMAL_BG),
			scr->items[PANEL_AREA_TITLE].x,
			scr->items[PANEL_AREA_TITLE].w,
			title.str, title.len, font1, ITEM_H_MARGIN);
out:
	free(title.ptr);
}

static xcb_window_t window_leader(xcb_window_t win)
{
	xcb_window_t ret = XCB_WINDOW_NONE;
	xcb_get_property_cookie_t c;
	xcb_get_property_reply_t *r;

	c = xcb_get_property(dpy, 0, win, a_leader, XCB_ATOM_WINDOW, 0, 1);
	r = xcb_get_property_reply(dpy, c, NULL);
	if (r && r->length != 0) {
		ret = *(xcb_window_t *) xcb_get_property_value(r);
		if (ret == win)
			ret = XCB_WINDOW_NONE;
	}

	free(r);
	return ret;
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

	if (status != WIN_STATUS_UNKNOWN) { /* do some sanity check */
		struct sprop class;

		get_sprop(&class, win, XCB_ATOM_WM_CLASS, UCHAR_MAX);
		if (!class.len)
			status = WIN_STATUS_UNKNOWN;

		if (class.ptr)
			free(class.ptr);
	}

	return status;
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

static void resort_client(struct client *cli)
{
	if (cli->scr->flags & SCR_FLG_SWITCH_WINDOW &&
	    !(cli->scr->flags & SCR_FLG_SWITCH_WINDOW_NOWARP)) {
		cli->scr->flags &= ~SCR_FLG_SWITCH_WINDOW;
		return;
	}

	list_del(&cli->head);
	list_add(&cli->tag->clients, &cli->head);
}

static void warp_pointer(xcb_window_t win, int16_t x, int16_t y)
{
	/* generates enter notify event */
	xcb_warp_pointer_checked(dpy, XCB_NONE, win, 0, 0, 0, 0, x, y);
}

static void center_pointer(struct client *cli)
{
	if (cli->flags & CLI_FLG_POPUP)
		return;

	warp_pointer(cli->win, cli->w / 2, cli->h / 2);
}

static void restore_window(xcb_window_t win, struct screen **scr,
			   struct tag **tag)
{
	struct list_head *cur;
	FILE *f;
	char path[sizeof("0xffffffffffffffff")];
	uint8_t buf[2];

	snprintf(path, sizeof(path), ".session/0x%x", win);

	if (!(f = fopen(path, "r"))) {
		ww("skip restore win 0x%x, %s\n", win, strerror(errno));
		return;
	}

	buf[0] = buf[1] = 0;
	fread(buf, sizeof(buf), 1, f); /* continue with 0,0 upon failure */
	fclose(f);

	list_walk(cur, &screens) {
		struct screen *tmp = list2screen(cur);
		if (buf[0] == tmp->id) {
			*scr = tmp;
			break;
		}
	}

	if (!*scr)
		return;

	list_walk(cur, &(*scr)->tags) {
		struct tag *tmp = list2tag(cur);
		if (buf[1] == tmp->id) {
			*tag = tmp;
			ii("restore win 0x%x scr %d tag %d '%s'\n", win,
			   (*scr)->id, (*tag)->id, (*tag)->name);
			return;
		}
	}
}

static void store_client(struct client *cli, uint8_t clean)
{
	FILE *f;
	char path[sizeof("0xffffffffffffffff")];
	uint8_t buf[2];

	if (window_status(cli->win) == WIN_STATUS_UNKNOWN) { /* gone */
		clean = 1;
	} else if (cli->flags & (CLI_FLG_POPUP | CLI_FLG_IGNORED)) {
		return;
	}

	snprintf(path, sizeof(path), ".session/0x%x", cli->win);

	if (clean) {
		errno = 0;
		unlink(path);
		ii("clean %s, errno=%d\n", path, errno);
		return;
	}

	if (!(f = fopen(path, "w+"))) {
		ee("fopen(%s) failed, %s\n", path, strerror(errno));
		return;
	}

	buf[0] = cli->scr->id;
	buf[1] = cli->tag->id;
	errno = 0;
	fwrite(buf, sizeof(buf), 1, f);
	ii("store win 0x%x scr %d tag %d '%s', errno=%d\n", cli->win,
	   cli->scr->id, cli->tag->id, cli->tag->name, errno);
	fclose(f);
}

static void free_client(struct client *cli)
{
	ii("free client win 0x%x\n", cli->win);
	store_client(cli, 1);
	list_del(&cli->head);
	list_del(&cli->list);
	free(cli);
}

#if 0
static void ping_window(xcb_window_t win, xcb_timestamp_t time)
{
	xcb_client_message_event_t e = { 0 };

	e.response_type = XCB_CLIENT_MESSAGE;
	e.window = win;
	e.type = a_protocols;
	e.format = 32;
	e.data.data32[0] = a_ping;
	e.data.data32[1] = time;
	e.data.data32[2] = win;

	ii("> ping win 0x%x time %u (%d)\n", win, time, a_ping);
	xcb_send_event(dpy, 0, win, XCB_EVENT_MASK_NO_EVENT, (const char *) &e);
	xcb_flush(dpy);
}
#endif

static struct client *pid2cli(pid_t pid)
{
	struct list_head *cur;

	list_walk(cur, &clients) {
		struct client *cli = glob2client(cur);

		if (cli->pid == pid)
			return cli;
	}

	return NULL;
}

#ifndef VERBOSE
#define trace_screen_metrics(scr) do {} while(0)
#else
#define trace_screen_metrics(scr)\
	ii("%s: screen %d geo %dx%d+%d+%d\n", __func__, scr->id, scr->w,\
	   scr->h, scr->x, scr->y)
#endif

static struct screen *coord2scr(int16_t x, int16_t y)
{
	struct list_head *cur;

	list_walk(cur, &screens) {
		struct screen *scr = list2screen(cur);

		if (scr->x <= x && x <= (scr->x + scr->w - 1) &&
		    scr->y <= y && y <= (scr->y + scr->h + panel_height)) {
			return scr;
		}
	}
	return list2screen(screens.next);
}

static struct client *scr2cli(struct screen *scr, xcb_window_t win,
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

static int pointer2coord(int16_t *x, int16_t *y, xcb_window_t *win)
{
	xcb_query_pointer_cookie_t c;
	xcb_query_pointer_reply_t *r;

	c = xcb_query_pointer(dpy, rootscr->root);
	r = xcb_query_pointer_reply(dpy, c, NULL);

	if (!r)
		return -1;

	*x = r->root_x;
	*y = r->root_y;

	if (win)
		*win = r->child;

	free(r);
	return 0;
}

static struct screen *pointer2scr(void)
{
	int16_t x;
	int16_t y;

	if (pointer2coord(&x, &y, NULL) < 0)
		curscr = list2screen(screens.prev);
	else
		curscr = coord2scr(x, y);

	return curscr;
}

static xcb_window_t pointer2win(void)
{
	int16_t x;
	int16_t y;
	xcb_window_t win;

	if (pointer2coord(&x, &y, &win) < 0)
		return XCB_WINDOW_NONE;

	curscr = coord2scr(x, y);
	return win;
}

static struct client *pointer2cli(void)
{
	struct list_head *head;
	xcb_window_t win = pointer2win();

	list_walk(head, &curscr->tag->clients) {
		struct client *cli = list2client(head);
		if (cli->flags & CLI_FLG_POPUP)
			return NULL;
		else if (cli->win == win)
			return cli;
	}

        return NULL;
}

static void unfocus_window(xcb_window_t win)
{
	if (win == toolbar.panel.win)
		return;
	else if (toolbar.cli && toolbar.cli->win == win)
		window_border_color(win, notice_bg);
	else
		window_border_color(win, normal_bg);

	xcb_window_t tmp = XCB_WINDOW_NONE;
	xcb_change_property(dpy, XCB_PROP_MODE_REPLACE, rootscr->root,
			    a_active_win, XCB_ATOM_WINDOW, 32, 1, &tmp);
}

static void focus_window(xcb_window_t win)
{
	if (win == toolbar.panel.win)
		return;
	else if (toolbar.cli && toolbar.cli->win == win)
		window_border_color(win, notice_bg);
	else
		window_border_color(win, active_bg);

	xcb_change_property(dpy, XCB_PROP_MODE_REPLACE, rootscr->root,
			    a_active_win, XCB_ATOM_WINDOW, 32, 1, &win);
	xcb_set_input_focus_checked(dpy, XCB_NONE, win, XCB_CURRENT_TIME);
	print_title(curscr, win);
}

static void unfocus_clients(struct tag *tag)
{
	struct list_head *cur;

	list_walk(cur, &tag->clients) {
		struct client *cli = list2client(cur);
		unfocus_window(cli->win);
	}
}

static void raise_window(xcb_window_t win)
{
	uint32_t val[1] = { XCB_STACK_MODE_ABOVE, };
	uint16_t mask = XCB_CONFIG_WINDOW_STACK_MODE;
	xcb_configure_window_checked(dpy, win, mask, val);
}

static void focus_root(void)
{
	print_title(curscr, XCB_WINDOW_NONE);
	xcb_set_input_focus_checked(dpy, XCB_NONE, rootscr->root,
				    XCB_CURRENT_TIME);
}

static void focus_any(pid_t pid)
{
	struct client *cli;

	curscr = pointer2scr();

	if (focus_root_) {
		focus_root_ = 0;
		focus_root();
		return;
	}

	cli = NULL;

	if (pid)
		cli = pid2cli(pid);

	if (cli)
		ii("pid %d win 0x%x\n", pid, cli->win);

	if (!cli && !(cli = front_client(curscr->tag)))
		cli = pointer2cli();

	if (cli) {
		if (window_status(cli->win) != WIN_STATUS_VISIBLE) {
			ww("invisible front win 0x%x\n", cli->win);
			cli = NULL;
		} else {
			ii("front win 0x%x\n", cli->win);
			raise_window(cli->win);
			focus_window(cli->win);
			center_pointer(cli);
		}
	}

	if (!cli) {
		focus_root();
		warp_pointer(rootscr->root, curscr->x + curscr->w / 2,
			     curscr->top + curscr->h / 2);
	}
}

static void hide_toolbox()
{
	xcb_unmap_window(dpy, toolbox.win);
	toolbox.cli = NULL;
}

static void show_toolbox(struct client *cli)
{
	uint32_t val[2];
	uint32_t mask;

	if (cli && cli->flags & (CLI_FLG_POPUP | CLI_FLG_EXCLUSIVE)) {
		return;
	} else if (toolbox.skip || !cli) {
		toolbox.skip = 0;
		hide_toolbox();
		return;
	}

	mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
#if 0
	val[0] = cli->x + 2 * BORDER_WIDTH;
	val[1] = cli->y + 2 * BORDER_WIDTH;
#else
	val[0] = (cli->x + cli->w) - toolbox.size;
	val[1] = (cli->y + cli->h) - toolbox.size;
#endif
	raise_window(toolbox.win);
	xcb_configure_window(dpy, toolbox.win, mask, val);
	xcb_map_window(dpy, toolbox.win);
	fill_rect(toolbox.win, toolbox.gc, color2ptr(NOTICE_BG), 0, 0,
		  toolbox.size, toolbox.size);

	struct color *fg = color2ptr(NOTICE_FG);
	uint16_t x = (toolbox.size - FONT2_SIZE) / 2;

	XftDrawStringUtf8(toolbox.draw, fg->val, font2, x, text_yoffs,
			 (XftChar8 *) BTN_TOOLS, slen(BTN_TOOLS));
	XSync(xdpy, 0);
	toolbox.skip = 0;
	toolbox.cli = cli;
}

static void init_toolbox(void)
{
	uint32_t val[2];
	uint32_t mask;

	toolbox.size = panel_height;

#if 1
	mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	val[0] = color2int(NOTICE_BG);
	val[1] = XCB_EVENT_MASK_BUTTON_PRESS;
	val[1] |= XCB_EVENT_MASK_LEAVE_WINDOW;
#else
	mask = XCB_CW_EVENT_MASK;
	val[0] = XCB_EVENT_MASK_BUTTON_PRESS;
	val[0] |= XCB_EVENT_MASK_LEAVE_WINDOW;
#endif
	toolbox.win = xcb_generate_id(dpy);
	xcb_create_window(dpy, XCB_COPY_FROM_PARENT, toolbox.win,
			  rootscr->root, 0, 0, toolbox.size, toolbox.size, 0,
			  XCB_WINDOW_CLASS_INPUT_OUTPUT, rootscr->root_visual,
			  mask, val);
	window_border_width(toolbox.win, 0);
	xcb_flush(dpy);
	mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND;
	val[0] = val[1] = color2int(NORMAL_BG);
	toolbox.gc = xcb_generate_id(dpy);
	xcb_create_gc(dpy, toolbox.gc, toolbox.win, mask, val);
	toolbox.draw = XftDrawCreate(xdpy, toolbox.win,
				     DefaultVisual(xdpy, xscr),
				     DefaultColormap(xdpy, xscr));
	XSync(xdpy, 0);
	xcb_flush(dpy);
}

static void close_client(struct client *cli)
{
	ii("terminate pid %d win 0x%x\n", cli->pid, cli->win);

	if (cli->pid)
		kill(cli->pid, SIGTERM);

	free_client(cli);
}

static void disable_events(xcb_window_t win)
{
	uint32_t val = 0;
	xcb_change_window_attributes_checked(dpy, win, XCB_CW_EVENT_MASK, &val);
	xcb_flush(dpy);
}

static uint16_t reset_toolbar(void)
{
	struct toolbar_item *ptr = toolbar_items;
	struct toolbar_item *end = toolbar_items + ARRAY_SIZE(toolbar_items);
	uint16_t h;
	uint16_t w = 0;

	while (ptr < end) {
		ptr->flags = 0;

		if (ptr->str == (const char *) BTN_FLAG &&
		    curscr->tag->anchor) {
			ptr->flags = ITEM_FLG_LOCKED;
		}

		ptr->x = w;
		text_exts(ptr->str, ptr->len, &ptr->w, &h, font2);
		w += (panel_height - ITEM_V_MARGIN);
		ptr++;
	}

	focused_item = NULL;
	toolbar.cli = NULL;
	toolbar.scr = NULL;
	toolbar.panel.win = XCB_WINDOW_NONE;

	ii("toolbar width %u\n", w);
	return w;
}

static void toolbar_ungrab_input(void)
{
	xcb_ungrab_pointer(dpy, XCB_CURRENT_TIME);
	xcb_ungrab_key(dpy, toolbar.kprev, rootscr->root, 0);
	xcb_ungrab_key(dpy, toolbar.knext, rootscr->root, 0);
	xcb_ungrab_key(dpy, toolbar.kenter, rootscr->root, 0);
	xcb_ungrab_key(dpy, toolbar.kclose, rootscr->root, 0);
}

static void toolbar_grab_input(void)
{
	xcb_grab_pointer(dpy, 1, rootscr->root,
			 XCB_EVENT_MASK_BUTTON_MOTION |
			 XCB_EVENT_MASK_BUTTON_PRESS |
			 XCB_EVENT_MASK_BUTTON_RELEASE,
			 XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
			 toolbar.panel.win, XCB_NONE, XCB_CURRENT_TIME);

	xcb_grab_key(dpy, 0, rootscr->root, 0, toolbar.kprev,
		     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
	xcb_grab_key(dpy, 0, rootscr->root, 0, toolbar.knext,
		     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
	xcb_grab_key(dpy, 0, rootscr->root, 0, toolbar.kenter,
		     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
	xcb_grab_key(dpy, 0, rootscr->root, 0, toolbar.kclose,
		     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
}

static void hide_toolbar(void)
{
	toolbar_ungrab_input();

	if (toolbar.panel.win == XCB_WINDOW_NONE)
		return;

	disable_events(toolbar.panel.win);
	xcb_flush(dpy);
	XftDrawDestroy(toolbar.panel.draw);
	xcb_free_gc(dpy, toolbar.panel.gc);
	xcb_destroy_window_checked(dpy, toolbar.panel.win);
	dd("destroyed toolbar win 0x%x", toolbar.panel.win);

	if (toolbar.cli) {
		struct client *cli = toolbar.cli;
		toolbar.cli = NULL;
		raise_window(cli->win);
		focus_window(cli->win);
		center_pointer(cli);
	}

	toolbar.scr->items[PANEL_AREA_TITLE].x = toolbar.title_x;
	title_width(toolbar.scr);
	xcb_flush(dpy);
	reset_toolbar();
}

static void close_window(xcb_window_t win)
{
	uint8_t i;
	struct timespec ts = { 0, 10000000 };
	pid_t pid;
	struct client *cli = win2cli(win);
	xcb_client_message_event_t e = { 0 };

	cli ? (pid = cli->pid) : (pid = 0);

	/* try to close gracefully */

	e.response_type = XCB_CLIENT_MESSAGE;
	e.window = win;
	e.type = a_protocols;
	e.format = 32;
	e.data.data32[0] = a_delete_win;
	e.data.data32[1] = XCB_CURRENT_TIME;

	xcb_send_event_checked(dpy, 0, win, XCB_EVENT_MASK_NO_EVENT,
			       (const char *) &e);
	xcb_flush(dpy);

	cli->busy++; /* give client a chance to exit gracefully */

	for (i = 0; i < 50; ++i) { /* wait for 500 ms max */
		if (window_status(win) != WIN_STATUS_VISIBLE) {
			cli->busy = 0;
			break;
		}

		ww("%u: window 0x%x still open (%u)\n", i, win, cli->busy);
		nanosleep(&ts, NULL);
	}

	if (cli->busy > 2) {
		close_client(cli);
		hide_toolbox();
		hide_toolbar();
	} else if (cli && !cli->busy) {
		free_client(cli);
		hide_toolbox();
		hide_toolbar();
	}

	focus_any(pid);
	xcb_flush(dpy);
}

static struct screen *cli2scr(struct client *cli)
{
#if 1
	return cli->scr;
#else
	struct list_head *scr_head;
	struct list_head *cli_head;

	list_walk(scr_head, &screens) {
		struct screen *scr = list2screen(scr_head);
		list_walk(cli_head, &scr->tag->clients) {
			if (cli == list2client(cli_head))
				return scr;
		}
	}
	return NULL;
#endif
}

static void draw_locked_text(struct toolbar_item *item)
{
	struct color *fg;
	struct color *bg;
	int16_t xx;
	uint16_t ww;

	if (item->flags & ITEM_FLG_LOCKED) {
		fg = color2ptr(NOTICE_FG);
		bg = color2ptr(NOTICE_BG);
	} else {
		fg = color2ptr(NORMAL_FG);
		bg = color2ptr(NORMAL_BG);
	}

	ww = panel_height - ITEM_V_MARGIN;
	xx = (ww - item->w) / 2;
	draw_panel_text(&toolbar.panel, fg, bg, item->x, ww,
			item->str, item->len, font2, xx);
}

static void draw_toolbar_text(struct toolbar_item *item, uint8_t flag)
{
	struct color *fg;
	struct color *bg;
	int16_t xx;
	uint16_t ww;

	if (item->flags & flag)
		return;

	if (flag == ITEM_FLG_ALERT) {
		fg = color2ptr(ALERT_FG);
		bg = color2ptr(ALERT_BG);
		item->flags &= ~(ITEM_FLG_ACTIVE | ITEM_FLG_NORMAL);
		item->flags &= ~(ITEM_FLG_FOCUSED | ITEM_FLG_ALERT);
	} else if (flag == ITEM_FLG_FOCUSED || flag == ITEM_FLG_LOCKED) {
		fg = color2ptr(NOTICE_FG);
		bg = color2ptr(NOTICE_BG);
		item->flags &= ~(ITEM_FLG_ACTIVE | ITEM_FLG_NORMAL);
		item->flags &= ~ITEM_FLG_ALERT;
	} else {
		fg = color2ptr(NORMAL_FG);
		bg = color2ptr(NORMAL_BG);
		item->flags &= ~(ITEM_FLG_FOCUSED | ITEM_FLG_ACTIVE);
		item->flags &= ~ITEM_FLG_ALERT;
	}

	ww = panel_height - ITEM_V_MARGIN;
	xx = (ww - item->w) / 2;

	if (item->w % 2)
		ww -= 2; /* FIXME: fonts looks better centered this way but
			  * this is not very logical */

	item->flags |= flag;
	draw_panel_text(&toolbar.panel, fg, bg, item->x, ww,
			item->str, item->len, font2, xx);
}

static void focus_toolbar_item(int16_t x, int16_t y)
{
	struct toolbar_item *ptr = toolbar_items;
	struct toolbar_item *end = toolbar_items + ARRAY_SIZE(toolbar_items);
	uint16_t xx = 0;
	uint8_t flg;

	focused_item = NULL;

	if (y > panel_height) /* out of range */
		return;

	while (ptr < end) {
		xx = ptr->x + panel_height - ITEM_V_MARGIN - 1;

		if (x < ptr->x || x > xx) { /* out of focus */
			if (ptr->flags & ITEM_FLG_LOCKED) {
				ptr->flags &= ~ITEM_FLG_LOCKED; /* toggle lock */
				flg = ITEM_FLG_LOCKED;
			} else {
				flg = ITEM_FLG_NORMAL;
			}
		} else { /* in focus */
			if (ptr->str == (const char *) BTN_CLOSE)
				flg = ITEM_FLG_ALERT;
			else
				flg = ITEM_FLG_FOCUSED;

			focused_item = ptr;
		}

		draw_toolbar_text(ptr, flg);
		ptr++;
	}
}

static void draw_toolbar(void)
{
	warp_pointer(toolbar.panel.win, toolbar_items[0].x, panel_height / 2);
	focus_toolbar_item(toolbar_items[0].x, 0);
	xcb_flush(dpy);
}

static void exec(const char *cmd)
{
	if (fork() != 0)
		return;

	if (homedir)
		chdir(homedir);

	close(xcb_get_file_descriptor(dpy));
	close(ConnectionNumber(xdpy));
	setsid();
	system(cmd);
	exit(0);
}

static void spawn_cleanup(int sig)
{
	while (waitpid(-1, NULL, WNOHANG) < 0) {
		if (errno != EINTR)
			break;
	}
}

static void spawn(void *ptr)
{
	struct arg *arg = (struct arg *) ptr;
	uint16_t len = baselen + sizeof("/keys/") + UCHAR_MAX;
	char path[len];

	if (!basedir)
		return;

	snprintf(path, len, "%s/keys/%s", basedir, arg->kmap->keyname);
	exec(path);
}

static void autostart(void)
{
	uint16_t len = baselen + sizeof("autostart");
	char path[len];

	if (!basedir)
		return;

	snprintf(path, len, "%s/autostart", basedir);
	exec(path);
}

static void show_menu(void)
{
	uint16_t len = baselen + sizeof("/panel/menu");
	char path[len];

	if (!basedir)
		return;

	snprintf(path, len, "%s/panel/menu", basedir);
	exec(path);
}

static void clean(void)
{
	xcb_disconnect(dpy);

	if (font1)
		XftFontClose(xdpy, font1);
	if (font2)
		XftFontClose(xdpy, font2);
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

static xcb_atom_t get_atom_by_name(const char *str, strlen_t len)
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

#ifndef TRACE_ATOM
#define print_atom_name(a) do {} while(0)
#else
#define print_atom_name(atom) {\
	strlen_t len__;\
	char *name__, *tmp__;\
	xcb_get_atom_name_reply_t *r__;\
	xcb_get_atom_name_cookie_t c__;\
	if (atom == _NET_WM_OPAQUE_REGION) {\
		ii("atom %d, name _NET_WM_OPAQUE_REGION | %s:%d\n", atom,\
		   __func__, __LINE__);\
		return;\
	}\
	c__ = xcb_get_atom_name(dpy, atom);\
	r__ = xcb_get_atom_name_reply(dpy, c__, NULL);\
	if (r__) {\
		len__ = xcb_get_atom_name_name_length(r__);\
		if (len__ > 0) {\
			name__ = xcb_get_atom_name_name(r__);\
			tmp__ = strndup(name__, len__);\
			ii("atom %d, name %s, len %d | %s:%d\n", atom, tmp__,\
			   len__, __func__, __LINE__);\
			free(tmp__);\
		}\
		free(r__);\
	}\
}
#endif /* TRACE */

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
		}

		ii("%s: %d,%d (%d)\n", name, scr->items[i].x,
		   scr->items[i].x + scr->items[i].w,
		   scr->items[i].w);
	}
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

static void update_client_list(void)
{
	struct list_head *cur;

	xcb_delete_property(dpy, rootscr->root, a_client_list);
	list_walk(cur, &clients) {
		struct client *cli = glob2client(cur);
		if (window_status(cli->win) == WIN_STATUS_UNKNOWN)
			continue;
		tt("append window 0x%x\n", cli->win);
		xcb_change_property(dpy, XCB_PROP_MODE_APPEND, rootscr->root,
				    a_client_list, XCB_ATOM_WINDOW, 32, 1,
				    &cli->win);
	}
}

static uint8_t tray_window(xcb_window_t win)
{
	uint8_t ret;
	xcb_get_property_cookie_t c;
	xcb_get_property_reply_t *r;

	c = xcb_get_property(dpy, 0, win, a_embed_info,
			     XCB_GET_PROPERTY_TYPE_ANY, 0, 2 * 32);
	r = xcb_get_property_reply(dpy, c, NULL);
	if (!r || r->length == 0) {
		tt("no _XEMBED_INFO (%d) available\n", a_embed_info);
		ret = 0;
	} else {
#ifdef TRACE
		uint32_t *val = xcb_get_property_value(r);
		tt("fmt %u, len %u, ver %u, flg %x\n", r->format, r->length,
		   val[0], val[1]);
#endif
		ret = 1;
	}

	free(r);
	return ret;
}

/* Specially treated window
 *
 * 1) only single instance of given window will be allowed:
 *
 *    /<basedir>/exclusive/{<winclass1>,<winclassN>}
 *
 * 2) force window location:
 *
 *    /<basedir>/<sub-dirs>/{<winclass1>,<winclassN>}
 *
 *    sub-dirs: {dock,center,top-left,top-right,bottom-left,bottom-right}
 *
 */

static uint32_t window_special(xcb_window_t win, const char *dir, uint8_t len)
{
	struct sprop class;
	char *path;
	struct stat st;
	uint32_t crc;
	xcb_atom_t atom;
	int count;
	int pathlen;

	if (!basedir)
		return 0;

	atom = XCB_ATOM_WM_CLASS;
	count = 0;
more:
	get_sprop(&class, win, atom, UCHAR_MAX);
	if (!class.ptr) {
		ww("unable to detect window class\n");
		return 0;
	}

	crc = 0;
	pathlen = baselen + len + class.len + 1;
	path = calloc(1, pathlen);
	if (!path)
		goto out;

	snprintf(path, pathlen, "%s/%s/%s", basedir, dir, class.str);
	if (class.str[0] && stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
		crc = crc32(class.str, class.len);
		tt("special win 0x%x, path %s, crc 0x%x\n", win, path, crc);
	} else if (++count < 2) {
		free(class.ptr);
		free(path);
		atom = XCB_ATOM_WM_NAME;
		goto more;
	}

out:
	free(class.ptr);
	free(path);
	return crc;
}

static int panel_window(xcb_window_t win)
{
	struct list_head *cur;

	list_walk(cur, &screens) {
		struct screen *scr = list2screen(cur);
		if (scr->panel.win == win)
			return 1;
	}
	return 0;
}

static void window_state(xcb_window_t win, uint8_t state)
{
	uint32_t data[] = { state, XCB_NONE };
	xcb_change_property_checked(dpy, XCB_PROP_MODE_REPLACE, win,
				    a_state, a_state, 32, 2, data);
}

static int setup_toolbar(struct client *cli)
{
	uint16_t w;
	uint32_t val[2], mask;

	w = reset_toolbar();

	if (cli) {
		toolbar.cli = cli;
	} else {
		if (list_empty(&curscr->tag->clients)) {
			ww("nothing to show on empty tag\n");
			return -1;
		}

		if (!(toolbar.cli = pointer2cli()))
			toolbar.cli = front_client(curscr->tag);

		if (!toolbar.cli) {
			ww("no front client on scr %u tag '%s'\n", curscr->id,
			   curscr->tag->name);
			return -1;
		}
	}

	toolbar.scr = curscr;
	toolbar.x = curscr->items[PANEL_AREA_TITLE].x;
	toolbar.y = curscr->panel.y;
	toolbar.panel.w = w;
	toolbar.panel.h = panel_height;
	toolbar.panel.win = xcb_generate_id(dpy);

	mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	val[0] = color2int(NORMAL_BG);
	val[1] = XCB_EVENT_MASK_VISIBILITY_CHANGE;
	val[1] |= XCB_EVENT_MASK_PROPERTY_CHANGE;
	val[1] |= XCB_EVENT_MASK_STRUCTURE_NOTIFY;
	val[1] |= XCB_EVENT_MASK_ENTER_WINDOW;
	val[1] |= XCB_EVENT_MASK_LEAVE_WINDOW;
	val[1] |= XCB_EVENT_MASK_BUTTON_PRESS;
	val[1] |= XCB_EVENT_MASK_KEY_PRESS;
	val[1] |= XCB_EVENT_MASK_POINTER_MOTION;

	window_border_color(toolbar.cli->win, notice_bg);
	xcb_create_window(dpy, XCB_COPY_FROM_PARENT, toolbar.panel.win,
			  rootscr->root,
			  0, 0, toolbar.panel.w, toolbar.panel.h,
			  0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  rootscr->root_visual, mask, val);
	xcb_flush(dpy);

	toolbar.panel.gc = xcb_generate_id(dpy);
	mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND;
	val[0] = val[1] = color2int(NORMAL_BG);
        xcb_create_gc(dpy, toolbar.panel.gc, toolbar.panel.win, mask, val);

	toolbar.panel.draw = XftDrawCreate(xdpy, toolbar.panel.win,
					   DefaultVisual(xdpy, xscr),
					   DefaultColormap(xdpy, xscr));

	ii("toolbar 0x%x geo %ux%u+%d+%d target 0x%x\n", toolbar.panel.win,
	   toolbar.panel.w, toolbar.panel.h, toolbar.x, toolbar.y,
	   toolbar.cli->win);
	return 0;
}

static void show_toolbar(struct client *cli)
{
	uint32_t val[4];
	uint32_t mask;

	if (toolbar.cli) {
		hide_toolbar();

		if (!cli)
			return;
	}

	if (setup_toolbar(cli) < 0)
		return;

	mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
	mask |= XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
	val[0] = toolbar.x;
	val[1] = toolbar.y;
	val[2] = toolbar.panel.w;
	val[3] = toolbar.panel.h;
	xcb_configure_window(dpy, toolbar.panel.win, mask, val);

	ii("toolbar 0x%x on scr %d, tag %s\n", toolbar.panel.win,
	   toolbar.scr->id, toolbar.scr->tag->name);
	xcb_map_window(dpy, toolbar.panel.win);
	draw_toolbar();
	toolbar.title_x = toolbar.scr->items[PANEL_AREA_TITLE].x;
	toolbar.scr->items[PANEL_AREA_TITLE].x = toolbar.x + toolbar.panel.w;
	title_width(toolbar.scr);
	print_title(toolbar.scr, toolbar.cli->win);
	raise_window(toolbar.panel.win);
	focus_window(toolbar.panel.win);
	toolbar_grab_input();
	xcb_flush(dpy);
}

static void toggle_toolbar(unused(void *ptr))
{
	show_toolbar(NULL);
}

static struct client *prev_client(struct client *cli)
{
	struct list_head *cur;

	list_back(cur, &cli->head) {
		cli = list2client(cur);
		if (window_status(cli->win) == WIN_STATUS_VISIBLE)
			return cli;
	}

	return NULL;
}

static struct client *next_client(struct client *cli)
{
	struct list_head *cur;

	list_walk(cur, &cli->head) {
		struct client *ret = list2client(cur);
		if (window_status(ret->win) == WIN_STATUS_VISIBLE)
			return ret;
	}

	dd("no next valid window after 0x%x", cli->win);
	return NULL;
}

static struct client *switch_window(struct screen *scr, enum dir dir)
{
	struct client *cli;

	hide_toolbox();
	hide_toolbar();

	if ((cli = pointer2cli())) {
		ii("scr %u tag '%s' prev cli %p, win 0x%x (pointer)\n",
		   scr->id, scr->tag->name, cli, cli->win);
	} else if ((cli = front_client(scr->tag))) {
		ii("scr %u tag '%s' prev cli %p, win 0x%x\n",
		   scr->id, scr->tag->name, cli, cli->win);
	} else {
		ee("no front windows found\n");
		return NULL;
	}

	trace_tag_windows(scr->tag);

	scr->flags |= SCR_FLG_SWITCH_WINDOW;

	if (dir == DIR_NEXT)
		cli = next_client(cli);
	else
		cli = prev_client(cli);

	if (!cli)
		return NULL;

	ii("scr %u tag '%s' next cli %p, win 0x%x\n",
	   scr->id, scr->tag->name, cli, cli->win);

	raise_window(cli->win);
	focus_window(cli->win);

	if (!(scr->flags & SCR_FLG_SWITCH_WINDOW_NOWARP)) {
		center_pointer(cli);
	} else {
		struct client *tmp = front_client(scr->tag);
		if (tmp)
			unfocus_window(tmp->win);

		if (scr->tag->visited != XCB_WINDOW_NONE) {
			unfocus_window(scr->tag->visited);
			scr->tag->visited = XCB_WINDOW_NONE;
		}

		resort_client(cli);
	}

	scr->flags &= ~SCR_FLG_SWITCH_WINDOW_NOWARP;
	xcb_flush(dpy);

	return cli;
}

static int16_t adjust_x(struct screen *scr, int16_t x, uint16_t w)
{
	if (x < scr->x || x > scr->x + scr->w || x + w > scr->x + scr->w)
		return scr->x;
	return x;
}

static int16_t adjust_y(struct screen *scr, int16_t y, uint16_t h)
{
	if (y < scr->top || y > scr->top + scr->h || y + h > scr->top + scr->h)
		return scr->top;
	else
		return y;
}

static uint16_t adjust_w(struct screen *scr, uint16_t w)
{
	if (w > scr->w)
		return scr->w - 2 * BORDER_WIDTH;
	else if (w < WIN_WIDTH_MIN)
		return scr->w / 2 - 2 * BORDER_WIDTH;
	return w;
}

static uint16_t adjust_h(struct screen *scr, uint16_t h)
{
	if (h > scr->h)
		return scr->h - 2 * BORDER_WIDTH;
	else if (h < WIN_HEIGHT_MIN)
		return scr->h / 2 - 2 * BORDER_WIDTH;
	return h;
}

static void client_moveresize(struct client *cli, int16_t x, int16_t y,
			      uint16_t w, uint16_t h)
{
	uint32_t val[4];
	uint16_t mask;

	if (!(cli->flags & CLI_FLG_DOCK)) {
		/* fit into monitor space */
		x = adjust_x(cli->scr, x, w);
		y = adjust_y(cli->scr, y, h);
		w = adjust_w(cli->scr, w);
		h = adjust_h(cli->scr, h);
	}

	val[0] = cli->x = x;
	val[1] = cli->y = y;
	val[2] = cli->w = w;
	val[3] = cli->h = h;
	mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
	mask |= XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
	xcb_configure_window_checked(dpy, cli->win, mask, val);

	tt("scr %d, cli %p, win 0x%x, geo %ux%u+%d+%d\n", cli->scr->id, cli,
	   cli->win, cli->w, cli->h, cli->x, cli->y);
}

#define GRIDCELL_MIN_WIDTH 100

static uint8_t cell_size(uint16_t n, uint16_t *w, uint16_t *h)
{
	uint16_t i;

	for (i = 1; i < curscr->tag->space.w / GRIDCELL_MIN_WIDTH; i++) {
		if (i * i >= n) {
			*w = curscr->tag->space.w / i;
			*h = curscr->tag->space.h / i;
			return 1;
		} else if (i * (i + 1) >= n) {
			*w = curscr->tag->space.w / (i + 1);
			*h = curscr->tag->space.h / i;
			return 1;
		}
	}

	ww("failed to calculate cell size for screen %d\n", curscr->id);
	return 0;
}

static void space_halfh(struct screen *scr, uint16_t y, uint8_t div)
{
	scr->tag->space.x = scr->x;
	scr->tag->space.y = y;
	scr->tag->space.w = scr->w;
	scr->tag->space.h = scr->h - scr->h / div;
}

static void space_halfw(struct screen *scr, uint16_t x, uint8_t div)
{
	scr->tag->space.x = x;
	scr->tag->space.y = scr->top;
	scr->tag->space.w = scr->w - scr->w / div;
	scr->tag->space.h = scr->h;
}

static void space_fullscr(struct screen *scr)
{
	scr->tag->space.x = scr->x;
	scr->tag->space.y = scr->top;
	scr->tag->space.w = scr->w;
	scr->tag->space.h = scr->h;
}

static void recalc_space(struct screen *scr, enum winpos pos)
{
	struct client *cli = scr->tag->anchor;

	if (!cli)
		space_fullscr(scr);
	else if (pos == WIN_POS_LEFT_FILL)
		space_halfw(scr, scr->x + cli->w + 2 * BORDER_WIDTH, cli->div);
	else if (pos == WIN_POS_RIGHT_FILL)
		space_halfw(scr, scr->x, cli->div);
	else if (pos == WIN_POS_TOP_FILL)
		space_halfh(scr, scr->top + cli->h + 2 * BORDER_WIDTH, cli->div);
	else if (pos == WIN_POS_BOTTOM_FILL)
		space_halfh(scr, scr->top, cli->div);
	else
		space_fullscr(scr);

	ii("pos %d tag '%s' space geo %ux%u%+d%+d\n", pos, scr->tag->name,
	   scr->tag->space.w, scr->tag->space.h, scr->tag->space.x,
	   scr->tag->space.y);

	return;
}

static void make_grid(void *ptr)
{
	struct arg *arg = (struct arg *) ptr;
	struct list_head *cur;
	uint16_t i, n;
	uint16_t cw, ch; /* cell size */
	int16_t x, y;

	curscr = coord2scr(arg->x, arg->y);
	n = 0;

	list_walk(cur, &curscr->tag->clients) {
		struct client *cli = list2client(cur);
		if (curscr->tag->anchor == cli)
			continue;
		else if (cli->win & (CLI_FLG_IGNORED | CLI_FLG_POPUP))
			continue;
		else if (window_status(cli->win) == WIN_STATUS_VISIBLE)
			n++;
	}

	if (n == 0) {
		return;
	} else if (n == 1) {
		return;
	} else if (!cell_size(n, &cw, &ch)) {
		return;
	} else if (n == 2) {
		if (!arg->data) { /* only toggle via shortcut */
			if (curscr->tag->grid2v)
				curscr->tag->grid2v = 0;
			else
				curscr->tag->grid2v = 1;
		}

		if (curscr->tag->grid2v) { /* vertical split */
			cw = curscr->tag->space.w / 2;
			ch = curscr->tag->space.h;
		} else { /* horizontal split */
			cw = curscr->tag->space.w;
			ch = curscr->tag->space.h / 2;
		}
	}

	ii("%d cells size of (%u,%u)\n", n, cw, ch);
	i = x = y = 0;

	list_walk(cur, &curscr->tag->clients) {
		int16_t yy;
		int16_t xx;
		uint16_t hh;
		uint16_t ww;
		struct client *cli = list2client(cur);

		if (curscr->tag->anchor == cli)
			continue;
		else if (cli->win & (CLI_FLG_IGNORED | CLI_FLG_POPUP))
			continue;
		if (window_status(cli->win) != WIN_STATUS_VISIBLE)
			continue;

		xx = curscr->tag->space.x + x;
		ww = (curscr->tag->space.x + curscr->tag->space.w) - (xx + cw);
		(ww > 2 * BORDER_WIDTH) ? (ww = cw) : (ww += cw);
		ww -= 2 * BORDER_WIDTH - WINDOW_PAD;

		yy = curscr->tag->space.y + y;
		hh = (curscr->tag->space.y + curscr->tag->space.h) - (yy + ch);
		(hh > 2 * BORDER_WIDTH) ? (hh = ch) : (hh += ch);
		hh -= 2 * BORDER_WIDTH - WINDOW_PAD;

		if (++i >= n) { /* last window occupies remaining space */
			ww = curscr->tag->space.w - 2 * BORDER_WIDTH;
			ww -= xx - curscr->tag->space.x;
		}

		client_moveresize(cli, xx, yy, ww, hh);
		x += cw;

		if (x > curscr->tag->space.w - cw) {
			x = 0;
			y += ch;
		}

		unfocus_window(cli->win);
	}

	xcb_flush(dpy);
}

static void flag_window(void *ptr)
{
	struct arg *arg = (struct arg *) ptr;
	struct toolbar_item *cur = toolbar_items;
	struct toolbar_item *end = toolbar_items + ARRAY_SIZE(toolbar_items);
	struct client *cli;

	curscr = coord2scr(arg->x, arg->y);
	cli = curscr->tag->anchor;

	if (curscr->tag->anchor) {
		curscr->tag->anchor = NULL;
	} else {
		curscr->tag->anchor = pointer2cli();
		curscr->tag->anchor->div = 1;
		struct arg arg = { .data = WIN_POS_LEFT_FILL, };
		place_window(&arg);
	}

	if (curscr->tag->anchor)
		window_border_color(curscr->tag->anchor->win, notice_bg);
	else if (cli)
		window_border_color(cli->win, active_bg);

	xcb_flush(dpy);

	if (!toolbar.cli)
		return;

	while (cur < end) {
		if (cur->str == (const char *) BTN_FLAG) {
			uint8_t flg;

			if (curscr->tag->anchor)
				flg = ITEM_FLG_LOCKED;
			else
				flg = ITEM_FLG_NORMAL;

			draw_toolbar_text(cur, flg);
			break;
		}
		cur++;
	}
}

static void window_halfh(uint16_t *w, uint16_t *h, uint8_t div)
{
	*w = curscr->w - 2 * BORDER_WIDTH;
	*h = curscr->h / div - 2 * BORDER_WIDTH - WINDOW_PAD;

	if (div == 2 && curscr->h % div)
		(*h)++;
}

static void window_halfw(uint16_t *w, uint16_t *h, uint8_t div)
{
	*w = curscr->w / div - 2 * BORDER_WIDTH;
	*h = curscr->h - 2 * BORDER_WIDTH;
}

static void window_halfwh(uint16_t *w, uint16_t *h)
{
	*w = curscr->w / 2 - 2 * BORDER_WIDTH - WINDOW_PAD;
	*h = curscr->h / 2 - 2 * BORDER_WIDTH - WINDOW_PAD;

	if (curscr->h % 2)
		(*h)++;
}

static void place_window(void *ptr)
{
	struct arg *arg = (struct arg *) ptr;
	int16_t x, y;
	uint16_t w, h;
	struct client *cli;
	enum winpos pos;

	curscr = coord2scr(arg->x, arg->y);

	if (toolbar.cli)
		cli = toolbar.cli;
	else if (curscr->tag->anchor)
		cli = curscr->tag->anchor;
	else if (!(cli = pointer2cli()))
		return;

	if (arg->kmap)
		pos = (enum winpos) arg->kmap->arg;
	else
		pos = (enum winpos) arg->data;

	ii("scr %d, win 0x%x, where %d, div %d\n", curscr->id, cli->win, pos,
	   cli->div);

	switch (pos) {
	case WIN_POS_FILL:
		tt("WIN_POS_FILL\n");
		cli->flags |= CLI_FLG_FULLSCREEN;
		x = curscr->x;
		y = curscr->top;
		w = curscr->w - 2 * BORDER_WIDTH;
		h = curscr->h - 2 * BORDER_WIDTH;
		curscr->tag->anchor = NULL;
		break;
	case WIN_POS_CENTER:
		cli->flags |= CLI_FLG_CENTER;
		x = curscr->x + curscr->w / 2 - curscr->w / 4;
		y = curscr->top + curscr->h / 2 - curscr->h / 4;
		window_halfwh(&w, &h);
		break;
	case WIN_POS_LEFT_FILL:
		tt("WIN_POS_LEFT_FILL\n");

		if (last_winpos != pos || ++cli->div > POS_DIV_MAX)
			cli->div = 2;

		x = curscr->x;
		y = curscr->top;
		window_halfw(&w, &h, cli->div);
		break;
	case WIN_POS_RIGHT_FILL:
		tt("WIN_POS_RIGHT_FILL\n");

		if (last_winpos != pos || ++cli->div > POS_DIV_MAX)
			cli->div = 2;

		x = curscr->x + curscr->w - curscr->w / cli->div;
		y = curscr->x;
		window_halfw(&w, &h, cli->div);
		break;
	case WIN_POS_TOP_FILL:
		tt("WIN_POS_TOP_FILL\n");

		if (last_winpos != pos || ++cli->div > POS_DIV_MAX)
			cli->div = 2;

		x = curscr->x;
		y = curscr->top;
		window_halfh(&w, &h, cli->div);
		break;
	case WIN_POS_BOTTOM_FILL:
		tt("WIN_POS_BOTTOM_FILL\n");

		if (last_winpos != pos || ++cli->div > POS_DIV_MAX)
			cli->div = 2;

		x = curscr->x;
		y = curscr->top + curscr->h - curscr->h / cli->div;
		window_halfh(&w, &h, cli->div);
		break;
	case WIN_POS_TOP_LEFT:
		tt("WIN_POS_TOP_LEFT\n");
		x = curscr->x;
		y = curscr->top;
		window_halfwh(&w, &h);
		break;
	case WIN_POS_TOP_RIGHT:
		tt("WIN_POS_TOP_RIGHT\n");
		x = curscr->x + curscr->w / 2;
		y = curscr->top;
		window_halfwh(&w, &h);
		break;
	case WIN_POS_BOTTOM_LEFT:
		tt("WIN_POS_BOTTOM_LEFT\n");
		x = curscr->x;
		y = curscr->top + curscr->h / 2;
		window_halfwh(&w, &h);
		break;
	case WIN_POS_BOTTOM_RIGHT:
		tt("WIN_POS_BOTTOM_RIGHT\n");
		x = curscr->x + curscr->w / 2;
		y = curscr->top + curscr->h / 2;
		window_halfwh(&w, &h);
		break;
	default:
		return;
	}

	last_winpos = pos;
	resort_client(cli);
	raise_window(cli->win);
	focus_window(cli->win);

	if (cli->div == POS_DIV_MAX)
		window_border_color(cli->win, alert_bg);

	client_moveresize(cli, x, y, w, h);

	if (curscr->tag->anchor == cli) {
		recalc_space(curscr, pos);
		struct arg arg = {
			.x = curscr->x,
			.y = curscr->top,
			.data = 1, /* disable vert/horiz toggle */
		};
		make_grid(&arg);
	}

	if (cli != toolbar.cli)
		center_pointer(cli);

	store_client(cli, 0);
	hide_toolbox();
	xcb_flush(dpy);
	return;
}

static void next_window(void *ptr)
{
	struct arg *arg = (struct arg *) ptr;

	curscr = coord2scr(arg->x, arg->y);
	switch_window(curscr, DIR_NEXT);
}

static void prev_window(void *ptr)
{
	struct arg *arg = (struct arg *) ptr;

	curscr = coord2scr(arg->x, arg->y);
	switch_window(curscr, DIR_PREV);
}

static void raise_client(unused(void *ptr))
{
	struct client *cli;

	if ((cli = front_client(curscr->tag)))
		unfocus_window(cli->win);

	if (!(cli = pointer2cli()))
		return;

	raise_window(cli->win);
	focus_window(cli->win);
	resort_client(cli);
	xcb_flush(dpy);
	store_client(cli, 0);
}

static void panel_raise(struct screen *scr)
{
	if (scr && scr->panel.win) {
		raise_window(scr->panel.win);
		struct list_head *cur;
		list_walk(cur, &scr->dock) {
			struct client *cli = list2client(cur);
			raise_window(cli->win);
		}
	}
}

static void print_tag(struct screen *scr, struct tag *tag, uint8_t flag)
{
	struct color *fg;
	struct color *bg;

	if (tag->flags & flag)
		return;

	if (flag == ITEM_FLG_ACTIVE) {
		fg = color2ptr(ACTIVE_FG);
		bg = color2ptr(ACTIVE_BG);
		tag->flags &= ~(ITEM_FLG_FOCUSED | ITEM_FLG_NORMAL);
	} else if (flag == ITEM_FLG_FOCUSED) {
		fg = color2ptr(NOTICE_FG);
		bg = color2ptr(NOTICE_BG);
		tag->flags &= ~(ITEM_FLG_ACTIVE | ITEM_FLG_NORMAL);
	} else {
		fg = color2ptr(NORMAL_FG);
		bg = color2ptr(NORMAL_BG);
		tag->flags &= ~(ITEM_FLG_FOCUSED | ITEM_FLG_ACTIVE);
	}

	tag->flags |= flag;
	draw_panel_text(&scr->panel, fg, bg, tag->x, tag->w, tag->name,
			tag->nlen, font1, ITEM_H_MARGIN);
}

static void show_windows(struct tag *tag, uint8_t focus)
{
	struct client *cli;
	struct list_head *cur;

	print_title(curscr, XCB_WINDOW_NONE); /* empty titlebar */

	if (list_empty(&tag->clients)) {
		focus_root();
		return;
	}

	list_walk(cur, &tag->clients) {
		struct client *cli = list2client(cur);
		window_state(cli->win, XCB_ICCCM_WM_STATE_NORMAL);
		xcb_map_window_checked(dpy, cli->win);
	}

	if (!focus || !(cli = front_client(tag)) ||
	    (cli->flags & CLI_FLG_IGNORED) ||
	    window_status(cli->win) != WIN_STATUS_VISIBLE) {
		focus_root();
		return;
	}

	raise_window(cli->win);
	focus_window(cli->win);
	center_pointer(cli);
}

static void hide_windows(struct tag *tag)
{
	struct list_head *cur;

	list_walk(cur, &tag->clients) {
		struct client *cli = list2client(cur);
		window_state(cli->win, XCB_ICCCM_WM_STATE_ICONIC);
		xcb_unmap_window_checked(dpy, cli->win);
	}
}

static void focus_tag(struct screen *scr, struct tag *tag)
{
	hide_toolbox();
	hide_toolbar();

	if (scr->tag) {
		print_tag(scr, scr->tag, ITEM_FLG_NORMAL);
		hide_windows(scr->tag);
	}

	scr->tag = tag;
	print_tag(scr, scr->tag, ITEM_FLG_ACTIVE);

	show_windows(tag, 1);
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

	focus_tag(scr, tag);
}

static void walk_tags(void *ptr)
{
	struct arg *arg = (struct arg *) ptr;

	curscr = coord2scr(arg->x, arg->y);

	if (list_single(&curscr->tags))
		return;

	switch_tag(curscr, (enum dir) arg->kmap->arg);
	xcb_flush(dpy);
}

static void retag_client(void *ptr)
{
	struct arg *arg = (struct arg *) ptr;
	struct client *cli;
	struct client *front;

	curscr = coord2scr(arg->x, arg->y);

	if (list_single(&curscr->tags))
		return;

	if (!(cli = pointer2cli()))
		return;

	list_del(&cli->head); /* remove from current tag */
	walk_tags(arg); /* switch to next tag */

	if ((front = front_client(curscr->tag)))
		unfocus_window(front->win);

	list_add(&curscr->tag->clients, &cli->head); /* re-tag */
	cli->scr = curscr;
	cli->tag = curscr->tag;
	raise_window(cli->win);
	focus_window(cli->win);
	center_pointer(cli);
	xcb_flush(dpy);
	store_client(cli, 0);
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

static struct tag *lookup_tag(struct screen *scr, xcb_window_t win)
{
	struct sprop class;
	char *path;
	struct stat st;
	struct list_head *cur;
	struct tag *tag;

	if (!basedir)
		return NULL;

	get_sprop(&class, win, XCB_ATOM_WM_CLASS, UCHAR_MAX);
	if (!class.ptr) {
		ww("unable to detect window class\n");
		return NULL;
	}

	ii("scr %d win 0x%x class '%s'\n", scr->id, win, class.str);

	tag = NULL;
	class.len += baselen + sizeof("screens/255/tags/255/");
	path = calloc(1, class.len);

	if (!path)
		goto out;

	list_walk(cur, &scr->tags) {
		tag = list2tag(cur);
		snprintf(path, class.len, "%s/screens/%d/tags/%d/%s", basedir,
			 scr->id, tag->id, class.str);

		if (stat(path, &st) < 0)
			continue;

		return tag;
	}

out:
	free(class.ptr);
	free(path);
	return NULL;
}

static struct tag *configured_tag(xcb_window_t win)
{
	struct tag *tag;
	struct list_head *cur;

	list_walk(cur, &screens) {
		struct screen *scr = list2screen(cur);
		if ((tag = lookup_tag(scr, win)))
			return tag;
	}

	return NULL;
}

static void dock_arrange(struct screen *scr)
{
	struct list_head *cur, *tmp;
	int16_t x, y;

	scr->items[PANEL_AREA_DOCK].x = scr->x + scr->w;
	scr->items[PANEL_AREA_DOCK].w = 0;

	x = scr->items[PANEL_AREA_DOCK].x;
	y = scr->panel.y + ITEM_V_MARGIN;

	list_walk_safe(cur, tmp, &scr->dock) {
		struct client *cli = list2client(cur);
		if (window_status(cli->win) == WIN_STATUS_UNKNOWN) { /* gone */
			list_del(&cli->head);
			list_del(&cli->list);
			free(cli);
			continue;
		}
		x -= (cli->w + ITEM_H_MARGIN + 2 * BORDER_WIDTH);
		client_moveresize(cli, x, y, cli->w, cli->h);
	}

	scr->items[PANEL_AREA_DOCK].x = x;
	scr->items[PANEL_AREA_DOCK].w = scr->items[PANEL_AREA_DOCK].x - x;
	title_width(scr);
	print_title(scr, XCB_WINDOW_NONE);
}

static void dock_del(struct client *cli)
{
	list_del(&cli->head);
	list_del(&cli->list);
	dock_arrange(cli->scr);
	free(cli);
}

static void dock_add(struct client *cli, uint8_t bw)
{
	uint16_t h;
	struct list_head *head;

	if (cli->flags & CLI_FLG_TRAY)
		head = &cli->scr->dock;
	else
		head = cli->scr->dock.next;

	list_add(head, &cli->head);
	list_add(&clients, &cli->list);

	cli->flags |= CLI_FLG_DOCK;

	h = panel_height - ITEM_V_MARGIN * 2 - bw * 2;

	if (cli->flags & CLI_FLG_TRAY)
		cli->w = h;
	else if (cli->h > panel_height) /* adjust width and height */
		cli->w = panel_height + panel_height / 3;
	/* else: client respects panel height so respect client's width
	 * in return; this allows to dock e.g. xclock application
	 */

	cli->h = h;

	if (cli->flags & CLI_FLG_TRAY)
		window_state(cli->win, XCB_ICCCM_WM_STATE_NORMAL);

	if (bw > 0) { /* adjust width if client desires to have a border */
		window_border_width(cli->win, BORDER_WIDTH);
		window_border_color(cli->win, normal_bg);
		cli->flags |= CLI_FLG_BORDER;
	}

	dock_arrange(cli->scr);
	raise_window(cli->win);
	xcb_map_window(dpy, cli->win);
	xcb_flush(dpy);
}

static void del_window(xcb_window_t win)
{
	struct screen *scr;
	struct client *cli;

	if (toolbar.cli && toolbar.cli->win == win)
		hide_toolbar();
	if (toolbox.cli && toolbox.cli->win == win)
		hide_toolbox();

	scr = curscr;
	cli = win2cli(win);
	if (!cli) {
		ii("unmanaged win 0x%x\n", win);
		xcb_unmap_subwindows_checked(dpy, win);
		goto flush;
	}

	if (cli->flags & CLI_FLG_DOCK) {
		dock_del(cli);
		goto out;
	}

	scr = cli2scr(cli); /* do it before client is freed */
	free_client(cli);
	ii("deleted win 0x%x\n", win);

	if (scr)
		print_title(scr, XCB_WINDOW_NONE);

out:
	focus_any(0);

flush:
	update_client_list();
	xcb_flush(dpy);
}

static uint32_t window_exclusive(xcb_window_t win)
{
	uint32_t crc;
	struct client *cli;
	struct list_head *cur, *tmp;

	crc = window_special(win, "exclusive", sizeof("exclusive"));

	list_walk_safe(cur, tmp, &clients) {
		cli = glob2client(cur);

		if (cli->win == win) {
			ii("cleanup cli %p win 0x%x\n", cli, win);
			list_del(&cli->head);
			list_del(&cli->list);
			free(cli);
		} else if (crc && cli->crc == crc && cli->win != win) {
			xcb_window_t old = cli->win;
			close_client(cli);
			del_window(old);
			ii("exclusive win 0x%x crc 0x%x\n", win, crc);
		}
	}

	return crc;
}

static struct client *add_window(xcb_window_t win, uint8_t tray, uint8_t scan)
{
	uint32_t flags;
	struct tag *tag;
	struct screen *scr;
	struct client *cli;
	uint32_t val[1];
	uint32_t crc;
	xcb_window_t leader;
	xcb_get_geometry_reply_t *g;
	xcb_get_window_attributes_cookie_t c;
	xcb_get_window_attributes_reply_t *a;

	if (win == rootscr->root || win == toolbar.panel.win)
		return NULL;

	flags = 0;

	if (window_status(win) == WIN_STATUS_UNKNOWN) {
		ww("ignore unknown window 0x%x\n", win);
		flags |= CLI_FLG_IGNORED;
	}

	if (window_special(win, "dock", sizeof("dock")))
		flags |= CLI_FLG_DOCK;
	else if (window_special(win, "center", sizeof("center")))
		flags |= CLI_FLG_CENTER;
	else if (window_special(win, "top-left", sizeof("top-left")))
		flags |= CLI_FLG_TOPLEFT;
	else if (window_special(win, "top-right", sizeof("top-right")))
		flags |= CLI_FLG_TOPRIGHT;
	else if (window_special(win, "bottom-left", sizeof("bottom-left")))
		flags |= CLI_FLG_BOTLEFT;
	else if (window_special(win, "bottom-right", sizeof("bottom-right")))
		flags |= CLI_FLG_BOTRIGHT;

	if (tray_window(win) || tray) {
		ii("win 0x%x provides embed info\n", win);
		flags |= CLI_FLG_TRAY;
	}

	if (window_special(win, "popup", sizeof("popup"))) {
		flags &= ~(CLI_FLG_DOCK | CLI_FLG_TRAY);
		flags |= CLI_FLG_POPUP;
	}

	if (!(flags & (CLI_FLG_DOCK | CLI_FLG_TRAY)))
		crc = window_exclusive(win);

	cli = NULL;
	a = NULL;
	g = xcb_get_geometry_reply(dpy, xcb_get_geometry(dpy, win), NULL);

	if (!g) { /* could not get initial geometry */
		ee("xcb_get_geometry() failed\n");
		goto out;
	}

	if (g->width <= WIN_WIDTH_MIN || g->height <= WIN_HEIGHT_MIN) {
		/* FIXME: current assumption is that window is not supposed
		 * to be shown
		 */
		ww("ignore tiny window 0x%x geo %ux%u%+d%+d\n", win, g->width,
		   g->height, g->x, g->y);
		xcb_change_property(dpy, XCB_PROP_MODE_APPEND, rootscr->root,
				    a_client_list, XCB_ATOM_WINDOW, 32, 1, &win);
		xcb_flush(dpy);
		goto out;
	}

	leader = window_leader(win);

	if (leader != XCB_WINDOW_NONE && !win2cli(leader)) {
		ww("ignore win 0x%x with hidden leader 0x%x\n", win, leader);
		flags |= CLI_FLG_IGNORED;
	}

	scr = NULL;
	tag = NULL;

	if (scan && !(flags & (CLI_FLG_TRAY | CLI_FLG_DOCK)))
		restore_window(win, &scr, &tag);

	if (!scr && !(scr = pointer2scr())) {
		if (panel_window(win)) {
			goto out;
		} else if ((cli = win2cli(win))) {
			ii("win 0x%x already on clients list\n", win);
			list_del(&cli->head);
			list_del(&cli->list);
		}
		scr = defscr;
	} else {
		if (scr->panel.win == win) {
			goto out; /* don't handle it here */
		} else if ((cli = scr2cli(scr, win, WIN_TYPE_DOCK))) {
			ii("win 0x%x already on dock list\n", win);
			list_del(&cli->head);
			list_del(&cli->list);
		} else if ((cli = scr2cli(scr, win, WIN_TYPE_NORMAL))) {
			ii("win 0x%x already on [%s] list\n", win,
			   scr->tag->name);
			list_del(&cli->head);
			list_del(&cli->list);
		}
	}

	tt("screen %d, win 0x%x, geo %ux%u+%d+%d\n", scr->id, win, g->width,
	   g->height, g->x, g->y);

	scr->flags &= ~SCR_FLG_SWITCH_WINDOW;

	c = xcb_get_window_attributes(dpy, win);
	a = xcb_get_window_attributes_reply(dpy, c, NULL);
	if (!a) {
		ee("xcb_get_window_attributes() failed\n");
		goto out;
	}

	if (!g->depth && !a->colormap) {
		tt("win %p, root %p, colormap=%p, class=%u, depth=%u\n",
		   win, g->root, a->colormap, a->_class, g->depth);
		xcb_destroy_window_checked(dpy, win);
		ww("zombie window 0x%x destroyed\n", win);
		goto out;
	}

	if (!(flags & CLI_FLG_TRAY) && a->override_redirect) {
		ww("ignore redirected window 0x%x\n", win);
		xcb_change_property(dpy, XCB_PROP_MODE_APPEND, rootscr->root,
				    a_client_list, XCB_ATOM_WINDOW, 32, 1,
				    &win);
		xcb_flush(dpy);
		goto out;
	}

	/* tell x server to restore window upon our sudden exit */
	xcb_change_save_set_checked(dpy, XCB_SET_MODE_INSERT, win);

	if (!cli) {
		cli = calloc(1, sizeof(*cli));
		if (!cli) {
			ee("calloc(%lu) failed\n", sizeof(*cli));
			goto out;
		}
	}

	cli->div = 1;
	cli->scr = scr;
	cli->win = win;
	cli->crc = crc;
	cli->flags = flags;

#define DONT_CENTER (\
	CLI_FLG_TOPLEFT |\
	CLI_FLG_TOPRIGHT |\
	CLI_FLG_BOTLEFT |\
	CLI_FLG_BOTRIGHT |\
	CLI_FLG_TRAY |\
	CLI_FLG_DOCK\
)

	if (!scan && g->width < scr->w / 2 && g->height < scr->h / 2 &&
	    !(flags & DONT_CENTER)) {
		cli->flags |= CLI_FLG_CENTER; /* show such windows in center */
	}

	if (cli->flags & CLI_FLG_CENTER) {
		g->x = scr->x + scr->w / 2 - g->width / 2;
		g->y = scr->top + scr->h / 2 - g->height / 2;
	} else if (cli->flags & CLI_FLG_TOPLEFT) {
		g->x = 0;
		g->y = 0;
	} else if (cli->flags & CLI_FLG_TOPRIGHT) {
		g->x = scr->w - g->width - 2 * BORDER_WIDTH;
		g->y = 0;
	} else if (cli->flags & CLI_FLG_BOTLEFT) {
		g->x = 0;
		g->y = scr->h - g->height - 2 * BORDER_WIDTH - 1;
	} else if (cli->flags & CLI_FLG_BOTRIGHT) {
		g->x = scr->w - g->width - 2 * BORDER_WIDTH;
		g->y = scr->h - g->height - 2 * BORDER_WIDTH - 1;
	} else if (cli->flags & CLI_FLG_TRAY || cli->flags & CLI_FLG_DOCK) {
		cli->w = g->width;
		cli->h = g->height;
		dock_add(cli, g->border_width);
		goto out;
	} else if (!scan) {
		g->x += scr->x;
		g->y += scr->y;
	}

	cli->tag = configured_tag(win); /* read tag from configuration */

	if (!cli->tag && tag) /* not configured, restore from last session */
		cli->tag = tag;
	else if (!cli->tag) /* nothing worked, map to current tag */
		cli->tag = scr->tag;

	window_border_width(cli->win, BORDER_WIDTH);

	/* subscribe events */
	val[0] = XCB_EVENT_MASK_ENTER_WINDOW;
	val[0] |= XCB_EVENT_MASK_PROPERTY_CHANGE;
	val[0] |= XCB_EVENT_MASK_STRUCTURE_NOTIFY;

	if (cli->flags & CLI_FLG_POPUP)
		val[0] |= XCB_EVENT_MASK_LEAVE_WINDOW;

	xcb_change_window_attributes_checked(dpy, win, XCB_CW_EVENT_MASK, val);
	unfocus_clients(curscr->tag);
	list_add(&cli->tag->clients, &cli->head);
	cli->pid = win2pid(win);
	list_add(&clients, &cli->list); /* also add to global list of clients */
	client_moveresize(cli, g->x, g->y, g->width, g->height);

	if (scr->tag != cli->tag) {
		window_state(cli->win, XCB_ICCCM_WM_STATE_ICONIC);
		xcb_unmap_window_checked(dpy, cli->win);
	} else {
		struct client *tmp;
		if ((tmp = pointer2cli()))
			resort_client(tmp);
		window_state(cli->win, XCB_ICCCM_WM_STATE_NORMAL);
		xcb_map_window_checked(dpy, cli->win);
		center_pointer(cli);
	}

	ii("screen %d tag '%s' win 0x%x pid %d geo %ux%u%+d%+d cli %p\n",
	   scr->id, cli->tag->name, cli->win, cli->pid, cli->w, cli->h,
	   cli->x, cli->y, cli);

	if (!(flags & (CLI_FLG_TRAY | CLI_FLG_DOCK))) {
		store_client(cli, 0);
		if (!scan) {
			focus_window(cli->win);
			raise_window(cli->win);
		}
	}

	update_client_list();
	xcb_flush(dpy);
out:
	free(a);
	free(g);
	return cli;
}

static int screen_panel(xcb_window_t win)
{
	struct list_head *cur;

	list_walk(cur, &screens) {
		struct screen *scr = list2screen(cur);
		if (scr->panel.win == win)
			return 1;
	}
	return 0;
}

static void hide_leader(xcb_window_t win)
{
	xcb_window_t leader = window_leader(win);

	if (leader != XCB_WINDOW_NONE) {
		struct client *cli = win2cli(leader);
		if (cli) {
			list_del(&cli->head);
			list_del(&cli->list);
			free(cli);
		}
		xcb_unmap_window_checked(dpy, leader);
	}
}

static void scan_clients(void)
{
	int i, n;
	xcb_query_tree_cookie_t c;
	xcb_query_tree_reply_t *tree;
	xcb_window_t *wins;
	struct client *cli;

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
		if (screen_panel(wins[i]))
			continue;
		add_window(wins[i], 0, 1);
		/* gotta do this otherwise empty windows are being shown
		 * in certain situations e.g. when adding systray clients
		 */
		hide_leader(wins[i]);
	}

	if ((cli = front_client(curscr->tag))) {
		raise_window(cli->win);
		focus_window(cli->win);
		center_pointer(cli);
	}

	free(tree);
	xcb_flush(dpy);
}

static void print_menu(struct screen *scr)
{
	uint16_t x, w;

	x = scr->items[PANEL_AREA_MENU].x;
	w = scr->items[PANEL_AREA_MENU].w;

	draw_panel_text(&scr->panel, color2ptr(NORMAL_FG),
			color2ptr(NORMAL_BG), x, w, menu_icon, menu_icon_len,
			menu_font, ITEM_H_MARGIN);
}

static void print_div(struct screen *scr)
{
	uint16_t x, w;

	x = scr->items[PANEL_AREA_DIV].x;
	w = scr->items[PANEL_AREA_DIV].w;

	draw_panel_text(&scr->panel, color2ptr(NORMAL_FG),
			color2ptr(NORMAL_BG), x, w, DIV_ICON,
			sizeof(DIV_ICON) - 1, font1, ITEM_H_MARGIN);
}

static int tag_pointed(struct tag *tag, int16_t x, int16_t y)
{
	if (tag && 0 <= y && y <= curscr->panel.y + panel_height &&
	    x >= tag->x && x <= tag->x + tag->w + TAG_GAP) {
		return 1;
	}

	return 0;
}

static void motion_retag(int16_t x, int16_t y)
{
	struct client *cli;
	struct list_head *cur;
	struct tag *tag;

	if (!motion_cli)
		return;

	tag = NULL;
	cli = motion_cli;
	motion_cli = NULL;

	list_walk(cur, &curscr->tags) {
		tag = list2tag(cur);

		if (tag_pointed(tag, x, y))
			break;

		tag = NULL;
	}

	if (!tag)
		return;

	switch_window(curscr, DIR_NEXT); /* focus window */
	list_del(&cli->head);
	list_add(&tag->clients, &cli->head); /* re-tag window */

	if (cli->scr != curscr) {
		/* new screen can have different size so place window in
		 * top-left corner */
		x = curscr->x;
		y = curscr->y;
	} else { /* use initial coords */
		x = motion_init_x;
		y = motion_init_y;
	}

	motion_init_x = motion_init_y = 0;

	cli->scr = curscr;
	cli->tag = tag;

	client_moveresize(cli, x, y, cli->w, cli->h);
	window_state(cli->win, XCB_ICCCM_WM_STATE_ICONIC);
	xcb_unmap_window_checked(dpy, cli->win);
	xcb_flush(dpy);
	store_client(cli, 0);
}

static void select_tag(struct screen *scr, int16_t x, int16_t y)
{
	struct list_head *cur;
	struct tag *prev;

	hide_toolbox();
	hide_toolbar();

	if (scr->tag && tag_pointed(scr->tag, x, y))
		return;
	else if (scr->tag) /* deselect current tag instantly */
		print_tag(scr, scr->tag, ITEM_FLG_NORMAL);

	prev = scr->tag;

	list_walk(cur, &scr->tags) { /* refresh labels */
		struct tag *tag = list2tag(cur);

		if (!tag_pointed(tag, x, y)) {
			print_tag(scr, tag, ITEM_FLG_NORMAL);
		} else {
			print_tag(scr, tag, ITEM_FLG_ACTIVE);
			scr->tag = tag;
			break;
		}
	}

	if (scr->tag && scr->tag != prev) {
		if (prev)
			hide_windows(prev);

		show_windows(scr->tag, 0);
	}

	xcb_set_input_focus(dpy, XCB_NONE, scr->panel.win, XCB_CURRENT_TIME);
}

static struct tag *tag_get(struct screen *scr, const char *name, uint8_t id)
{
	struct list_head *cur;
	struct tag *tag;

	list_walk(cur, &scr->tags) {
		tag = list2tag(cur);
		if (tag->id == id) {
			if (tag->name && strcmp(tag->name, name) == 0)
				return tag;
			else if (tag->name)
				free(tag->name);

			/* clean area */
			fill_rect(scr->panel.win, scr->panel.gc,
				  color2ptr(NORMAL_BG),
				  tag->x, 0, tag->w, panel_height);

			tag->nlen = strlen(name);
			tag->name = strdup(name);
			return tag;
		}
	}

	tag = calloc(1, sizeof(*tag));
	if (!tag) {
		ee("calloc() failed\n");
		return NULL;
	}

	tag->id = id;
	tag->nlen = strlen(name);
	tag->name = strdup(name);
	if (!tag->name) {
		ee("strdup(%s) failed\n", name);
		free(tag);
		return NULL;
	}

	list_init(&tag->clients);
	list_add(&scr->tags, &tag->head);

	struct tag *tmp = scr->tag;
	scr->tag = tag; /* this is temporary for space calculations */
	recalc_space(scr, 0);
	scr->tag = tmp;

	return tag;
}

static int tag_add(struct screen *scr, const char *name, uint8_t id,
		   uint16_t pos)
{
	uint8_t flg;
	struct tag *tag;
	uint16_t h;

	tag = tag_get(scr, name, id);
	if (!tag)
		return 0;

	text_exts(name, tag->nlen, &tag->w, &h, font1);
	tag->w += ITEM_H_MARGIN * 2;
	ii("tag '%s' len %u width %u\n", name, tag->nlen, tag->w);

	if (pos != scr->items[PANEL_AREA_TAGS].x) {
		flg = ITEM_FLG_NORMAL;
	} else {
		flg = ITEM_FLG_ACTIVE;
		scr->tag = tag;
	}

	tag->x = pos;
	tag->flags = 0;
	print_tag(scr, tag, flg);

	return pos + tag->w + TAG_GAP;
}

static int tag_del(struct screen *scr, uint8_t id)
{
	struct list_head *cur;

	list_walk(cur, &scr->tags) {
		struct tag *tag = list2tag(cur);

		if (tag->id != id)
			continue;

		if (!list_empty(&tag->clients))
			return 0; /* do not delete non-empty tag */

		list_del(&tag->head);

		if (tag->name)
			free(tag->name);

		free(tag);
		return 1;
	}

	return 1; /* not found */
}

/*
 * Tags dir structure:
 *
 * /<basedir>/screens/<screennumber>/tags/<tagnumber>/{.name,<winclass1>,<winclassN>}
 */

static int init_tags(struct screen *scr)
{
	uint16_t pos;
	uint8_t i;
	strlen_t len = baselen + sizeof("screens/255/tags/255/.name");
	char path[len];
	char name[TAG_NAME_MAX + 1] = "0";
	int fd;
	struct stat st;
	uint8_t delete;

	pos = scr->items[PANEL_AREA_TAGS].x;
	if (!basedir) {
		ww("base directory is not set\n");
		goto out;
	}

	for (i = 0; i < UCHAR_MAX; i++ ) {
		delete = 0;
		st.st_mode = 0;
		sprintf(path, "%s/screens/%d/tags/%d", basedir, scr->id, i);

		if (stat(path, &st) < 0)
			delete = 1;
		if ((st.st_mode & S_IFMT) != S_IFDIR)
			delete = 1;

		if (delete && tag_del(scr, i))
			continue;

		sprintf(path, "%s/screens/%d/tags/%d/.name", basedir, scr->id, i);
		fd = open(path, O_RDONLY);
		if (fd > 0) {
			read(fd, name, sizeof(name) - 1);
			close(fd);
		}

		if (name[0] == '\0')
			snprintf(name, sizeof(name), "%d", i);
		else if (name[strlen(name) - 1] == '\n')
			name[strlen(name) - 1] = '\0';

		tt("screen %d tag %d name %s\n", scr->id, i, name);
		pos = tag_add(scr, name, i,  pos);
		memset(name, 0, TAG_NAME_MAX);
	}

out:
	if (pos == scr->items[PANEL_AREA_TAGS].x) /* add default tag */
		pos = tag_add(scr, "*", 0, pos);

	return pos;
}

static void redraw_panel_items(struct screen *scr)
{
	struct list_head *cur;

	/* clean panel */
	fill_rect(scr->panel.win, scr->panel.gc, color2ptr(NORMAL_BG), scr->x,
		  0, scr->w, panel_height);

	print_menu(scr);

	list_walk(cur, &scr->tags) {
		uint8_t flg;
		struct tag *tag = list2tag(cur);

		tag->flags = 0; /* reset tag */

		if (scr->tag == tag)
			flg = ITEM_FLG_ACTIVE;
		else
			flg = ITEM_FLG_NORMAL;

		print_tag(scr, tag, flg);
	}

	print_div(scr);
	print_title(scr, XCB_WINDOW_NONE);
}

static void update_panel_items(struct screen *scr)
{
	int16_t x = 0;
	uint16_t h, w;

	/* clean panel */
	fill_rect(scr->panel.win, scr->panel.gc, color2ptr(NORMAL_BG), scr->x,
		  0, scr->w, panel_height);

	text_exts(menu_icon, menu_icon_len, &w, &h, menu_font);

	scr->items[PANEL_AREA_MENU].x = TAG_GAP;
	scr->items[PANEL_AREA_MENU].w = w + 2 * ITEM_H_MARGIN;
	x += scr->items[PANEL_AREA_MENU].w + 2 * TAG_GAP;
	print_menu(scr);

	scr->items[PANEL_AREA_TAGS].x = x;
	scr->items[PANEL_AREA_TAGS].w = init_tags(scr);

	scr->items[PANEL_AREA_DIV].x = scr->items[PANEL_AREA_TAGS].w;
	text_exts(DIV_ICON, sizeof(DIV_ICON) - 1, &w, &h, font1);
	w += scr->items[PANEL_AREA_DIV].x + 2 * ITEM_H_MARGIN;
	scr->items[PANEL_AREA_DIV].w = w;
	print_div(scr);

	scr->items[PANEL_AREA_TITLE].x = scr->items[PANEL_AREA_DIV].w;

	dock_arrange(scr);
}

static void refresh_panel(uint8_t id)
{
	struct list_head *cur;

	list_walk(cur, &screens) {
		struct screen *scr = list2screen(cur);
		if (scr->id != id)
			continue;

		print_tag(scr, scr->tag, ITEM_FLG_NORMAL);
		hide_windows(scr->tag);
		update_panel_items(scr);
		focus_tag(scr, list2tag(scr->tags.next));
		break;
	}
}

static void grab_key(xcb_key_symbols_t *syms, struct keymap *kmap)
{
	xcb_void_cookie_t c;
	xcb_generic_error_t *e;
	xcb_keycode_t *key;

	key = xcb_key_symbols_get_keycode(syms, kmap->sym);
	if (!key) {
		ee("xcb_key_symbols_get_keycode(sym=0x%x) failed\n", kmap->sym);
		return;
	}

	kmap->key = *key;
	free(key);

	c = xcb_grab_key_checked(dpy, 1, rootscr->root, kmap->mod, kmap->key,
				 XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
	e = xcb_request_check(dpy, c);
        if (e) {
		ee("xcb_grab_key_checked() failed, err=%d\n", e->error_code);
		kmap->key = 0;
	}

#ifdef TRACE
	if (!e) {
		ii("grab mod 0x%x + key 0x%x (sym=0x%x)\n", kmap->mod,
		   kmap->key, kmap->sym);
	}
#endif
}

static void map_key(const char *path, xcb_key_symbols_t *syms, uint16_t mod,
		    xcb_keysym_t sym, char *action, const char *keyname)
{
	int fd, n;
	struct keymap *kmap;
	struct list_head *cur;

	fd = open(path, O_RDONLY);
	if (!fd) {
		ee("open(%s) failed\n", path);
		return;
	}

	n = read(fd, action, actname_max);
	if (n < 0) {
		ee("read(%s) failed\n", path);
		goto out;
	} else if (n < 1) {
		ww("%s: zero bytes read, no action\n", path);
		goto out;
	}
	action[n] = '\0';

	/* first check if we just re-bind existing action */
	list_walk(cur, &keymap) {
		kmap = list2keymap(cur);
		if (strcmp(kmap->actname, action) != 0)
			continue;

		kmap->mod = mod;
		kmap->sym = sym;

		grab_key(syms, kmap);
		if (!kmap->key) {
			ww("nack re-map %s to %s\n", kmap->actname, keyname);
		} else {
			if (kmap->alloc)
				free(kmap->keyname);
			kmap->alloc = 1;
			kmap->keyname = strdup("keyname");
			tt("re-map %s to %s\n", kmap->actname, keyname);
		}
		goto out;
	}

	/* not really, add new spawn binding */
	kmap = calloc(1, sizeof(*kmap));
	if (!kmap)
		goto out;

	kmap->mod = mod;
	kmap->sym = sym;

	grab_key(syms, kmap);
	if (!kmap->key) {
		free(kmap);
	} else {
		kmap->alloc = 1;
		kmap->keyname = strdup(keyname);
		kmap->actname = "spawn";
		kmap->action = spawn;
		list_add(&keymap, &kmap->head);
		tt("map %s to %s\n", kmap->actname, keyname);
	}
out:
	close(fd);
}

static void init_toolbar_keys(xcb_key_symbols_t *syms)
{
	xcb_keycode_t *key;

	key = xcb_key_symbols_get_keycode(syms, XK_Left);
	if (!key) {
		ee("xcb_key_symbols_get_keycode(sym=0x%x) failed\n", XK_Left);
		return;
	}

	toolbar.kprev = *key;
	free(key);

	key = xcb_key_symbols_get_keycode(syms, XK_Right);
	if (!key) {
		ee("xcb_key_symbols_get_keycode(sym=0x%x) failed\n", XK_Right);
		return;
	}

	toolbar.knext = *key;
	free(key);

	key = xcb_key_symbols_get_keycode(syms, XK_Return);
	if (!key) {
		ee("xcb_key_symbols_get_keycode(sym=0x%x) failed\n", XK_Return);
		return;
	}

	toolbar.kenter = *key;
	free(key);

	key = xcb_key_symbols_get_keycode(syms, XK_Escape);
	if (!key) {
		ee("xcb_key_symbols_get_keycode(sym=0x%x) failed\n", XK_Escape);
		return;
	}

	toolbar.kclose = *key;
	free(key);
}

static void init_keys(void)
{
	int tmp;
	uint16_t len = baselen + sizeof("/keys/") + UCHAR_MAX;
	char path[len], *ptr;
	char buf[actname_max];
	struct stat st;
	xcb_key_symbols_t *syms;
	uint8_t i;

	if (!basedir)
		return;

	syms = xcb_key_symbols_alloc(dpy);
	if (!syms) {
		ee("xcb_key_symbols_alloc() failed\n");
		return;
	}

	snprintf(path, len, "%s/keys/", basedir);
	tmp = strlen(path);
	ptr = path + tmp;
	len -= tmp;
	for (i = 33; i < 127; i++) {
		snprintf(ptr, len, "mod_%c", i);
		if (stat(path, &st) < 0)
			continue;
		map_key(path, syms, MOD, i, buf, ptr);
	}
	for (i = 1; i < 13; i++) {
		snprintf(ptr, len, "mod_f%u", i);
		if (stat(path, &st) < 0)
			continue;
		map_key(path, syms, MOD, XK_F1 + (i - 1), buf, ptr);
	}
	for (i = 1; i < 13; i++) {
		snprintf(ptr, len, "shift_f%u", i);
		if (stat(path, &st) < 0)
			continue;
		map_key(path, syms, SHIFT, XK_F1 + (i - 1), buf, ptr);
	}
	for (i = 0; i < ARRAY_SIZE(kmap_def); i++) {
		snprintf(ptr, len, "%s", kmap_def[i].keyname);
		if (stat(path, &st) < 0)
			continue;
		ii("path: %s\n", path);
	}
	snprintf(ptr, len, "%s", "ctrl_alt_delete");
	if (stat(path, &st) == 0)
		map_key(path, syms, CTRL | ALT, XK_Delete, buf, ptr);

	init_toolbar_keys(syms);

	xcb_key_symbols_free(syms);
}

static void init_panel(struct screen *scr)
{
	uint32_t val[2], mask;

	panel_height = font1->ascent + font1->descent + 2 * ITEM_V_MARGIN;

	if (panel_height % 2)
		panel_height++;

	scr->panel.win = xcb_generate_id(dpy);

	mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	val[0] = color2int(NORMAL_BG);
	val[1] = XCB_EVENT_MASK_BUTTON_PRESS;
	val[1] |= XCB_EVENT_MASK_BUTTON_RELEASE;
	val[1] |= XCB_EVENT_MASK_POINTER_MOTION;
	val[1] |= XCB_EVENT_MASK_VISIBILITY_CHANGE;
	val[1] |= XCB_EVENT_MASK_EXPOSURE;

	if (panel_top) {
		scr->panel.y = scr->y;
		scr->top = scr->panel.y + panel_height + PANEL_SCREEN_GAP;
	} else {
		scr->panel.y = (scr->h + scr->y) - panel_height;
		scr->top = scr->y;
	}

	xcb_create_window(dpy, XCB_COPY_FROM_PARENT, scr->panel.win,
			  rootscr->root,
			  scr->x, scr->panel.y, scr->w, panel_height, 0,
			  XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  rootscr->root_visual, mask, val);
	xcb_flush(dpy); /* flush this operation otherwise panel will be
			   misplaced in multiscreen setup */

	xcb_change_property(dpy, XCB_PROP_MODE_REPLACE, scr->panel.win,
			    XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
			    sizeof("yawmpanel") - 1, "yawmpanel");

	xcb_change_property(dpy, XCB_PROP_MODE_REPLACE, scr->panel.win,
			    XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
			    sizeof("yawmpanel") - 1, "yawmpanel");

	scr->panel.gc = xcb_generate_id(dpy);
	mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND;
	val[0] = val[1] = color2int(NORMAL_BG);
	xcb_create_gc(dpy, scr->panel.gc, scr->panel.win, mask, val);

	xcb_map_window(dpy, scr->panel.win);

	/* now correct screen height */
	scr->h -= panel_height + PANEL_SCREEN_GAP;

	scr->panel.draw = XftDrawCreate(xdpy, scr->panel.win,
					DefaultVisual(xdpy, xscr),
					DefaultColormap(xdpy, xscr));

	ii("screen %d, panel 0x%x geo %ux%u+%d+%d\n", scr->id, scr->panel.win,
	   scr->w, panel_height, scr->x, scr->panel.y);
}

static void move_panel(struct screen *scr)
{
	uint32_t val[3];
	uint16_t mask;

	val[0] = scr->x;

	if (panel_top)
		val[1] = scr->y;
	else
		val[1] = scr->h + scr->y;

	val[2] = scr->w;

	mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
	mask |= XCB_CONFIG_WINDOW_WIDTH;
	xcb_configure_window(dpy, scr->panel.win, mask, val);
	update_panel_items(scr);
	xcb_flush(dpy);
}

static void tray_notify(xcb_atom_t atom)
{
	xcb_client_message_event_t e = { 0 };

	e.response_type = XCB_CLIENT_MESSAGE;
	e.window = rootscr->root;
	e.type = XCB_ATOM_RESOURCE_MANAGER;
	e.format = 32;
	e.data.data32[0] = XCB_CURRENT_TIME;
	e.data.data32[1] = atom;
	e.data.data32[2] = defscr->panel.win;

	xcb_send_event(dpy, 0, rootscr->root, 0xffffff, (void *) &e);
}

static void init_tray(void)
{
	xcb_get_selection_owner_cookie_t c;
	xcb_get_selection_owner_reply_t *r;
	xcb_atom_t a_tray;
	char *name = xcb_atom_name_by_screen("_NET_SYSTEM_TRAY", defscr->id);

	if (!name) {
		ee("failed to get systray atom name\n");
		return;
	}

	a_tray = get_atom_by_name(name, strlen(name));
	free(name);

	ii("tray atom %d\n", a_tray);
	print_atom_name(a_tray);
	xcb_set_selection_owner(dpy, defscr->panel.win, a_tray,
				XCB_CURRENT_TIME);

	/* verify selection */
	c = xcb_get_selection_owner(dpy, a_tray);
	r = xcb_get_selection_owner_reply(dpy, c, NULL);
	if (!r) {
		ee("xcb_get_selection_owner(%s) failed\n", name);
		return;
	}

	if (r->owner != defscr->panel.win)
		ww("systray owned by win 0x%x scr %d\n", r->owner, defscr->id);
	else
		tray_notify(a_tray);

	free(r);
}

static void screen_add(uint8_t id, xcb_randr_output_t out,
		       int16_t x, int16_t y, uint16_t w, uint16_t h,
		       char *name)
{
	struct screen *scr;

	scr = calloc(1, sizeof(*scr));
	if (!scr)
		panic("calloc(%lu) failed\n", sizeof(*scr));

	scr->id = id;
	scr->out = out;
	scr->x = x;
	scr->y = y;
	scr->w = w;
	scr->h = h;
	scr->name = strdup(name);

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

	ii("add screen %d (%p), size %dx%d+%d+%d\n", scr->id, scr, scr->w,
	   scr->h, scr->x, scr->y);
}

static void init_crtc(uint8_t i, uint8_t *id, xcb_randr_output_t out,
		      xcb_randr_get_output_info_reply_t *inf,
		      xcb_timestamp_t ts, char *name)
{
	struct list_head *cur;
	xcb_randr_get_crtc_info_cookie_t c;
	xcb_randr_get_crtc_info_reply_t *r;

	c = xcb_randr_get_crtc_info(dpy, inf->crtc, ts);
	r = xcb_randr_get_crtc_info_reply(dpy, c, NULL);
	if (!r)
		return;

	ii("crtc%d geo %ux%u%+d%+d\n", i, r->width, r->height, r->x, r->y);

	/* find a screen that matches new geometry so we can re-use it */
	list_walk(cur, &screens) {
		struct screen *scr = list2screen(cur);

		if (r->width == scr->w && r->height == scr->h + panel_height &&
		    r->x == scr->x && r->y == scr->y) {
			ii("crtc%d is a clone of screen %d\n", i, scr->id);
			free(scr->name);
			scr->name = strdup(name);
			goto out;
		}
	}

	/* adapt request screen geometry if none found */
	list_walk(cur, &screens) {
		struct screen *scr = list2screen(cur);
		if (scr->out == out) {
			ii("crtc%d, screen %d: "
			   "old geo %ux%u+%d+%d, "
			   "new geo %ux%u+%d+%d\n",
			   i, scr->id,
			   scr->w, scr->h + panel_height, scr->x, scr->y,
			   r->width, r->height, r->x, r->y);
			scr->x = r->x;
			scr->w = r->width;
			scr->y = r->y;
			scr->h = r->height;
			move_panel(scr);
			free(scr->name);
			scr->name = strdup(name);
			goto out;
		}
	}

	/* one screen per output; share same root window via common
	 * xcb_screen_t structure
	 */
	screen_add(*id, out, r->x, r->y, r->width, r->height, name);

out:
	(uint8_t)(*id)++;
	free(r);
}

static void init_output(uint8_t i, uint8_t *id, xcb_randr_output_t out,
			xcb_timestamp_t ts)
{
	char name[UCHAR_MAX];
	strlen_t len;
	xcb_randr_get_output_info_cookie_t c;
	xcb_randr_get_output_info_reply_t *r;

	c = xcb_randr_get_output_info(dpy, out, ts);
	r = xcb_randr_get_output_info_reply(dpy, c, NULL);
	if (!r)
		return;

	len = xcb_randr_get_output_info_name_length(r) + 1;
	if (len > sizeof(name))
		len = sizeof(name);
	snprintf(name, len, "%s", xcb_randr_get_output_info_name(r));

	ii("output %s, size %ux%u\n", name, r->mm_width, r->mm_height);

	if (r->connection != XCB_RANDR_CONNECTION_CONNECTED)
		ii("output %s%d not connected\n", name, i);
	else
		init_crtc(i, id, out, r, ts, name);

	free(r);
}

static void init_outputs(void)
{
	struct stat st;
	struct screen *scr;
	struct list_head *cur, *tmp;
	xcb_randr_get_screen_resources_current_cookie_t c;
	xcb_randr_get_screen_resources_current_reply_t *r;
	xcb_randr_output_t *out;
	int len;
	uint8_t id, i;

	c = xcb_randr_get_screen_resources_current(dpy, rootscr->root);
	r = xcb_randr_get_screen_resources_current_reply(dpy, c, NULL);
	if (!r) {
		ii("RandR extension is not present\n");
		return;
	}

	len = xcb_randr_get_screen_resources_current_outputs_length(r);
	out = xcb_randr_get_screen_resources_current_outputs(r);
	ii("found %d screens\n", len);

	if (stat("panel/top", &st) == 0) {
		panel_top = 1;
		menu_icon = MENU_ICON_DOWN;
		menu_icon_len = sizeof(MENU_ICON_DOWN) - 1;
	} else {
		panel_top = 0;
		menu_icon = MENU_ICON_UP;
		menu_icon_len = sizeof(MENU_ICON_UP) - 1;
	}

	/* reset geometry of previously found screens */
	list_walk_safe(cur, tmp, &screens) {
		scr = list2screen(cur);
		scr->x = scr->y = scr->w = scr->h = 0;
	}

	id = 0;
	for (i = 0; i < len; i++)
		init_output(i, &id, out[i], r->config_timestamp);

	free(r);

	if (list_empty(&screens)) { /* randr failed or not supported */
		screen_add(0, 0, 0, 0, rootscr->width_in_pixels,
			   rootscr->height_in_pixels, "-");
	}

	if (!defscr)
		defscr = list2screen(screens.next); /* use first one */
	if (!curscr)
		curscr = defscr;

	/* force refresh all panels */
	list_walk(cur, &screens) {
		scr = list2screen(cur);
		update_panel_items(scr);
	}

	trace_screens();
	init_tray();
	scan_clients();
}

static void load_color(const char *path, struct color *color)
{
	uint32_t val;
	XRenderColor ref;
	uint8_t buf[sizeof("0xffffff")];
	int fd;
	FILE *f;

	fd = open(path, O_RDONLY);
	if (fd < 3) {
		val = color->def;
		if ((f = fopen(path, "w"))) {
			fprintf(f, "0x%x", val);
			fclose(f);
		}
	} else { /* at least 0x0 */
		memset(buf, 0, sizeof(buf));
		read(fd, buf, sizeof(buf));
		close(fd);
		val = strtol((const char *) buf, NULL, 16);
	}

	if (color->type == COLOR_TYPE_INT) {
		*((uint32_t *) color->val) = val;
	} else {
		color->def = val;
		ref.alpha = 0xffff;
		ref.red = (val & 0xff0000) >> 8;
		ref.green = val & 0xff00;
		ref.blue = (val & 0xff) << 8;
		XftColorAllocValue(xdpy, DefaultVisual(xdpy, xscr),
				   DefaultColormap(xdpy, xscr), &ref,
				   color->val);
	}
}

static void init_colors(void)
{
	uint16_t len = baselen + sizeof("/colors/") + UCHAR_MAX;
	char path[len];
	struct color *ptr = defcolors;

	while (ptr->fname) {
		snprintf(path, len, "%s/colors/%s", basedir, ptr->fname);
		load_color(path, ptr++);
	}
}

static void focus_screen(uint8_t id)
{
	struct list_head *cur;

	list_walk(cur, &screens) {
		struct screen *scr = list2screen(cur);
		if (scr->id == id) {
			ii("focus screen %u tag %s", id, scr->tag->name);
			focus_tag(scr, scr->tag);
			curscr = scr;
			warp_pointer(rootscr->root, curscr->x + curscr->w / 2,
				     curscr->top + curscr->h / 2);
			return;
		}
	}
}

static void focus_window_req(xcb_window_t win)
{
	struct list_head *cur;

	if (errno != 0) {
		ee("failed to focus win 0x%x\n", win);
		return;
	}

	if (win == XCB_WINDOW_NONE)
		return;

	list_walk(cur, &clients) {
		struct client *cli = glob2client(cur);
		if (cli->win == win) {
			struct screen *scr;

			if (cli->flags & CLI_FLG_IGNORED) {
				ww("win 0x%x is ignored\n", cli->win);
				return;
			}

			if (!(scr = cli2scr(cli))) {
				ww("cannot focus offscreen win 0x%x\n", win);
				return;
			}

			if (!cli->tag) {
				ww("cannot focus tagless win 0x%x\n", win);
				return;
			}

			curscr = scr;
			focus_tag(curscr, cli->tag);
			raise_window(cli->win);
			focus_window(cli->win);
			center_pointer(cli);

			ii("focus screen %u tag %s win 0x%x",
			   curscr->id, cli->tag ? cli->tag->name : "<nil>",
			   win);
			return;
		}
	}
}

static uint32_t clients_list_seq;

static void update_seq()
{
	char path[baselen + sizeof("/tmp/.seq")];
	FILE *f;

	sprintf(path, "%s/tmp/.seq", basedir); /* NOTE: path storage re-used */

	if (!(f = fopen(path, "w+"))) {
		ee("fopen(%s) failed, %s\n", path, strerror(errno));
		return;
	}

	fprintf(f, "%u\n", clients_list_seq++);
	fclose(f);
}

static void dump_tags(void)
{
	struct list_head *cur;
	char path[baselen + sizeof("/tmp/tags")];
	FILE *f;

	sprintf(path, "%s/tmp/tags", basedir);

	if (!(f = fopen(path, "w+"))) {
		ee("fopen(%s) failed, %s\n", path, strerror(errno));
		return;
	}

	list_walk(cur, &screens) {
		struct list_head *curtag;
		struct screen *scr = list2screen(cur);

		list_walk(curtag, &scr->tags) {
			struct list_head *cli;
			struct tag *tag = list2tag(curtag);
			char current;
			uint16_t clicnt = 0;

			list_walk(cli, &tag->clients)
				clicnt++;

			curscr->tag == tag ? (current = '*') : (current = ' ');
			fprintf(f, "%u\t%u\t%s\t%ux%u%+d%+d\t%u\t%c\n", scr->id,
				tag->id, tag->name, tag->w, panel_height,
				tag->x, scr->panel.y, clicnt, current);
		}
	}

	fclose(f);
	update_seq();
}

static void dump_screens(void)
{
	struct list_head *cur;
	char path[baselen + sizeof("/tmp/screens")];
	FILE *f;

	sprintf(path, "%s/tmp/screens", basedir);

	if (!(f = fopen(path, "w+"))) {
		ee("fopen(%s) failed, %s\n", path, strerror(errno));
		return;
	}

	list_walk(cur, &screens) {
		struct screen *scr = list2screen(cur);
		char current;

		curscr == scr ? (current = '*') : (current = ' ');
		fprintf(f, "%u\t%s\t%ux%u%+d%+d\t%c\n", scr->id, scr->name,
			scr->w, scr->h + panel_height + PANEL_SCREEN_GAP,
			scr->x, scr->y, current);
	}

	fclose(f);
	update_seq();
}

static void dump_clients(uint8_t all)
{
	char path[baselen + sizeof("/tmp/clients")];
	struct list_head *cur;
	FILE *f;

	sprintf(path, "%s/tmp/clients", basedir);

	if (!(f = fopen(path, "w+"))) {
		ee("fopen(%s) failed, %s\n", path, strerror(errno));
		return;
	}

	list_walk(cur, &clients) {
		struct sprop title;
		char temp[sizeof("255 ") + TAG_NAME_MAX +
			  2 * sizeof("0xffffffff ") + sizeof("65535 ")];
		struct client *cli = glob2client(cur);
		xcb_window_t win = pointer2win();
		enum winstatus status;
		const char *tag;
		char current;

		if (cli->flags & CLI_FLG_IGNORED) {
			ww("win 0x%x is ignored\n", cli->win);
			continue;
		}

		cli->win == win ? (current = '*') : (current = ' ');
		status = window_status(cli->win);

		if (!all && status == WIN_STATUS_UNKNOWN)
			continue;
		else if (!all && cli->flags & CLI_FLG_DOCK)
			continue;
		else if (!all && cli->flags & CLI_FLG_TRAY)
			continue;

		if (cli->tag)
			tag = cli->tag->name;
		else if (cli->flags & CLI_FLG_DOCK)
			tag = "<dock>";
		else if (cli->flags & CLI_FLG_TRAY)
			tag = "<tray>";
		else
			tag = "<nil>";

		snprintf(temp, sizeof(temp), "%c\t%u\t%s\t0x%x\t%d\t",
			 current, cli->scr->id, tag, cli->win,
			 win2pid(cli->win));
		fwrite(temp, strlen(temp), 1, f);

		get_sprop(&title, cli->win, a_net_wm_name, UINT_MAX);

		if (!title.ptr || !title.len) {
			fputs("<nil>\n", f);
		} else {
			fwrite(title.str, title.len, 1, f);
			fputc('\n', f);
			free(title.ptr);
		}
	}

	fclose(f);
	update_seq();
}

#define match(str0, str1) strncmp(str0, str1, sizeof(str1) - 1) == 0

static void handle_user_request(int fd)
{
	char req[32];
	struct sprop name;

	if (fd < 0) {
		get_sprop(&name, rootscr->root, XCB_ATOM_WM_NAME, UCHAR_MAX);
		if (!name.ptr) {
			get_sprop(&name, rootscr->root, a_net_wm_name, UINT_MAX);
			if (!name.ptr)
				return;
		}
	} else {
		if (read(fd, req, sizeof(req)) < 1) {
			ee("read(%d) failed, %s\n", fd, strerror(errno));
			return;
		}
		name.str = req;
		name.len = sizeof(req);
		name.ptr = NULL;
	}

	ii("handle request '%s'\n", name.str);

	if (match(name.str, "reload-keys")) {
		init_keys();
	} else if (match(name.str, "reinit-outputs")) {
		init_outputs();
	} else if (match(name.str, "list-clients")) {
		dump_clients(0);
	} else if (match(name.str, "list-clients-all")) {
		dump_clients(1);
	} else if (match(name.str, "list-screens")) {
		dump_screens();
	} else if (match(name.str, "list-tags")) {
		dump_tags();
	} else if (match(name.str, "refresh-panel")) {
		const char *arg = &name.str[sizeof("refresh-panel")];
		if (arg)
			refresh_panel(atoi(arg));
	} else if (match(name.str, "focus-screen")) {
		const char *arg = &name.str[sizeof("focus-screen")];
		if (arg)
			focus_screen(atoi(arg));
	} else if (match(name.str, "focus-window")) {
		const char *arg = &name.str[sizeof("focus-window")];
		errno = 0;
		if (arg)
			focus_window_req(strtol(arg, NULL, 16));
	} else if (match(name.str, "make-grid")) {
		struct arg arg = {
			.x = curscr->x,
			.y = curscr->top,
			.data = 0,
		};
		make_grid(&arg);
	} else if (match(name.str, "reload-colors")) {
		struct list_head *cur;
		init_colors();
		list_walk(cur, &screens) {
			struct screen *scr = list2screen(cur);
			panel_raise(scr);
			redraw_panel_items(scr);
		}
	}

	free(name.ptr);
	xcb_flush(dpy);
}

#undef match

#define area(scr, area, ex)\
	(ex >= scr->items[area].x &&\
	 ex <= scr->items[area + 1].x)

#ifndef VERBOSE
#define dump_coords(scr, x) ;
#else
#define dump_coords(scr, ex) {\
	int i;\
	for (i = 0; i < PANEL_AREA_MAX; i++) {\
		ii("%d: %d <= %d <= %d (w = %d)\n", i, scr->items[i].x, ex,\
		   scr->items[i + 1].x, scr->items[i].w);\
		if area(scr, i, ex)\
			ii("inside element %d\n", i);\
	}\
}
#endif

static void toolbar_button_press(struct arg *arg)
{
	if (!focused_item) {
		hide_toolbar();
		return;
	}

	if (focused_item->str == (const char *) BTN_CLOSE) {
		if (toolbar.cli)
			close_window(toolbar.cli->win);
	} else if (focused_item->str == (const char *) BTN_LEFT) {
		arg->data = WIN_POS_LEFT_FILL;
		place_window((void *) arg);
	} else if (focused_item->str == (const char *) BTN_RIGHT) {
		arg->data = WIN_POS_RIGHT_FILL;
		place_window((void *) arg);
	} else if (focused_item->str == (const char *) BTN_TOP) {
		arg->data = WIN_POS_TOP_FILL;
		place_window((void *) arg);
	} else if (focused_item->str == (const char *) BTN_BOTTOM) {
		arg->data = WIN_POS_BOTTOM_FILL;
		place_window((void *) arg);
	} else if (focused_item->str == (const char *) BTN_CENTER) {
		arg->data = WIN_POS_CENTER;
		place_window((void *) arg);
	} else if (focused_item->str == (const char *) BTN_EXPAND) {
		arg->data = WIN_POS_FILL;
		place_window((void *) arg);
		hide_toolbar();
	} else if (focused_item->str == (const char *) BTN_FLAG) {
		curscr = coord2scr(arg->x, arg->y);
		if (!curscr->tag->anchor || curscr->tag->anchor != toolbar.cli) {
			curscr->tag->anchor = toolbar.cli;
			curscr->tag->anchor->div = 1;
			focused_item->flags |= ITEM_FLG_LOCKED;
			struct arg arg = { .data = WIN_POS_LEFT_FILL, };
			place_window(&arg);
		} else {
			curscr->tag->anchor = NULL;
			focused_item->flags &= ~ITEM_FLG_LOCKED;
			recalc_space(curscr, 0);
		}

		draw_locked_text(focused_item);
	} else if (focused_item->str == (const char *) BTN_MOVE) {
		struct client *cli = toolbar.cli;
		cli->flags |= CLI_FLG_MOVE;
		hide_toolbar();
		center_pointer(cli);

		/* subscribe to motion events */

		xcb_grab_pointer(dpy, 0, rootscr->root,
				 XCB_EVENT_MASK_POINTER_MOTION |
				 XCB_EVENT_MASK_BUTTON_RELEASE,
				 XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
				 cli->win, XCB_NONE, XCB_CURRENT_TIME);
		xcb_flush(dpy);
	} else {
		hide_toolbar();
	}

	hide_toolbox();
	toolbox.skip = 1;
}

static void panel_button_press(xcb_button_press_event_t *e)
{
	motion_cli = NULL;
	curscr = coord2scr(e->root_x, e->root_y);
	dd("screen %d, press at %d,%d", curscr->id, e->event_x, e->event_y);
	dump_coords(curscr, e->event_x);

	if area(curscr, PANEL_AREA_TAGS, e->event_x) {
		dd("panel press time %u", e->time);
		tag_time = e->time;
		select_tag(curscr, e->event_x, e->event_y);
		xcb_flush(dpy);
	} else if area(curscr, PANEL_AREA_TITLE, e->event_x) {
		ii("title\n");
		curscr->flags |= SCR_FLG_SWITCH_WINDOW_NOWARP;
		switch_window(curscr, DIR_NEXT);
	} else if area(curscr, PANEL_AREA_DOCK, e->event_x) {
		ii("dock\n");
	}
}

static void handle_button_release(xcb_button_release_event_t *e)
{
	struct client *cli = win2cli(e->child);

	if (cli && cli->flags & CLI_FLG_MOVE) {
		cli->flags &= ~CLI_FLG_MOVE;
		return;
	}

	curscr = coord2scr(e->root_x, e->root_y);

	if (curscr->panel.win == e->event || curscr->panel.win == e->child) {
		if area(curscr, PANEL_AREA_TAGS, e->event_x) {
			dd("panel release time %u", e->time - tag_time);
			if (e->time - tag_time > TAG_LONG_PRESS)
				ii("panel long press\n");
			motion_retag(e->event_x, e->event_y);
		} else if area(curscr, PANEL_AREA_MENU, e->event_x) {
			show_menu();
		} else if area(curscr, PANEL_AREA_DIV, e->event_x) {
			show_toolbar(NULL);
			hide_toolbox();
		}
	} else if (cli && cli->pos) {
		struct arg arg = {
			.x = curscr->x,
			.y = curscr->top,
			.data = cli->pos,
		};
		cli->pos = 0;
		place_window(&arg);
	}

	if (toolbar.panel.win != e->event && toolbar.panel.win != e->child)
		toolbar_ungrab_input();

	xcb_flush(dpy);
}

static void handle_button_press(xcb_button_press_event_t *e)
{
	te("XCB_BUTTON_PRESS: root 0x%x, pos %d,%d; event 0x%x, pos %d,%d; "
	   "child 0x%x, detail %d\n", e->root, e->root_x, e->root_y, e->event,
	   e->event_x, e->event_y, e->child, e->detail);

	if (toolbox.win &&
	    (toolbox.win == e->event || toolbox.win == e->child)) {
		hide_toolbox();
		show_toolbar(NULL);
		return;
	}

	curscr = coord2scr(e->root_x, e->root_y);
	trace_screen_metrics(curscr);

	switch (e->detail) {
	case MOUSE_BTN_LEFT:
		if (curscr->panel.win == e->event) {
			panel_button_press(e);
			return;
		} else if (toolbar.panel.win == e->event) {
			struct arg data = { .x = e->root_x, .y = e->root_y, };
			toolbar_button_press(&data);
			return;
		} else {
			struct client *cli = win2cli(e->child);
			if (cli)
				center_pointer(cli);
		}
		break;
	case MOUSE_BTN_MID:
		break;
	case MOUSE_BTN_RIGHT:
		panel_items_stat(curscr);
		break;
	default:
		break;
	}

	/* pressed with modifier */

	if (e->event != e->root || e->child == XCB_WINDOW_NONE)
		return;

	/* prepare for motion event */

	raise_window(e->child);
	panel_raise(curscr);
	hide_toolbox();

	/* subscribe to motion events */

	xcb_grab_pointer(dpy, 0, e->root,
			 XCB_EVENT_MASK_BUTTON_MOTION |
			 XCB_EVENT_MASK_BUTTON_RELEASE,
			 XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
			 e->child, XCB_NONE, XCB_CURRENT_TIME);
	xcb_flush(dpy);

	motion_cli = NULL;
	motion_init_x = motion_init_y = 0;
}

static void handle_panel_motion(int16_t x, int16_t y)
{
	struct list_head *cur;

	list_walk(cur, &curscr->tags) {
		struct tag *tag = list2tag(cur);

		if (tag == curscr->tag) {
			continue;
		} else if (!tag_pointed(tag, x, y)) {
			if (tag->flags & ITEM_FLG_FOCUSED)
				print_tag(curscr, tag, ITEM_FLG_NORMAL);
		} else if (tag_pointed(tag, x, y)) {
			if (!(tag->flags & ITEM_FLG_FOCUSED))
				print_tag(curscr, tag, ITEM_FLG_FOCUSED);
		}
	}
}

#define MOTION_ZONE_DIV 7
#define MOTION_ZONE_MUL 3

static void motion_place(struct client *cli, int16_t x, int16_t y)
{
	uint32_t color = notice_bg;
	uint16_t dw = curscr->w / MOTION_ZONE_DIV;
	uint16_t dh = curscr->h / MOTION_ZONE_DIV;
	uint16_t sw = curscr->x + curscr->w;
	uint16_t sh = curscr->y + curscr->h + panel_height;
	int16_t sx = curscr->x;
	int16_t sy = curscr->y;
	uint16_t dw2 = dw * MOTION_ZONE_MUL;
	uint16_t dh2 = dh * MOTION_ZONE_MUL;

	if (x >= sx  && x < sx + dw && y >= sy && y < sy + dh) {
		cli->pos = WIN_POS_TOP_LEFT;
	} else if (x >= sx && x < sx + dw && y > sh - dh && y <= sh) {
		cli->pos = WIN_POS_BOTTOM_LEFT;
	} else if (x > sw - dw && x <= sw && y >= sy && y < sy + dh) {
		cli->pos = WIN_POS_TOP_RIGHT;
	} else if (x > sw - dw && x <= sw && y > sh - dh && y <= sh) {
		cli->pos = WIN_POS_BOTTOM_RIGHT;
	} else if (x > sx + dw2 && x <= sw - dw2 && y > sy && y < sy + dh) {
		cli->pos = WIN_POS_TOP_FILL;
	} else if (x > sx + dw2 && x <= sw - dw2 && y > sh - dh && y <= sh) {
		cli->pos = WIN_POS_BOTTOM_FILL;
	} else if (x >= sx && x <= sx + dw && y > sy + dh2 && y <= sh - dh2) {
		cli->pos = WIN_POS_LEFT_FILL;
	} else if (x > sw - dw && x <= sw && y > sy + dh2 && y < sh - dh2) {
		cli->pos = WIN_POS_RIGHT_FILL;
	} else {
		cli->pos = 0;
		color = active_bg;
	}

	cli->div = 1;
	window_border_color(cli->win, color);
}

static uint16_t pointer_x;

static void handle_motion_notify(xcb_motion_notify_event_t *e)
{
	uint16_t mask;
	uint32_t val[2];
	struct client *cli;

	te("XCB_MOTION_NOTIFY: root %+d%+d, event 0x%x %+d%+d, child 0x%x\n",
	   e->root_x, e->root_y, e->event, e->event_x, e->event_y, e->child);

	if (e->event == toolbar.panel.win) {
		focus_toolbar_item(e->event_x, e->event_y);
		pointer_x = e->event_x;
		return;
	}

	curscr = coord2scr(e->root_x, e->root_y);

	if (!curscr)
		return;

	trace_screen_metrics(curscr);
	cli = win2cli(e->child); /* window is being moved so search in global list */

	if (cli && cli->flags & (CLI_FLG_DOCK | CLI_FLG_TRAY | CLI_FLG_IGNORED)) {
		ww("win 0x%x is not moveable\n", cli->win);
		return;
	} else if (curscr && curscr->panel.win == e->child) {
		/* panel window is a child in motion with modifier key */
		handle_panel_motion(e->event_x, e->event_y);
		return;
	} else if (!e->child || !cli) {
		if (e->child)
			ww("win 0x%x is not managed\n", e->child);
		return;
	}

	motion_place(cli, e->root_x, e->root_y);

	/* save initial coords on the first move */

	if (!motion_init_x)
		motion_init_x = cli->x;

	if (!motion_init_y)
		motion_init_y = cli->y;

	motion_cli = cli;

	cli->x = e->root_x - cli->w / 2 - BORDER_WIDTH;
	cli->y = e->root_y - cli->h / 2 - BORDER_WIDTH;

	mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
	val[0] = cli->x;
	val[1] = cli->y;
	xcb_configure_window_checked(dpy, cli->win, mask, val);
	xcb_flush(dpy);

	if (cli->scr != curscr && !(cli->flags & CLI_FLG_DOCK)) { /* retag */
		list_del(&cli->head);
		list_add(&curscr->tag->clients, &cli->head);
		cli->tag = curscr->tag;
		cli->scr = curscr;
		ii("win 0x%x now on tag %s screen %d\n", e->child,
		   curscr->tag->name, curscr->id);
		resort_client(cli);
		store_client(cli, 0);
	}
}

static void toolbar_key_press(xcb_key_press_event_t *e)
{
	uint8_t flg;
	struct toolbar_item *cur;
	struct toolbar_item *end;

	if (toolbar.kclose == e->detail) {
		hide_toolbar();
		return;
	} else if (toolbar.kenter == e->detail) {
		struct arg data = { .x = e->root_x, .y = e->root_y, };
		toolbar_button_press(&data);
		return;
	}

	cur = focused_item;
	end = toolbar_items + ARRAY_SIZE(toolbar_items);

	if (!focused_item)
		focused_item = toolbar_items;

	if (toolbar.knext == e->detail) {
		if (++focused_item == end)
			focused_item = toolbar_items;
	} else if (toolbar.kprev == e->detail) {
		if (--focused_item == toolbar_items - 1)
			focused_item = end - 1;
	}

	if (cur)
		draw_toolbar_text(cur, ITEM_FLG_NORMAL);

	if (focused_item->str == (const char *) BTN_CLOSE)
		flg = ITEM_FLG_ALERT;
	else
		flg = ITEM_FLG_FOCUSED;

	draw_toolbar_text(focused_item, flg);
}

static void handle_key_press(xcb_key_press_event_t *e)
{
	struct list_head *cur;

	if (toolbar.panel.win &&
	    (e->event == toolbar.panel.win || e->child == toolbar.panel.win)) {
		toolbar_key_press(e);
		return;
	}

	ii("screen %d, event 0x%x, child 0x%x, key 0x%x, state 0x%x, pos %d,%d\n",
	   curscr->id, e->event, e->child,
	   e->detail, e->state, e->root_x, e->root_y);

	list_walk(cur, &keymap) {
		struct keymap *kmap = list2keymap(cur);
		if (kmap->key == e->detail && kmap->mod == e->state) {
			struct arg arg = {
				.x = e->root_x,
				.y = e->root_y,
				.kmap = kmap,
				.data = 0,
			};
			kmap->action(&arg);
			return;
		}
	}
}

static void handle_visibility(xcb_window_t win)
{
	struct list_head *cur;
	uint8_t panel;

	if (win == toolbar.panel.win)
		draw_toolbar();

	/* Check if this is a panel that needs some refreshment. */
	panel = 0;
	list_walk(cur, &screens) {
		struct screen *scr = list2screen(cur);
		if (scr->panel.win == win) {
			panel = 1;
			break;
		}
	}

	if (!panel)
		return;

	/* One of the panels needs to be refreshed; however under certain
	 * conditions in multiscreen mode all of them might need to undergo
	 * the same treatment...
	 */
	list_walk(cur, &screens) {
		struct screen *scr = list2screen(cur);
		panel_raise(scr);
		redraw_panel_items(scr);
	}

	xcb_flush(dpy);
}

static void handle_unmap_notify(xcb_unmap_notify_event_t *e)
{
	if (window_status(e->window) == WIN_STATUS_UNKNOWN) {
		ii("window 0x%x gone\n", e->window);
		del_window(e->window);
	}
}

static void handle_enter_notify(xcb_enter_notify_event_t *e)
{
	struct client *cli;

	if (e->event == toolbox.win)
		return;

	if (e->event == toolbar.panel.win) {
		focus_window(e->event);
		toolbar_grab_input();
		xcb_flush(dpy);
		return;
	}

	if (!(curscr = coord2scr(e->root_x, e->root_y))) {
		ww("set up unmanaged screen\n");
		/* Handle situation when wm started with monitors being
		 * set in clone mode and then switched into tiling mode
		 * with the same resolution; the latter aparently is not
		 * associated with any xrandr event.
		 *
		 * Initialize outputs as soon as any window is moved in. */
		init_outputs();
		return;
	}

	if (curscr && curscr->panel.win == e->event)
		return;

	if (curscr->tag->visited != XCB_WINDOW_NONE)
		unfocus_window(curscr->tag->visited);

	curscr->tag->visited = e->event;

	if (!(cli = pointer2cli()))
		cli = win2cli(e->event);

	unfocus_clients(curscr->tag);

	if (!cli) {
		focus_window(e->event);
	} else {
		cli->flags &= ~CLI_FLG_IGNORED;
		focus_window(cli->win);
		resort_client(cli);
	}

	if (e->mode == MOD)
		raise_window(e->event);

	show_toolbox(cli);
	xcb_flush(dpy);
}

static void handle_leave_notify(xcb_leave_notify_event_t *e)
{
	struct client *cli = win2cli(e->event);

	hide_toolbox();

	if (e->event == toolbox.win)
		return;

	if (cli && cli->flags & CLI_FLG_POPUP) {
		close_client(cli);
		focus_root_ = 1;
	}
}

static void handle_wmname(xcb_property_notify_event_t *e)
{
	struct color *bg, *fg;
	struct client *cli = win2cli(e->window);

	if (cli && cli->tag == curscr->tag) {
		print_title(cli->scr, e->window);
		xcb_flush(dpy);
	} else if (cli) {
		fg = color2ptr(NOTICE_FG);
		bg = color2ptr(NOTICE_BG);
		draw_panel_text(&cli->scr->panel, fg, bg, cli->tag->x,
				cli->tag->w, cli->tag->name, cli->tag->nlen,
				font1, ITEM_H_MARGIN);
		xcb_flush(dpy);
	}
}

static void handle_property_notify(xcb_property_notify_event_t *e)
{
	print_atom_name(e->atom);

	if (e->atom == XCB_ATOM_WM_NAME) {
		if (e->window == rootscr->root) {
			handle_user_request(-1);
		} else if (curscr) {
			handle_wmname(e);
		}
	} else if (e->atom == a_usertime) {
		if (!e->time)
			dd("_NET_WM_USER_TIME from win 0x%x time %u state %u",
			   e->window, e->time, e->state);
	} else if (e->atom == a_has_vt) {
		struct list_head *cur;
		list_walk(cur, &screens) {
			struct screen *scr = list2screen(cur);
			panel_raise(scr);
			redraw_panel_items(scr);
		}

		xcb_flush(dpy);
	}
}

#ifndef VERBOSE
#define print_configure_notify(e) ;
#else
static void print_configure_notify(xcb_configure_notify_event_t *e)
{
	ii("prop:\n"
	   " response_type=%u\n"
	   " sequence=%u\n"
	   " event=0x%x\n"
	   " window=0x%x\n"
	   " above_sibling=0x%x\n"
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
#endif

static void handle_configure_notify(xcb_configure_notify_event_t *e)
{
	print_configure_notify(e);

	if (e->event == rootscr->root && e->window == rootscr->root) {
		struct screen *scr = pointer2scr();
		if (scr) /* update because screen geo could change */
			update_panel_items(scr);
	} else if (e->window != rootscr->root && e->border_width) {
		window_border_width(e->window, BORDER_WIDTH);
		xcb_flush(dpy);
	}
}

static void tray_add(xcb_window_t win)
{
	struct client *cli;

	ii("%s: win 0x%x\n", __func__, win);

	cli = win2cli(win);
	if (!cli) {
		cli = add_window(win, 1, 0);
		if (!cli) {
			ee("add_window(0x%x) failed\n", win);
			return;
		}
	}
}

#define SYSTEM_TRAY_REQUEST_DOCK 0
#define SYSTEM_TRAY_BEGIN_MESSAGE 1
#define SYSTEM_TRAY_CANCEL_MESSAGE 2

static void handle_client_message(xcb_client_message_event_t *e)
{
	print_atom_name(e->type);
	dd("win 0x%x data[] = { %u, %u, %u, ... }, format %d type %d",
	   e->window, e->data.data32[0], e->data.data32[1], e->data.data32[2],
	   e->format, e->type);
	print_atom_name(e->data.data32[0]);
	print_atom_name(e->data.data32[1]);

	if (e->type == a_net_wm_state &&  e->format == 32 &&
	    e->data.data32[1] == a_maximize_win) {
		struct arg arg = { .data = WIN_POS_FILL, };
		place_window((void *) &arg);
	} else if (e->type == a_net_wm_state &&  e->format == 32 &&
	    e->data.data32[1] == a_fullscreen) {
		struct arg arg = { .data = WIN_POS_FILL, };
		place_window((void *) &arg);
	} else if (e->type == a_net_wm_state &&  e->format == 32 &&
	    e->data.data32[1] == a_hidden) {
		dd("_NET_WM_STATE_HIDDEN from win 0x%x", e->window);
	} else if (e->type == a_protocols && e->format == 32 &&
		   e->data.data32[0] == a_ping) {
		dd("pong win 0x%x time %u", e->data.data32[2],
		   e->data.data32[1]);
	} else if (e->type == a_systray && e->format == 32 &&
	    e->data.data32[1] == SYSTEM_TRAY_REQUEST_DOCK) {
		tray_add(e->data.data32[2]);
	} else if (e->type == a_active_win && e->format == 32) {
		struct client *cli = win2cli(e->window);
		if (!cli || (cli->flags & CLI_FLG_DOCK))
			return;
		focus_tag(cli->scr, cli->tag);
		raise_window(e->window);
		xcb_flush(dpy);
	}
}

#ifndef VERBOSE
#define print_configure_request(e) ;
#else
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

static void print_configure_request(xcb_configure_request_event_t *e)
{
	ii("prop:\n"
	   " response_type=%u\n"
	   " stack_mode=%u\n"
	   " sequence=%u\n"
	   " parent=0x%x\n"
	   " window=0x%x\n"
	   " sibling=0x%x\n"
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
	struct list_head *cur;
	struct client *cli = NULL;
	uint32_t val[7] = { 0 };
	int i = 0;
	uint16_t w, h, mask = 0;
	int16_t x, y;

	list_walk(cur, &defscr->dock) {
		cli = list2client(cur);
		if (cli->flags & CLI_FLG_TRAY && cli->win == e->window)
			break;
		cli = NULL;
	}

	/* check if window is placed within screen boundaries */
	if (!cli) { /* excluding tray windows */
		x = adjust_x(curscr, e->x, e->width);
		y = adjust_y(curscr, e->y, e->height);
		w = adjust_w(curscr, e->width);
		h = adjust_h(curscr, e->height);
	} else {
		x = cli->x;
		y = cli->y;
		w = cli->w;
		h = cli->h;
	}

	/* the order has to correspond to the order value_mask bits */
	if (e->value_mask & XCB_CONFIG_WINDOW_X || e->x != x) {
		val[i++] = x;
		mask |= XCB_CONFIG_WINDOW_X;
	}
	if (e->value_mask & XCB_CONFIG_WINDOW_Y || e->y != y) {
		val[i++] = e->y;
		mask |= XCB_CONFIG_WINDOW_Y;
	}
	if (e->value_mask & XCB_CONFIG_WINDOW_WIDTH || e->width != w) {
		val[i++] = w;
		mask |= XCB_CONFIG_WINDOW_WIDTH;
	}
	if (e->value_mask & XCB_CONFIG_WINDOW_HEIGHT || e->height != h) {
		val[i++] = h;
		mask |= XCB_CONFIG_WINDOW_HEIGHT;
	}
	if (e->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) {
		if (e->border_width > 1)
			val[i++] = BORDER_WIDTH;
		else
			val[i++] = e->border_width;
		mask |= XCB_CONFIG_WINDOW_BORDER_WIDTH;
	}
	if (e->value_mask & XCB_CONFIG_WINDOW_SIBLING) {
		val[i++] = e->sibling;
		mask |= XCB_CONFIG_WINDOW_SIBLING;
	}
	if (e->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) {
		val[i++] = e->stack_mode;
		mask |= XCB_CONFIG_WINDOW_STACK_MODE;
	}

	xcb_configure_window_checked(dpy, e->window, mask, val);
	xcb_flush(dpy);
}

static void handle_randr_notify(xcb_randr_screen_change_notify_event_t *e)
{
	ii("root 0x%x, win 0x%x, sizeID %u, w %u, h %u, mw %u, mh %u\n",
	   e->root, e->request_window, e->sizeID, e->width, e->height,
	   e->mwidth, e->mheight);

	init_outputs();
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

	type = XCB_EVENT_RESPONSE_TYPE(e);
	mm("got event %d (%d)\n", e->response_type, type);

#define WIN(struct_ptr) ((struct_ptr *) e)->window

	switch (type) {
	case 0: break; /* NO EVENT */
	case XCB_VISIBILITY_NOTIFY:
		te("XCB_VISIBILITY_NOTIFY: win 0x%x, state %d\n",
		   WIN(xcb_visibility_notify_event_t),
		   ((xcb_visibility_notify_event_t *) e)->state);
                switch (((xcb_visibility_notify_event_t *) e)->state) {
                case XCB_VISIBILITY_FULLY_OBSCURED:
                case XCB_VISIBILITY_PARTIALLY_OBSCURED: /* fall through */
		default:
			handle_visibility(WIN(xcb_visibility_notify_event_t));
                }
		break;
	case XCB_EXPOSE:
		te("XCB_EXPOSE: win 0x%x\n", WIN(xcb_expose_event_t));
		handle_visibility(WIN(xcb_expose_event_t));
		break;
	case XCB_BUTTON_PRESS:
		handle_button_press((xcb_button_press_event_t *) e);
		break;
	case XCB_BUTTON_RELEASE:
		te("XCB_BUTTON_RELEASE: pos %d,%d, event 0x%x, child 0x%x\n",
		   ((xcb_button_press_event_t *) e)->root_x,
		   ((xcb_button_press_event_t *) e)->root_y,
		   ((xcb_button_press_event_t *) e)->event,
		   ((xcb_button_press_event_t *) e)->child);
		handle_button_release((xcb_button_release_event_t *) e);
		break;
	case XCB_MOTION_NOTIFY:
		handle_motion_notify((xcb_motion_notify_event_t *) e);
		break;
	case XCB_CONFIGURE_REQUEST:
		te("XCB_CONFIGURE_REQUEST: win 0x%x\n",
		   ((xcb_configure_request_event_t *) e)->window);
		print_configure_request((xcb_configure_request_event_t *) e);
		handle_configure_request((xcb_configure_request_event_t *) e);
		break;
	case XCB_DESTROY_NOTIFY:
		te("XCB_DESTROY_NOTIFY: event 0x%x, win 0x%x\n",
		   ((xcb_destroy_notify_event_t *) e)->event,
		   ((xcb_destroy_notify_event_t *) e)->window);
		del_window(((xcb_destroy_notify_event_t *) e)->window);
		break;
	case XCB_ENTER_NOTIFY:
		te("XCB_ENTER_NOTIFY: root 0x%x, event 0x%x, child 0x%x\n",
		   ((xcb_enter_notify_event_t *) e)->root,
		   ((xcb_enter_notify_event_t *) e)->event,
		   ((xcb_enter_notify_event_t *) e)->child);
		te("detail 0x%x, state 0x%x, mode 0x%x\n",
		   ((xcb_enter_notify_event_t *) e)->detail,
		   ((xcb_enter_notify_event_t *) e)->state,
		   ((xcb_enter_notify_event_t *) e)->mode);
		te("at root %d,%d event %d,%d\n",
		   ((xcb_enter_notify_event_t *) e)->root_x,
		   ((xcb_enter_notify_event_t *) e)->root_y,
		   ((xcb_enter_notify_event_t *) e)->event_x,
		   ((xcb_enter_notify_event_t *) e)->event_y);
		handle_enter_notify((xcb_enter_notify_event_t *) e);
		break;
	case XCB_LEAVE_NOTIFY:
		handle_leave_notify((xcb_leave_notify_event_t *) e);
		break;
	case XCB_KEY_PRESS:
		te("XCB_KEY_PRESS: root 0x%x, win 0x%x, child 0x%x, key %d\n",
		   ((xcb_key_press_event_t *) e)->root,
		   ((xcb_key_press_event_t *) e)->event,
		   ((xcb_key_press_event_t *) e)->child,
		   ((xcb_key_press_event_t *) e)->detail);
		handle_key_press((xcb_key_press_event_t *) e);
		break;
	case XCB_KEY_RELEASE:
		te("XCB_KEY_RELEASE: root 0x%x, win 0x%x, child 0x%x, key %d\n",
		   ((xcb_key_release_event_t *) e)->root,
		   ((xcb_key_release_event_t *) e)->event,
		   ((xcb_key_release_event_t *) e)->child,
		   ((xcb_key_release_event_t *) e)->detail);
		curscr->flags &= ~SCR_FLG_SWITCH_WINDOW;
		break;
	case XCB_CREATE_NOTIFY:
		te("XCB_CREATE_NOTIFY: parent 0x%x, window 0x%x, "
		   "geo %ux%u+%d+%d, border %u, override-redirect %u\n",
		   ((xcb_create_notify_event_t *) e)->parent,
		   ((xcb_create_notify_event_t *) e)->window,
		   ((xcb_create_notify_event_t *) e)->width,
		   ((xcb_create_notify_event_t *) e)->height,
		   ((xcb_create_notify_event_t *) e)->x,
		   ((xcb_create_notify_event_t *) e)->y,
		   ((xcb_create_notify_event_t *) e)->border_width,
		   ((xcb_create_notify_event_t *) e)->override_redirect);
		break;
	case XCB_MAP_NOTIFY:
		te("XCB_MAP_NOTIFY: event 0x%x, win 0x%x, redirect %u\n",
		   ((xcb_map_notify_event_t *) e)->event,
		   ((xcb_map_notify_event_t *) e)->window,
		   ((xcb_map_notify_event_t *) e)->override_redirect);
		break;
	case XCB_MAP_REQUEST:
		te("XCB_MAP_REQUEST: parent 0x%x, win 0x%x\n",
		   ((xcb_map_request_event_t *) e)->parent,
		   ((xcb_map_request_event_t *) e)->window);
		add_window(((xcb_map_notify_event_t *) e)->window, 0, 0);
		break;
	case XCB_PROPERTY_NOTIFY:
		te("XCB_PROPERTY_NOTIFY: win 0x%x, atom %d\n",
		   ((xcb_property_notify_event_t *) e)->window,
		   ((xcb_property_notify_event_t *) e)->atom);
		handle_property_notify((xcb_property_notify_event_t *) e);
		break;
	case XCB_UNMAP_NOTIFY:
		te("XCB_UNMAP_NOTIFY: event 0x%x, window 0x%x\n",
		   ((xcb_unmap_notify_event_t *) e)->event,
		   ((xcb_unmap_notify_event_t *) e)->window);
		handle_unmap_notify((xcb_unmap_notify_event_t *) e);
		break;
	case XCB_CLIENT_MESSAGE:
		te("XCB_CLIENT_MESSAGE: win 0x%x, type %d\n",
		   ((xcb_client_message_event_t *) e)->window,
		   ((xcb_client_message_event_t *) e)->type);
		handle_client_message((xcb_client_message_event_t *) e);
		break;
	case XCB_CONFIGURE_NOTIFY:
		te("XCB_CONFIGURE_NOTIFY: event 0x%x, window 0x%x, above 0x%x\n",
		   ((xcb_configure_notify_event_t *) e)->event,
		   ((xcb_configure_notify_event_t *) e)->window,
		   ((xcb_configure_notify_event_t *) e)->above_sibling);
		handle_configure_notify((xcb_configure_notify_event_t *) e);
		break;
	default:
		te("unhandled event type %d\n", type);
		break;
	}

        if (type - randrbase == XCB_RANDR_SCREEN_CHANGE_NOTIFY) {
		ii("XCB_RANDR_SCREEN_CHANGE_NOTIFY\n");
		handle_randr_notify((xcb_randr_screen_change_notify_event_t *) e);
	}

	free(e);
	return 1;
}

static void init_font(void)
{
	font1 = XftFontOpen(xdpy, xscr, XFT_FAMILY, XftTypeString,
			    FONT1_NAME, XFT_SIZE, XftTypeDouble,
			    FONT1_SIZE, NULL);
	if (!font1)
		panic("XftFontOpen(%s)\n", FONT1_NAME);

	text_yoffs = font1->ascent + ITEM_V_MARGIN;

	font2 = XftFontOpen(xdpy, xscr, XFT_FAMILY, XftTypeString,
			    FONT2_NAME, XFT_SIZE, XftTypeDouble,
			    FONT2_SIZE, NULL);
	if (font2) {
		menu_font = font2;
	} else {
		menu_font = font1;
		ee("XftFontOpen(%s)\n", FONT2_NAME);
	}
}

static void init_keys_def(void)
{
	strlen_t tmp;
	uint8_t i;
	char path[baselen + sizeof("/keys/") + UCHAR_MAX];
	FILE *f;
	xcb_key_symbols_t *syms;

	list_init(&keymap);

	xcb_ungrab_key(dpy, XCB_GRAB_ANY, rootscr->root, XCB_MOD_MASK_ANY);

	syms = xcb_key_symbols_alloc(dpy);
	if (!syms) {
		ee("xcb_key_symbols_alloc() failed\n");
		return;
	}

	for (i = 0; i < ARRAY_SIZE(kmap_def); i++) {
		struct keymap *kmap = &kmap_def[i];

		grab_key(syms, kmap);
		if (!kmap->key) {
			ww("nack map %s to %s\n", kmap->actname, kmap->keyname);
			continue;
		}

		ii("map %s to %s\n", kmap->actname, kmap->keyname);
		list_add(&keymap, &kmap->head);

		if (!basedir)
			continue;

		tmp = strlen(kmap->actname);
		if (tmp > actname_max)
			actname_max = tmp;

		sprintf(path, "%s/keys/%s", basedir, kmap->keyname);

		if ((f = fopen(path, "w"))) {
			ii("write '%s'\n", kmap->actname);
			fprintf(f, "%s", kmap->actname);
			fclose(f);
		}
	}

	actname_max++;
}

static void init_randr(void)
{
	const xcb_query_extension_reply_t *ext;

	ext = xcb_get_extension_data(dpy, &xcb_randr_id);
	if (!ext || !ext->present) {
		ii("RandR extension is not present\n");
		return;
	}

	randrbase = ext->first_event;
	xcb_randr_select_input(dpy, rootscr->root,
			       XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE |
			       XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE |
			       XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE |
			       XCB_RANDR_NOTIFY_MASK_OUTPUT_PROPERTY);
}

#define support_atom(a)\
	xcb_change_property(dpy, XCB_PROP_MODE_APPEND, rootscr->root,\
			    a_supported, XCB_ATOM_ATOM, 32, 1, a)

static void init_rootwin(void)
{
	xcb_atom_t a_supported;
	xcb_window_t win = rootscr->root;
	xcb_void_cookie_t c;
	uint32_t val = XCB_EVENT_MASK_BUTTON_PRESS;

	xcb_eval(c, xcb_grab_button(dpy, 0, win, val, XCB_GRAB_MODE_ASYNC,
		 XCB_GRAB_MODE_ASYNC, win, XCB_NONE, MOUSE_BTN_LEFT, MOD));
	xcb_eval(c, xcb_grab_button(dpy, 0, win, val, XCB_GRAB_MODE_ASYNC,
		 XCB_GRAB_MODE_ASYNC, win, XCB_NONE, MOUSE_BTN_MID, MOD));
	xcb_eval(c, xcb_grab_button(dpy, 0, win, val, XCB_GRAB_MODE_ASYNC,
		 XCB_GRAB_MODE_ASYNC, win, XCB_NONE, MOUSE_BTN_RIGHT, MOD));

	/* subscribe events */
	val = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT;
	val |= XCB_EVENT_MASK_STRUCTURE_NOTIFY;
	val |= XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_change_window_attributes(dpy, win, XCB_CW_EVENT_MASK, &val);

	a_embed_info = atom_by_name("_XEMBED_INFO");
	a_state = atom_by_name("WM_STATE");
	a_client_list = atom_by_name("_NET_CLIENT_LIST");
	a_systray = atom_by_name("_NET_SYSTEM_TRAY_OPCODE");
	a_active_win = atom_by_name("_NET_ACTIVE_WINDOW");
	a_has_vt = atom_by_name("XFree86_has_VT");
	a_supported = atom_by_name("_NET_SUPPORTED");
	a_net_wm_name = atom_by_name("_NET_WM_NAME");
	a_net_wm_pid = atom_by_name("_NET_WM_PID");
	a_leader = atom_by_name("WM_CLIENT_LEADER");
	a_protocols = atom_by_name("WM_PROTOCOLS");
	a_delete_win = atom_by_name("WM_DELETE_WINDOW");
	a_maximize_win = atom_by_name("_NET_WM_STATE_MAXIMIZED_VERT");
	a_net_wm_state = atom_by_name("_NET_WM_STATE");
	a_fullscreen = atom_by_name("_NET_WM_STATE_FULLSCREEN");
	a_hidden = atom_by_name("_NET_WM_STATE_HIDDEN");
	a_usertime = atom_by_name("_NET_WM_USER_TIME");
	a_ping = atom_by_name("_NET_WM_PING");

	xcb_delete_property(dpy, win, a_supported);
	support_atom(&a_active_win);

	xcb_flush(dpy);
}

static int init_homedir(void)
{
	int mode = S_IRWXU;

	homedir = getenv("YAWM_HOME");
	if (!homedir) {
		homedir = getenv("HOME");
		if (!homedir)
			return -1;
	}

	baselen = strlen(homedir) + sizeof("/.yawm");
	basedir = calloc(1, baselen);
	if (!basedir) {
		ee("calloc(%d) failed, use built-in config\n", baselen);
		return -1;
	}

	if (chdir(homedir) < 0) {
		ee("chdir(%s) failed\n", homedir);
		goto homeless;
	}

	if (mkdir(".yawm", mode) < 0 && errno != EEXIST) {
		ee("mkdir(%s/.yawm) failed\n", homedir);
		goto err;
	}

	snprintf(basedir, baselen, "%s/.yawm", homedir);
	ii("basedir: %s\n", basedir);

	if (chdir(basedir) < 0) { /* change to working directory */
		ee("chdir(%s) failed\n", basedir);
		goto err;
	}

	if (mkdir(".session", mode) < 0 && errno != EEXIST) {
		ee("mkdir(%s/.yawm/.session) failed\n", homedir);
		goto err;
	}

	if (mkdir("screens", mode) < 0 && errno != EEXIST) {
		ee("mkdir(%s/.yawm/screens) failed\n", homedir);
		goto err;
	}

	if (mkdir("panel", mode) < 0 && errno != EEXIST) {
		ee("mkdir(%s/.yawm/panel) failed\n", homedir);
		goto err;
	}

	if (mkdir("keys", mode) < 0 && errno != EEXIST) {
		ee("mkdir(%s/.yawm/keys) failed\n", homedir);
		goto err;
	}

	if (mkdir("colors", mode) < 0 && errno != EEXIST) {
		ee("mkdir(%s/.yawm/colors) failed\n", homedir);
		goto err;
	}

	return 0;

err:
	chdir(homedir);
homeless:
	free(basedir);
	basedir = NULL;
	baselen = 0;
	ww("not all directories are available some features will be disabled\n");
	return -1;
}

enum fdtypes {
	FD_SRV,
	FD_CTL,
	FD_MAX,
};

static int open_control(int fd)
{
	char path[sizeof(YAWM_CTRL) + sizeof("255") - 1];

	snprintf(path, sizeof(path), YAWM_CTRL ":%d", xscr);
	return open_fifo(path, fd);
}

static inline void handle_server_event(struct pollfd *pfd)
{
	if (pfd->revents & POLLIN)
		while (handle_events()) {} /* read all events */

	pfd->revents = 0;
}

static inline void handle_control_event(struct pollfd *pfd)
{
	if (pfd->revents & POLLIN)
		handle_user_request(pfd->fd);

	/* reset pipe */
	pfd->fd = open_control(pfd->fd);
	pfd->revents = 0;
}

int main()
{
	struct pollfd pfds[FD_MAX];
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

	if (init_homedir() < 0)
		ww("home directory no initialized\n");

	if (signal(SIGCHLD, spawn_cleanup) == SIG_ERR)
		panic("SIGCHLD handler failed\n");

	xdpy = XOpenDisplay(NULL);
	if (!xdpy) {
		ee("XOpenDisplay(%s) failed\n", getenv("DISPLAY"));
		return 1;
	}
	xscr = DefaultScreen(xdpy);
	init_font();
	init_colors();
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

	ii("root 0x%x, size %dx%d\n", rootscr->root, rootscr->width_in_pixels,
	   rootscr->height_in_pixels);

	list_init(&screens);
	list_init(&clients);

	init_keys_def();
	init_keys();
	init_rootwin();
	init_randr();
	init_outputs();
	init_toolbox();
	focus_root();
	xcb_flush(dpy);

	autostart();

	pfds[FD_SRV].fd = xcb_get_file_descriptor(dpy);
	pfds[FD_SRV].events = POLLIN;
	pfds[FD_SRV].revents = 0;

	pfds[FD_CTL].fd = open_control(-1);
	pfds[FD_CTL].events = POLLIN;
	pfds[FD_CTL].revents = 0;

	ii("defscr %d, curscr %d\n", defscr->id, curscr->id);
	ii("enter events loop\n");

	while (1) {
		errno = 0;
		int rc = poll(pfds, ARRAY_SIZE(pfds), -1);
		if (rc == 0) { /* timeout */
			/* TODO: some user-defined periodic task */
		} else if (rc < 0) {
			if (errno == EINTR)
				continue;
			/* something weird happened, but relax and try again */
			sleep(1);
			continue;
		}

		handle_control_event(&pfds[FD_CTL]);
		handle_server_event(&pfds[FD_SRV]);

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
