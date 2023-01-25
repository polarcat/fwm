/* fwm.c: flatter window manager
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
#include "fwm.h"
#include "list.h"

/* defines */

#ifndef MAX_PATH
#define MAX_PATH 255
#endif

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

#define BORDER_WIDTH 2
#define WINDOW_PAD 0

#define WIN_DELAY_US 10000

#ifndef WIN_WIDTH_MIN
#define WIN_WIDTH_MIN 10
#endif

#ifndef WIN_HEIGHT_MIN
#define WIN_HEIGHT_MIN 10
#endif

#define WIN_FLG_SCAN (1 << 0)
#define WIN_FLG_TRAY (1 << 1)
#define WIN_FLG_USER (1 << 2)

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
#define CLI_FLG_LANCHOR (1 << 13)
#define CLI_FLG_RANCHOR (1 << 14)
#define CLI_FLG_LDOCK (1 << 15)

#define SCR_FLG_SWITCH_WINDOW (1 << 1)
#define SCR_FLG_SWITCH_WINDOW_NOWARP (1 << 2)
#define SCR_FLG_CLIENT_RETAG (1 << 3)

#define TAG_NAME_MAX 32

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
//#define BTN_PLACE "" /* fa-paperclip [&#xf0c6;] */
#define BTN_PLACE "" /* fa-plus [&#xf067;] */

struct toolbar_item {
	const char *str;
	uint8_t len;
	uint16_t x;
	uint16_t w;
	uint8_t flags;
};

static struct toolbar_item toolbar_items[] = {
	{ BTN_CLOSE, slen(BTN_CLOSE), },
	{ BTN_CENTER, slen(BTN_CENTER), },
	{ BTN_FLAG, slen(BTN_FLAG), },
	{ BTN_LEFT, slen(BTN_LEFT), },
	{ BTN_RIGHT, slen(BTN_RIGHT), },
	{ BTN_TOP, slen(BTN_TOP), },
	{ BTN_BOTTOM, slen(BTN_BOTTOM), },
	{ BTN_EXPAND, slen(BTN_EXPAND), },
	{ BTN_MOVE, slen(BTN_MOVE), },
	{ BTN_MOUSE, slen(BTN_MOUSE), },
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
};

enum {
	TOP_LEFT,
	TOP_RIGHT,
	BOTTOM_LEFT,
	BOTTOM_RIGHT,
};

struct toolbox {
	xcb_drawable_t win;
	xcb_gcontext_t gc;
	XftDraw *draw;
	struct client *cli;
	uint8_t size;
	uint8_t visible;
	uint16_t xdiv;
	uint16_t x;
	uint16_t y;
	uint8_t gravity;
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
struct screen *dockscr;
static xcb_screen_t *rootscr; /* root window details */
static struct toolbar toolbar; /* window toolbar */
static uint8_t focus_root_;
static struct client *motion_cli;
static int16_t motion_init_x;
static int16_t motion_init_y;
static uint8_t space_width = 1;
static int16_t save_x_;
static int16_t save_y_;

struct tag {
	struct list_head head;
	struct list_head clients;
	struct client *visited; /* last focused client */
	struct client *prev; /* prev focused client */
	struct client *front; /* front client */
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

#ifndef WIN_INC_STEP
#define WIN_INC_STEP 20
#endif

#ifndef POS_DIV_MAX
#define POS_DIV_MAX 9
#endif

#define GROW_STEP .1
#define GROW_STEP_GLOW 1.2
#define GROW_STEP_MIN 1.1

struct client {
	struct list_head head; /* local list */
	struct list_head list; /* global list */
	int16_t x, y;
	uint16_t w, h;
	float div; /* for position calculation */
	uint16_t inc; /* window size increment */
	xcb_window_t win;
	xcb_window_t leader;
	pid_t pid;
	struct screen *scr;
	struct tag *tag;
	uint32_t flags;
	uint32_t crc; /* based on class name */
	uint8_t busy;
	uint8_t pos; /* enum winpos */
	uint64_t ts; /* raise timestamp */
};

#define list2cli(item) list_entry(item, struct client, head)
#define glob2client(item) list_entry(item, struct client, list)

struct list_head clients; /* keep track of all clients */

struct config {
	xcb_window_t win;
	struct list_head head;
};

#define list2cfg(item) list_entry(item, struct config, head)

struct list_head configs; /* keep track of configured but not mapped windows */

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
	WIN_POS_PRESERVE,
};

static uint8_t last_winpos;

enum dir {
	DIR_NEXT = 1,
	DIR_PREV,
};

struct arg {
	struct client *cli;
	struct keymap *kmap;
	uint32_t data;
};

static void walk_tags(struct arg *);
static void retag_client(struct arg *);
static void next_window(struct arg *);
static void prev_window(struct arg *);
static void raise_client(struct arg *);
static void place_window(struct arg *);
static void grow_window(struct arg *);
static void make_grid(struct arg *);
static void show_toolbar(struct arg *);
static void flag_window(struct arg *);

struct keymap {
	uint16_t mod;
	xcb_keysym_t sym;
	xcb_keycode_t key;
	char *keyname;
	const char *actname;
	void (*action)(struct arg *);
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
	  raise_client, 1, },
	{ MOD, XK_u, 0, "mod_u", "retag next",
	  retag_client, DIR_NEXT, },
	{ MOD, XK_y, 0, "mod_y", "retag prev",
	  retag_client, DIR_PREV, },
	{ MOD, XK_o, 0, "mod_o", "next tag",
	  walk_tags, DIR_NEXT, },
	{ MOD, XK_i, 0, "mod_i", "prev tag",
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
	{ MOD, XK_F1, 0, "mod_f1", "grow",
	  grow_window, WIN_POS_PRESERVE, },
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
	{ MOD, XK_F4, 0, "mod_f4", "show toolbar",
	  show_toolbar, },
	{ MOD, XK_F2, 0, "mod_f2", "flag window",
	  flag_window, },
	{ SHIFT, XK_Delete, 0, "shift_delete", "raise client",
	  raise_client, 1, },
};

#define list2keymap(item) list_entry(item, struct keymap, head)

static struct list_head keymap;

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
	BORDER_FG,
	FOCUS_FG,
	TITLE_FG,
};

static XftColor normal_fg;
static uint32_t normal_bg;
static XftColor active_fg;
static uint32_t active_bg;
static XftColor alert_fg;
static uint32_t alert_bg;
static XftColor notice_fg;
static uint32_t notice_bg;
static uint32_t border_fg;
static uint32_t focus_fg;
static XftColor title_fg;

static struct color defcolors[] = {
	{ "normal-fg", &normal_fg, 0xa0a0a0, COLOR_TYPE_XFT, },
	{ "normal-bg", &normal_bg, 0x202020, COLOR_TYPE_INT, },
	{ "active-fg", &active_fg, 0xc0c0c0, COLOR_TYPE_XFT, },
	{ "active-bg", &active_bg, 0x41749c, COLOR_TYPE_INT, },
	{ "alert-fg", &alert_fg, 0xc32d2d, COLOR_TYPE_XFT, },
	{ "alert-bg", &alert_bg, 0x90ae2b, COLOR_TYPE_INT, },
	{ "notice-fg", &notice_fg, 0x101010, COLOR_TYPE_XFT, },
	{ "notice-bg", &notice_bg, 0x90ae2b, COLOR_TYPE_INT, },
	{ "border-fg", &border_fg, 0x444444, COLOR_TYPE_INT, },
	{ "focus-fg", &focus_fg, 0x41749c, COLOR_TYPE_INT, },
	{ "title-fg", &title_fg, 0x90ae2b, COLOR_TYPE_XFT, },
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
static xcb_timestamp_t toolbox_time;

/* globals */

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
static uint8_t disp;

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

static const char *homedir;
static uint8_t homelen;
static struct toolbar_item *focused_item;
static uint8_t toolbar_pressed;
static uint8_t ignore_panel;

static uint8_t randrbase;

static void scan_clients(uint8_t rescan);

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

static struct client *pid2dock(pid_t pid)
{
	struct list_head *cur_scr;
	struct list_head *cur_cli;

	list_walk(cur_scr, &screens) {
		struct screen *scr = list2screen(cur_scr);

		list_walk(cur_cli, &scr->dock) {
			struct client *cli = list2cli(cur_cli);
			if (cli->pid == pid)
				return cli;
		}
	}

	ww("client with pid %u not found\n", pid);
	return NULL;
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

static void timestamp(struct client *cli)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
	cli->ts = (ts.tv_sec * 1000000000 + ts.tv_nsec) / 1000; /* us */
}

static enum winstatus window_status(xcb_window_t win)
{
	enum winstatus status;
	xcb_get_window_attributes_cookie_t c;
	xcb_get_window_attributes_reply_t *a;

	if (win == XCB_WINDOW_NONE)
		return WIN_STATUS_UNKNOWN;

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
		xcb_get_geometry_cookie_t c = xcb_get_geometry(dpy, win);
		xcb_get_geometry_reply_t *g;
		g = xcb_get_geometry_reply(dpy, c, NULL);
		if (!g)
			status = WIN_STATUS_UNKNOWN;
		free(g);
	}

	return status;
}

static struct client *validate_client(struct list_head *head)
{
	struct client *cli;

	if (head->next && (cli = list2cli(head->next))) {
		if (window_status(cli->win) != WIN_STATUS_UNKNOWN)
			return cli;
	}

	if (head->prev && (cli = list2cli(head->prev))) {
		if (window_status(cli->win) != WIN_STATUS_UNKNOWN)
			return cli;
	}

	return NULL;
}

static struct client *front_client(struct tag *tag)
{
	if (list_empty(&tag->clients))
		return NULL;
	else if (tag->front)
		return tag->front;
	else if (tag->visited)
		return tag->visited;
	else
		return validate_client(&tag->clients);
}

#ifndef TRACE
#define trace_tag_windows(tag) ;
#else
#define trace_tag_windows(tag) {\
	struct list_head *cur__;\
	struct client *front__ = front_client(tag);\
	ii("tag %s, clients %p | %s\n", tag->name, &tag->clients, __func__);\
	if (front__) {\
		list_walk(cur__, &tag->clients) {\
			struct client *tmp__ = list2cli(cur__);\
			if (tmp__ == front__) {\
				ii(" cli %p win %#x *\n", tmp__, tmp__->win);\
			} else {\
				ii(" cli %p win %#x\n", tmp__, tmp__->win);\
			}\
		}\
	}\
}
#endif

static void text_exts(const char *text, int len, uint16_t *w, uint16_t *h,
		      XftFont *font)
{
	XGlyphInfo ext;

	XftTextExtentsUtf8(xdpy, font, (XftChar8 *) text, len, &ext);

	dd("text: %s\n  x = %d\n  y = %d\n  width = %d\n  height = %d\n"
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

	title->w = x - space_width;
}

static inline void free_window_title(struct sprop *title)
{
	free(title->ptr);
	title->ptr = NULL;
	title->len = 0;
}

static uint8_t get_window_title(xcb_window_t win, struct sprop *title)
{
	get_sprop(title, win, a_net_wm_name, UINT_MAX);
	if (!title->ptr || !title->len) {
		get_sprop(title, win, XCB_ATOM_WM_NAME, UCHAR_MAX);
		if (!title->ptr || !title->len)
			return 0;
	}

	return 1;
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

	dd("scr %d tag '%s' title '%s'\n", scr->id, scr->tag->name, title.str);

	if (w > scr->items[PANEL_AREA_TITLE].w) {
		do {
			text_exts(title.str, title.len--, &w, &h, font1);
		} while (w > scr->items[PANEL_AREA_TITLE].w);
		title.str[title.len - 3] = '.';
		title.str[title.len - 2] = '.';
		title.str[title.len - 1] = '.';
	}

	draw_panel_text(&scr->panel, color2ptr(TITLE_FG),
			color2ptr(NORMAL_BG),
			scr->items[PANEL_AREA_TITLE].x,
			scr->items[PANEL_AREA_TITLE].w,
			title.str, title.len, font1, space_width);
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

static void border_color(xcb_window_t win, uint32_t color)
{
	uint32_t val[1] = { color, };
	uint16_t mask = XCB_CW_BORDER_PIXEL;
	xcb_change_window_attributes_checked(dpy, win, mask, val);
}

static void border_width(xcb_window_t win, uint16_t width)
{
	uint32_t val[1] = { width, };
	uint16_t mask = XCB_CONFIG_WINDOW_BORDER_WIDTH;
	xcb_configure_window_checked(dpy, win, mask, val);
}

static void init_motion(xcb_window_t win)
{
	xcb_grab_pointer(dpy, 0, rootscr->root,
			 XCB_EVENT_MASK_POINTER_MOTION |
			 XCB_EVENT_MASK_BUTTON_RELEASE,
			 XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
			 win, XCB_NONE, XCB_CURRENT_TIME);
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

	uint8_t offset = BORDER_WIDTH * 2;
	warp_pointer(cli->win, cli->w / 2 + offset, cli->h / 2 + offset);
}

static void restore_window(xcb_window_t win, struct screen **scr,
			   struct tag **tag)
{
	struct list_head *cur;
	FILE *f;
	char path[homelen + sizeof("/.session/0xffffffffffffffff")];
	uint8_t data[2];

	*scr = NULL;
	*tag = NULL;
	sprintf(path, "%s/.session/%#x", homedir, win);

	if (!(f = fopen(path, "r"))) {
		ww("skip restore win %#x, %s\n", win, strerror(errno));
		return;
	}

	data[0] = data[1] = 0;
	fread(data, sizeof(data), 1, f); /* continue with 0,0 upon failure */
	fclose(f);

	list_walk(cur, &screens) {
		struct screen *tmp = list2screen(cur);
		if (data[0] == tmp->id) {
			*scr = tmp;
			break;
		}
	}

	if (!*scr)
		return;

	list_walk(cur, &(*scr)->tags) {
		struct tag *tmp = list2tag(cur);
		if (data[1] == tmp->id) {
			*tag = tmp;
			tt("restore win %#x scr %d tag %d '%s'\n", win,
			   (*scr)->id, (*tag)->id, (*tag)->name);
			return;
		}
	}
}

static void store_window(xcb_window_t win, uint8_t *data, uint8_t size, uint8_t clean)
{
	FILE *f;
	char path[homelen + sizeof("/.session/0xffffffffffffffff")];

	sprintf(path, "%s/.session/%#x", homedir, win);

	if (clean) {
		errno = 0;
		unlink(path);
		tt("clean %s, errno=%d\n", path, errno);
		return;
	}

	if (!(f = fopen(path, "w+"))) {
		ee("fopen(%s) failed, %s\n", path, strerror(errno));
		return;
	}

	errno = 0;
	fwrite(data, size, 1, f); /* ignore errors */
	fclose(f);
}

static void store_client(struct client *cli, uint8_t clean)
{
	uint8_t data[2];

	if (window_status(cli->win) == WIN_STATUS_UNKNOWN) { /* gone */
		clean = 1;
	} else if (!cli->scr || !cli->tag) {
		clean = 1;
	} else if (cli->flags & CLI_FLG_POPUP) {
		return;
	}

	if (clean) {
		store_window(cli->win, NULL, 0, clean);
		tt("clean win %#x\n", cli->win);
	} else {
		data[0] = cli->scr->id;
		data[1] = cli->tag->id;
		store_window(cli->win, data, sizeof(data), clean);
		tt("store win %#x scr %d tag %d '%s'\n", cli->win,
		   cli->scr->id, cli->tag->id, cli->tag->name);
	}
}

static void space_halfh(struct screen *scr, uint16_t y, float div)
{
	scr->tag->space.x = scr->x;
	scr->tag->space.y = y;
	scr->tag->space.w = scr->w;
	scr->tag->space.h = scr->h - scr->h / div;
}

static void space_halfw(struct screen *scr, uint16_t x, float div)
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

static void free_client(struct client **ptr)
{
	struct client *cli = *ptr;
	struct list_head *cur;

	dd("free cli %p win %#x | prev %p next %p\n", cli, cli->win,
	   cli->head.prev, cli->head.next);

	list_walk(cur, &curscr->tags) {
		struct tag *tag = list2tag(cur);

		dd("tag '%s' anchor: %p visited: %p front: %p\n",
		tag->name, tag->anchor, tag->visited, tag->front);

		if (cli == tag->anchor) {
			tag->anchor = NULL;
			space_fullscr(curscr);
		}

		if (cli == tag->visited)
			tag->visited = NULL;

		if (cli == tag->front)
			tag->front = NULL;
	}

	if (cli == toolbox.cli)
		toolbox.cli = NULL;

	store_client(cli, 1);
	list_del(&cli->head);
	list_del(&cli->list);
	free(cli);
	*ptr = NULL;
}

static void update_dock(pid_t pid, char *msg)
{
	size_t len;
	xcb_client_message_event_t e;
	struct client *cli = pid2dock(pid);

	if (!msg || !cli)
		return;

	memset(&e, 0, sizeof(e));

	e.response_type = XCB_CLIENT_MESSAGE;
	e.window = cli->win;
	e.type = a_protocols;
	e.format = 8;

	len = strlen(msg);

	if (len > sizeof(e.data.data8) - 1)
		len = sizeof(e.data.data8) - 1;

	memcpy(e.data.data8, msg, len);
	xcb_send_event_checked(dpy, 0, cli->win, XCB_EVENT_MASK_NO_EVENT,
			       (const char *) &e);
	xcb_flush(dpy);

	ii("win %#x message '%s' len %zu\n", cli->win, msg, len);
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

	ii("> ping win %#x time %u (%d)\n", win, time, a_ping);
	xcb_send_event_checked(dpy, 0, win, XCB_EVENT_MASK_NO_EVENT, (const char *) &e);
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

static struct client *dock2cli(struct screen *scr, xcb_window_t win)
{
	struct list_head *cur;

	list_walk(cur, &scr->dock) {
		struct client *cli = list2cli(cur);

		if (cli->win == win)
			return cli;
	}

	return NULL;
}

static struct client *tag2cli(struct tag *tag, xcb_window_t win)
{
	struct list_head *cur;

	list_walk(cur, &tag->clients) {
		struct client *cli = list2cli(cur);

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

	if (win) {
		if (r->child == XCB_WINDOW_NONE ||
		    window_status(r->child) != WIN_STATUS_VISIBLE) {
			ww("ignore win %#x @%d,%d\n", r->child, *x, *y);
			*win = XCB_WINDOW_NONE;
		} else {
			dd("child win %#x @%d,%d\n", r->child, *x, *y);
			*win = r->child;
		}
	}

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
		struct client *cli = list2cli(head);
		if (cli->flags & CLI_FLG_POPUP)
			return NULL;
		else if (cli->win == win)
			return cli;
	}

        return NULL;
}

static void unfocus_window(xcb_window_t win)
{
	if (win == XCB_WINDOW_NONE)
		return;
	else if (win == toolbar.panel.win)
		return;
	else
		border_color(win, border_fg);

	xcb_window_t tmp = XCB_WINDOW_NONE;
	xcb_change_property(dpy, XCB_PROP_MODE_REPLACE, rootscr->root,
			    a_active_win, XCB_ATOM_WINDOW, 32, 1, &tmp);
}

static void focus_window(xcb_window_t win)
{
	if (win == toolbar.panel.win)
		return;
	else
		border_color(win, focus_fg);

	xcb_change_property_checked(dpy, XCB_PROP_MODE_REPLACE,
				    rootscr->root, a_active_win,
				    XCB_ATOM_WINDOW, 32, 1, &win);
	xcb_set_input_focus_checked(dpy, XCB_NONE, win, XCB_CURRENT_TIME);
	print_title(curscr, win);
}

static void unfocus_clients(struct tag *tag)
{
	struct list_head *cur;

	list_walk(cur, &tag->clients) {
		struct client *cli = list2cli(cur);
		unfocus_window(cli->win);
	}
}

static void raise_window(xcb_window_t win)
{
	uint32_t val[1] = { XCB_STACK_MODE_ABOVE, };
	uint16_t mask = XCB_CONFIG_WINDOW_STACK_MODE;

	xcb_configure_window_checked(dpy, win, mask, val);
}

static void print_menu(struct screen *scr)
{
	uint16_t x, w;

	x = scr->items[PANEL_AREA_MENU].x;
	w = scr->items[PANEL_AREA_MENU].w;

	draw_panel_text(&scr->panel, color2ptr(NORMAL_FG),
			color2ptr(NORMAL_BG), x, w, menu_icon, menu_icon_len,
			menu_font, space_width);
}

static void print_tag(struct screen *scr, struct tag *tag, uint8_t flag)
{
	struct color *fg;
	struct color *bg;

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
			tag->nlen, font1, space_width);
}

static void print_div(struct screen *scr)
{
	uint16_t x, w;

	x = scr->items[PANEL_AREA_DIV].x;
	w = scr->items[PANEL_AREA_DIV].w;

	draw_panel_text(&scr->panel, color2ptr(NORMAL_FG),
			color2ptr(NORMAL_BG), x, w, DIV_ICON,
			sizeof(DIV_ICON) - 1, font1, space_width);
}

static void raise_panel(struct screen *scr)
{
	struct list_head *cur;

	if (scr && scr->panel.win) {
		raise_window(scr->panel.win);

		list_walk(cur, &scr->dock)
			raise_window((list2cli(cur))->win);
	}
}

static void redraw_panel(struct screen *scr, struct client *cli, uint8_t raise)
{
	struct list_head *cur;

	fill_rect(scr->panel.win, scr->panel.gc, color2ptr(NORMAL_BG), scr->x,
		  0, scr->w, panel_height);

	print_menu(scr);

	list_walk(cur, &scr->tags) {
		struct tag *tag = list2tag(cur);
		print_tag(scr, tag, tag->flags);
	}

	print_div(scr);

	if (cli)
		print_title(scr, cli->win);

	if (raise)
		raise_panel(scr);
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

	dd("win %#x new geo %ux%u+%d+%d scr %d cli %p\n", cli->win, cli->w,
	   cli->h, cli->x, cli->y, cli->scr->id, cli);
}

static void move_toolbox(struct client *cli)
{
	uint32_t val[2];
	uint32_t mask;

	toolbox.x = (cli->x + cli->w) - toolbox.size;
	toolbox.y = (cli->y + cli->h) - toolbox.size;
	val[0] = toolbox.x;
	val[1] = toolbox.y;
	mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
	xcb_configure_window_checked(dpy, toolbox.win, mask, val);
}

static int toolbox_obscured(struct client *cli, int16_t x, int16_t y)
{
	struct list_head *cur;
	uint16_t d = toolbox.size - 2 * BORDER_WIDTH;

	list_walk(cur, &curscr->tag->clients) {
		struct client *it = list2cli(cur);
		int16_t xx;
		int16_t yy;
		uint16_t ww;
		uint16_t hh;

		if (it == cli)
			continue;

		(it->x < 0) ? (xx = curscr->x) : (xx = it->x);
		(it->y < 0) ? (yy = curscr->y) : (yy = it->y);

		ww = xx + it->w;
		hh = yy + it->h;

		if (cli->ts < it->ts &&
		    x >= xx && x + d <= ww && y >= yy && y + d <= hh) {
			return 1;
		}
	}

	return 0;
}

static int find_toolbox(struct client *cli)
{
	int16_t xx = curscr->x;
	uint16_t ww = curscr->x + curscr->w;
	int16_t yy = curscr->y;
	uint16_t hh = curscr->y + curscr->h;
	uint16_t bb = 2 * BORDER_WIDTH;
	uint16_t d = toolbox.size - bb;

	if (cli->x + cli->w >= xx && cli->x + cli->w <= ww &&
		   cli->y >= yy && cli->y <= hh &&
		   !toolbox_obscured(cli, cli->x + cli->w - d, cli->y)) {
		toolbox.x = cli->x + cli->w - d - bb;
		toolbox.y = cli->y + bb;
		toolbox.gravity = TOP_RIGHT;
		dd("TOP RIGHT is OK win %#x\n", cli->win);
	} else if (cli->x >= xx && cli->x <= ww &&
	    cli->y >= yy && cli->y <= hh &&
	    !toolbox_obscured(cli, cli->x, cli->y)) {
		toolbox.x = cli->x + bb;
		toolbox.y = cli->y + bb;
		toolbox.gravity = TOP_LEFT;
		dd("TOP LEFT is OK win %#x\n", cli->win);
	} else if (cli->x + cli->w >= xx && cli->x + cli->w <= ww &&
		   cli->y + cli->h >= yy && cli->y + cli->h <= hh &&
		   !toolbox_obscured(cli, cli->x + cli->w - d,
				     cli->y + cli->h - d)) {
		toolbox.x = cli->x + cli->w - d - bb;
		toolbox.y = cli->y + cli->h - d - bb;
		toolbox.gravity = BOTTOM_RIGHT;
		dd("BOT RIGHT is OK win %#x\n", cli->win);
	} else if (cli->x >= xx && cli->x <= ww &&
		   cli->y + cli->h >= yy && cli->y + cli->h <= hh &&
		   !toolbox_obscured(cli, cli->x, cli->y + cli->h - d)) {
		toolbox.x = cli->x + bb;
		toolbox.y = cli->y + cli->h - d - bb;
		toolbox.gravity = BOTTOM_LEFT;
		dd("BOT LEFT is OK win %#x\n", cli->win);
	} else {
		ww("no toolbox for win %#x geo %ux%u+%d+%d scr %u tag '%s'\n",
		   cli->win, cli->x, cli->y, cli->w, cli->h, cli->scr->id,
		   cli->scr->tag->name);
		return 0;
	}

	return 1;
}

static void hide_toolbox()
{
	toolbox.visible = 0;

	if (toolbox.win != XCB_WINDOW_NONE)
		xcb_unmap_window_checked(dpy, toolbox.win);
}

static void draw_toolbox(const char *str, uint8_t len)
{
	struct color *fg = color2ptr(NOTICE_FG);
	uint16_t x = (toolbox.size - FONT2_SIZE) / toolbox.xdiv;

	fill_rect(toolbox.win, toolbox.gc, color2ptr(NOTICE_BG), 1, 1,
	 toolbox.size - ITEM_V_MARGIN, toolbox.size - ITEM_V_MARGIN);

	XftDrawStringUtf8(toolbox.draw, fg->val, font2, x, text_yoffs,
			  (XftChar8 *) str, len);
	XSync(xdpy, 0);
}

static void show_toolbox(struct client *cli)
{
	uint32_t val[2];
	uint32_t mask;
	const char *str;
	uint8_t len;

	if (!cli || (cli && cli->flags & (CLI_FLG_POPUP | CLI_FLG_EXCLUSIVE)))
		return;

	if (!find_toolbox(cli)) {
		hide_toolbox();
		return;
	}

	raise_window(toolbox.win);
	mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
	val[0] = toolbox.x - BORDER_WIDTH * 3;
	val[1] = toolbox.y + BORDER_WIDTH;
	xcb_configure_window(dpy, toolbox.win, mask, val);
	xcb_map_window_checked(dpy, toolbox.win);

	if (motion_cli && (motion_cli->flags & CLI_FLG_MOVE)) {
		str = BTN_MOVE;
		len = slen(BTN_MOVE);
	} else if (curscr->tag->anchor == cli) {
		str = BTN_FLAG;
		len = slen(BTN_FLAG);
	} else {
		str = BTN_TOOLS;
		len = slen(BTN_TOOLS);
	}

	draw_toolbox(str, len);
	toolbox.cli = cli;
	toolbox.visible = 1;
}

static void toolbar_ungrab_input(void)
{
	xcb_ungrab_pointer(dpy, XCB_CURRENT_TIME);
	xcb_ungrab_key(dpy, toolbar.kprev, rootscr->root, 0);
	xcb_ungrab_key(dpy, toolbar.knext, rootscr->root, 0);
	xcb_ungrab_key(dpy, toolbar.kenter, rootscr->root, 0);
	xcb_ungrab_key(dpy, toolbar.kclose, rootscr->root, 0);
}

static void disable_events(xcb_window_t win)
{
	uint32_t val = 0;
	xcb_change_window_attributes_checked(dpy, win, XCB_CW_EVENT_MASK, &val);
	xcb_flush(dpy);
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
	title_width(toolbar.scr);

	if (toolbar.cli) {
		struct arg arg = { .cli = toolbar.cli, .kmap = NULL, };
		toolbar.cli = NULL;
		raise_client(&arg);
		center_pointer(arg.cli);
		show_toolbox(arg.cli);
		print_title(toolbar.scr, arg.cli->win);
	}

	xcb_flush(dpy);

	focused_item = NULL;
	toolbar.cli = NULL;
	toolbar.scr = NULL;
	toolbar.panel.win = XCB_WINDOW_NONE;

	dd("destroyed toolbar win %#x\n", toolbar.panel.win);
}

static void focus_root(void)
{
	toolbox.cli = NULL;
	hide_toolbox();

	toolbar.cli = NULL;
	hide_toolbar();

	print_title(curscr, XCB_WINDOW_NONE);
	xcb_set_input_focus_checked(dpy, XCB_NONE, rootscr->root,
				    XCB_CURRENT_TIME);
}

static void focus_any(pid_t pid)
{
	struct arg arg;

	curscr = pointer2scr();

	if (focus_root_) {
		focus_root_ = 0;
		focus_root();
		return;
	}

	arg.cli = NULL;
	arg.kmap = NULL;

	if (pid)
		arg.cli = pid2cli(pid);

	if (arg.cli)
		ii("pid %d win %#x\n", pid, arg.cli->win);
	else if (!arg.cli && !(arg.cli = front_client(curscr->tag)))
		arg.cli = pointer2cli();

	if (arg.cli) {
		if (window_status(arg.cli->win) != WIN_STATUS_VISIBLE) {
			ww("invisible front win %#x\n", arg.cli->win);
			arg.cli = NULL;
			curscr->tag->front = NULL;
		} else {
			ii("front win %#x\n", arg.cli->win);
			raise_client(&arg);
			center_pointer(arg.cli);
			return;
		}
	}

	focus_root();
	warp_pointer(rootscr->root, curscr->x + curscr->w / 2,
		     curscr->top + curscr->h / 2);
}

static void draw_hintbox(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
	fill_rect(toolbox.win, toolbox.gc, color2ptr(ALERT_BG), x + 1, y + 1,
		  w - 2 , h - 2);
}

static uint8_t hintbox_pos_;

static void show_hintbox(uint8_t pos)
{
	uint32_t val[2];
	uint16_t x, y, w, h;

	if (pos == WIN_POS_TOP_LEFT) {
		val[0] = curscr->x;
		val[1] = curscr->top;
		x = y = 0;
		w = h = toolbox.size / 2;
	} else if (pos == WIN_POS_BOTTOM_LEFT) {
		val[0] = curscr->x + BORDER_WIDTH;
		val[1] = curscr->top + curscr->h - toolbox.size - 2 * BORDER_WIDTH;
		x = 0;
		y = w = h = toolbox.size / 2;
	} else if (pos == WIN_POS_TOP_RIGHT) {
		val[0] = curscr->x + curscr->w - toolbox.size;
		val[1] = curscr->top;
		x = w = h = toolbox.size / 2;
		y = 0;
	} else if (pos == WIN_POS_BOTTOM_RIGHT) {
		val[0] = curscr->x + curscr->w - toolbox.size - BORDER_WIDTH;
		val[1] = curscr->top + curscr->h - toolbox.size - 2 * BORDER_WIDTH;
		x = y = w = h = toolbox.size / 2;
	} else if (pos == WIN_POS_TOP_FILL) {
		val[0] = curscr->x + curscr->w / 2 - toolbox.size / 2;
		val[1] = curscr->top;
		x = y = 0;
		w = toolbox.size;
		h = toolbox.size / 2;
	} else if (pos == WIN_POS_BOTTOM_FILL) {
		val[0] = curscr->x + curscr->w / 2 - toolbox.size / 2;
		val[1] = curscr->top + curscr->h - toolbox.size;
		x = 0;
		y = h = toolbox.size / 2;
		w = toolbox.size;
	} else if (pos == WIN_POS_LEFT_FILL) {
		val[0] = curscr->x;
		val[1] = curscr->top + curscr->h / 2 - toolbox.size / 2;
		x = y = 0;
		w = toolbox.size / 2;
		h = toolbox.size;
	} else if (pos == WIN_POS_RIGHT_FILL) {
		val[0] = curscr->x + curscr->w - toolbox.size;
		val[1] = curscr->top + curscr->h / 2 - toolbox.size / 2;
		x = w = toolbox.size / 2;
		y = 0;
		h = toolbox.size;
	} else {
		hintbox_pos_ = 0;
		return;
	}

	if (hintbox_pos_ != pos) {
		uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
		xcb_configure_window(dpy, toolbox.win, mask, val);
		xcb_map_window_checked(dpy, toolbox.win);
		draw_hintbox(x, y, w, h);
	}

	raise_window(toolbox.win);
	hintbox_pos_ = pos;
	toolbox.cli = NULL;
	toolbox.visible = 1;
}

static void init_toolbox(void)
{
	uint32_t val[2];
	uint32_t mask;

	if (toolbox.win != XCB_WINDOW_NONE)
		return;

	toolbox.size = panel_height;
	toolbox.win = xcb_generate_id(dpy);
	mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	val[0] = color2int(NORMAL_BG);
	val[1] = XCB_EVENT_MASK_BUTTON_PRESS;
	val[1] |= XCB_EVENT_MASK_BUTTON_RELEASE;
	val[1] |= XCB_EVENT_MASK_LEAVE_WINDOW;

	xcb_create_window(dpy, XCB_COPY_FROM_PARENT, toolbox.win,
			  rootscr->root, 0, 0, toolbox.size, toolbox.size, 0,
			  XCB_WINDOW_CLASS_INPUT_OUTPUT, rootscr->root_visual,
			  mask, val);

	border_color(toolbox.win, notice_bg);
	border_width(toolbox.win, BORDER_WIDTH);
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

static void close_client(struct client **ptr)
{
	struct client *cli = *ptr;

	ii("terminate pid %d cli %p win %#x\n", cli->pid, cli, cli->win);

	if (cli->pid)
		kill(cli->pid, SIGTERM);

	free_client(ptr);
}

static void reset_toolbar_item(struct toolbar_item *item, uint16_t w)
{
	uint16_t h;

	item->flags = 0;

	if (item->str == (const char *) BTN_FLAG && curscr->tag->anchor)
		item->flags = ITEM_FLG_LOCKED;

	item->x = w;
	text_exts(item->str, item->len, &item->w, &h, font2);
}

static void reset_toolbar(void)
{
	struct toolbar_item *ptr = toolbar_items;
	struct toolbar_item *end = toolbar_items + ARRAY_SIZE(toolbar_items);
	int32_t w;
	uint8_t reverse;

	if (toolbox.gravity == TOP_LEFT || toolbox.gravity == BOTTOM_LEFT) {
		reverse = 1;
		w =  ARRAY_SIZE(toolbar_items) * panel_height - panel_height;
	} else {
		reverse = 0;
		w = 0;
	}

	while (ptr < end) {
		reset_toolbar_item(ptr, w);
		reverse ? (w -= panel_height) : (w += panel_height);
		ptr++;
	}
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

static void close_window(xcb_window_t win)
{
	uint8_t i;
	struct timespec ts = { 0, 10000000 };
	struct client *cli;
	xcb_client_message_event_t e = { 0 };

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

	if (!(cli = win2cli(win)))
		goto out;

	cli->busy++; /* give client a chance to exit gracefully */

	for (i = 0; i < 50; ++i) { /* wait for 500 ms max */
		if (window_status(win) != WIN_STATUS_VISIBLE) {
			cli->busy = 0;
			break;
		}

		ww("%u: window %#x still open (%u)\n", i, win, cli->busy);
		nanosleep(&ts, NULL);
	}

	if (cli->busy > 2) {
		cli->busy = 0;
		close_client(&cli);
	} else if (!cli->busy) {
		free_client(&cli);
	}

	if (!cli) { /* client closed */
		hide_toolbox();
		toolbar.cli = NULL;
		hide_toolbar();
		focus_any(0);
	}

out:
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
			if (cli == list2cli(cli_head))
				return scr;
		}
	}
	return NULL;
#endif
}

static void draw_toolbar_text(struct toolbar_item *item, uint8_t flag)
{
	struct color *fg;
	struct color *bg;
	uint16_t item_w;
	uint16_t xx;

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

	if (item->str == (const char *) BTN_CLOSE)
		fg = color2ptr(ALERT_FG);

	item->flags |= flag;
	item_w = panel_height - ITEM_V_MARGIN;

	if (item->w % 2)
		item_w = item->w;
	else
		item_w = item->w + 1; /* text is more "centered" this way */

	xx = (panel_height - item_w) / 2;

	fill_rect(toolbar.panel.win, toolbar.panel.gc, bg, item->x, 1,
	 toolbox.size - ITEM_V_MARGIN, toolbox.size - ITEM_V_MARGIN);
	XftDrawStringUtf8(toolbar.panel.draw, fg->val, font2, item->x + xx,
	 text_yoffs, (XftChar8 *) item->str, item->len);
	XSync(xdpy, 0);
}

static void focus_toolbar_item(struct toolbar_item *item, int16_t x)
{
	uint8_t flg;
	uint16_t xx = item->x + panel_height - ITEM_V_MARGIN - 1;

	if (x < item->x || x > xx) { /* out of focus */
		if (item->flags & ITEM_FLG_LOCKED &&
		 curscr->tag->anchor == toolbar.cli) {
			item->flags &= ~ITEM_FLG_LOCKED; /* toggle lock */
			flg = ITEM_FLG_LOCKED;
		} else {
			flg = ITEM_FLG_NORMAL;
		}
	} else { /* in focus */
		flg = ITEM_FLG_FOCUSED;
		focused_item = item;
	}

	draw_toolbar_text(item, flg);
}

static void focus_toolbar_items(int16_t x, int16_t y)
{
	struct toolbar_item *ptr = toolbar_items;
	struct toolbar_item *end = toolbar_items + ARRAY_SIZE(toolbar_items);

	focused_item = NULL;

	if (y > panel_height) /* out of range */
		return;

	while (ptr < end) {
		focus_toolbar_item(ptr, x);
		ptr++;
	}
}

static void draw_toolbar(void)
{
	struct toolbar_item *ptr;
	ptr = &toolbar_items[ARRAY_SIZE(toolbar_items) - 1];

	warp_pointer(toolbar.panel.win, ptr->x, panel_height / 2);
	focus_toolbar_items(ptr->x, 0);
	xcb_flush(dpy);
}

static void *task(void *arg)
{
	system((const char *) arg);
	free(arg);
	return NULL;
}

static void run(const char *cmd)
{
	if (!cmd)
		return;

	const char *userhome = getenv("HOME");
	pthread_t t;

	if (userhome)
		chdir(userhome);

	pthread_create(&t, NULL, task, (void *) cmd);
}

static void spawn_cleanup(int sig)
{
	while (waitpid(-1, NULL, WNOHANG) < 0) {
		if (errno != EINTR)
			break;
	}
}

static void spawn(struct arg *arg)
{
	uint16_t len = homelen + sizeof("/keys/") + UCHAR_MAX;
	char *cmd;

	if (!(cmd = calloc(1, len)))
		return;

	snprintf(cmd, len, "%s/keys/%s", homedir, arg->kmap->keyname);
	run(cmd);
}

static void autostart(void)
{
	uint16_t len = homelen + sizeof("/autostart");
	char *cmd;

	if (!(cmd = calloc(1, len)))
		return;

	snprintf(cmd, len, "%s/autostart", homedir);
	run(cmd);
}

static void show_menu(void)
{
	uint16_t len = homelen + sizeof("/panel/menu");
	char *cmd;

	if (!(cmd = calloc(1, len)))
		return;

	snprintf(cmd, len, "%s/panel/menu", homedir);
	run(cmd);
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
		tt("append window %#x\n", cli->win);
		xcb_change_property(dpy, XCB_PROP_MODE_APPEND, rootscr->root,
				    a_client_list, XCB_ATOM_WINDOW, 32, 1,
				    &cli->win);
	}
}

#if 0
static uint8_t tray_window(xcb_window_t win)
{
	uint8_t ret;
	xcb_get_property_cookie_t c;
	xcb_get_property_reply_t *r;

	c = xcb_get_property(dpy, 0, win, a_embed_info,
			     XCB_GET_PROPERTY_TYPE_ANY, 0, 2 * 32);
	r = xcb_get_property_reply(dpy, c, NULL);
	if (!r || r->length == 0) {
		tt("_XEMBED_INFO (%d) is not available\n", a_embed_info);
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
#endif

static uint8_t dock_left(char *path, uint8_t offs,
			 const char *name, uint8_t len,
			 uint8_t scrid)
{
	struct stat st = {0};
	char *file;

	if (MAX_PATH - offs - sizeof("left-gravity/") < len) {
		ww("path exceeds maximum length %u\n", MAX_PATH);
		return 0;
	}

	file = &path[offs];
	*file = '\0';
	file = strcat(file, "left-gravity/");
	strncat(file, name, len);
	return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

static uint8_t dock_default(char *path, uint8_t offs,
			    const char *name, uint8_t len,
			    uint8_t scrid)
{
	struct stat st = {0};
	char *file;

	if (MAX_PATH - offs - 1 < len) {
		ww("path exceeds maximum length %u\n", MAX_PATH);
		return 0;
	}

	file = &path[offs];
	*file = '\0';
	strncat(file, name, len);
	return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

static uint32_t dock_gravity(char *path, const char *name, uint8_t len,
			    uint8_t scrid)
{
	char link[MAX_PATH] = {0};
	uint8_t n;

	n = snprintf(path, MAX_PATH, "%s/screens/%u/dock/", homedir, scrid);
	chdir(path);

	if (readlink("left-anchor", link, sizeof(link) - 1) > 0) {
		if (strncmp(link, name, len) == 0)
			return (CLI_FLG_LANCHOR | CLI_FLG_DOCK);
	}

	if (readlink("right-anchor", link, sizeof(link) - 1) > 0) {
		if (strncmp(link, name, len) == 0)
			return (CLI_FLG_RANCHOR | CLI_FLG_DOCK);
	}

	if (dock_left(path, n, name, len, scrid))
		return (CLI_FLG_LDOCK | CLI_FLG_DOCK);
	else if (dock_default(path, n, name, len, scrid))
		return CLI_FLG_DOCK;

	return 0;
}

static uint32_t dock_window(char *path, const char *name, uint8_t len)
{
	struct list_head *cur;
	const char *userhome = getenv("HOME");

	if (!name[0] || !len)
		return 0;

	list_walk(cur, &screens) {
		uint32_t flags;
		struct screen *scr = list2screen(cur);

		if ((flags = dock_gravity(path, name, len, scr->id))) {
			if (userhome)
				chdir(userhome);

			dockscr = scr;
			return flags;
		}
	}

	if (userhome)
		chdir(userhome);

	dockscr = NULL;
	return 0;
}

/* special windows
 *
 * 1) only single instance of given window will be allowed:
 *
 *    /<homedir>/exclusive/{<winclass1>,<winclassN>}
 *
 * 2) force window location:
 *
 *    /<homedir>/<sub-dirs>/{<winclass1>,<winclassN>}
 *
 *    sub-dirs: {center,top-left,top-right,bottom-left,bottom-right}
 *
 * 3) dock windows:
 *
 *    /<homedir>/screens/<screen>/dock/{<winclass1>,<winclassN>}
 *
 */

static uint32_t specialcrc(xcb_window_t win, const char *dir, uint8_t len,
			   uint32_t *crc)
{
	struct sprop class;
	char path[MAX_PATH];
	struct stat st;
	xcb_atom_t atom;
	uint8_t count;
	uint32_t flags;

	atom = XCB_ATOM_WM_CLASS;
	count = 0;
more:
	flags = 0;
	get_sprop(&class, win, atom, UCHAR_MAX);

	if (!class.ptr) {
		ww("unable to detect window class\n");
		return 0;
	}

	memset(path, 0, sizeof(path));

	if (dir[0] == 'd' && dir[1] == 'o' && dir[2] == 'c' && dir[3] == 'k') {
		flags = dock_window(path, class.str, class.len);
		count++; /* do dock check only once */
	} else {
		uint8_t n = snprintf(path, MAX_PATH, "%s/%s/", homedir, dir);

		if ((MAX_PATH - n - 1) < len) {
			flags = 0;
		} else {
			strncat(&path[n], class.str, class.len);
			flags = (class.str[0] && (stat(path, &st) == 0) &&
				 S_ISREG(st.st_mode));
		}
	}

	if (flags && crc) {
		*crc = crc32(class.str, class.len);
		dd("special win %#x, path %s, crc %#x\n", win, path, *crc);
	} else if (++count < 2) {
		free(class.ptr);
		atom = XCB_ATOM_WM_NAME;
		goto more;
	}

	free(class.ptr);
	return flags;
}

static uint32_t special(xcb_window_t win, const char *dir, uint8_t len)
{
	return specialcrc(win, dir, len, NULL);
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

static void setup_toolbar(struct client *cli)
{
	uint16_t w = ARRAY_SIZE(toolbar_items) * panel_height;
	uint32_t val[2], mask;

	reset_toolbar();

	toolbar.cli = cli;
	toolbar.scr = curscr;

	if (toolbox.gravity == TOP_LEFT || toolbox.gravity == BOTTOM_LEFT)
		toolbar.x = toolbox.x + toolbox.size + BORDER_WIDTH;
	else
		toolbar.x = toolbox.x - w - BORDER_WIDTH;

	toolbar.y = toolbox.y;

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

	border_color(toolbar.cli->win, notice_bg);
	xcb_create_window(dpy, XCB_COPY_FROM_PARENT, toolbar.panel.win,
			  rootscr->root,
			  0, 0, toolbar.panel.w, toolbar.panel.h,
			  0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  rootscr->root_visual, mask, val);
	border_color(toolbar.panel.win, notice_bg);
	border_width(toolbar.panel.win, BORDER_WIDTH);
	xcb_flush(dpy);

	toolbar.panel.gc = xcb_generate_id(dpy);
	mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND;
	val[0] = val[1] = color2int(NORMAL_BG);
        xcb_create_gc(dpy, toolbar.panel.gc, toolbar.panel.win, mask, val);

	toolbar.panel.draw = XftDrawCreate(xdpy, toolbar.panel.win,
					   DefaultVisual(xdpy, xscr),
					   DefaultColormap(xdpy, xscr));

	ii("toolbar %#x geo %ux%u+%d+%d target %#x\n", toolbar.panel.win,
	   toolbar.panel.w, toolbar.panel.h, toolbar.x, toolbar.y,
	   toolbar.cli->win);
}

static void show_toolbar(struct arg *arg)
{
	uint32_t val[4];
	uint32_t mask;

	if (!arg->cli)
		return;

	setup_toolbar(arg->cli);

	mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
	mask |= XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
	val[0] = toolbar.x;
	val[1] = toolbar.y + BORDER_WIDTH;
	val[2] = toolbar.panel.w;
	val[3] = toolbar.panel.h;
	xcb_configure_window(dpy, toolbar.panel.win, mask, val);

	ii("toolbar %#x on scr %d, tag %s\n", toolbar.panel.win,
	   toolbar.scr->id, toolbar.scr->tag->name);
	xcb_map_window_checked(dpy, toolbar.panel.win);
	draw_toolbar();
	title_width(toolbar.scr);
	print_title(toolbar.scr, toolbar.cli->win);
	raise_window(toolbar.panel.win);
	focus_window(toolbar.panel.win);
	toolbar_grab_input();
	xcb_flush(dpy);
}

static struct client *prev_client(struct client *cli)
{
	struct list_head *cur;

	list_back(cur, &cli->head) {
		cli = list2cli(cur);
		if (window_status(cli->win) == WIN_STATUS_VISIBLE)
			return cli;
	}

	return NULL;
}

static struct client *next_client(struct client *cli)
{
	struct list_head *cur;

	if (!cli) {
		ww("client has gone\n");
		return NULL;
	}

	list_walk(cur, &cli->head) {
		struct client *ret = list2cli(cur);

		if (window_status(ret->win) == WIN_STATUS_VISIBLE) {
			dd("ret %p %s:%d\n", ret, __func__, __LINE__);
			return ret;
		}
	}

	ww("no next valid window after %#x\n", cli->win);
	return cli;
}

static struct client *switch_window(struct screen *scr, enum dir dir)
{
	struct arg arg;

	hide_toolbox();
	hide_toolbar();

	if ((arg.cli = pointer2cli())) {
		tt("scr %u tag '%s' prev cli %p, win %#x (pointer)\n",
		   scr->id, scr->tag->name, arg.cli, arg.cli->win);
	} else if ((arg.cli = front_client(scr->tag))) {
		tt("scr %u tag '%s' prev cli %p, win %#x\n",
		   scr->id, scr->tag->name, arg.cli, arg.cli->win);
	} else {
		ee("no front windows found\n");
		return NULL;
	}

	if (arg.cli->scr) {
		curscr = arg.cli->scr;
		scr = curscr;
	}

	trace_tag_windows(curscr->tag);

	scr->flags |= SCR_FLG_SWITCH_WINDOW;

	if (dir == DIR_NEXT)
		arg.cli = next_client(arg.cli);
	else
		arg.cli = prev_client(arg.cli);

	if (!arg.cli)
		return NULL;

	tt("scr %u tag '%s' next cli %p, win %#x\n",
	   scr->id, scr->tag->name, arg.cli, arg.cli->win);

	arg.kmap = NULL;
	raise_client(&arg);

	if (!(scr->flags & SCR_FLG_SWITCH_WINDOW_NOWARP))
		center_pointer(arg.cli);

	scr->flags &= ~SCR_FLG_SWITCH_WINDOW_NOWARP;
	xcb_flush(dpy);

	return arg.cli;
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

static void make_grid(struct arg *arg)
{
	struct tag *tag = curscr->tag;
	struct list_head *cur;
	uint16_t i;
	uint16_t n = 0;
	uint16_t cw, ch; /* cell size */
	int16_t x, y;
	uint64_t ts;

	list_walk(cur, &tag->clients) {
		struct client *cli = list2cli(cur);

		if (tag->anchor == cli)
			continue;
		else if (cli->flags & CLI_FLG_POPUP)
			continue;
		else if (window_status(cli->win) == WIN_STATUS_VISIBLE)
			n++;
	}

	if (n == 0) {
		return;
	} else if (n == 1 && !tag->anchor) {
		return;
	} else if (n == 1 && tag->anchor) {
		tag->grid2v = (tag->anchor->h > tag->anchor->w);

		if (tag->grid2v) { /* vertical split */
			tag->space.w = curscr->w - tag->anchor->w;
			tag->space.w -= 2 * BORDER_WIDTH;
			cw = tag->space.w;
			ch = tag->space.h;
		} else { /* horizontal split */
			tag->space.h = curscr->h - tag->anchor->h;
			tag->space.h -= 2 * BORDER_WIDTH;
			cw = tag->space.w;
			ch = tag->space.h;
		}
	} else if (n == 2) {
		if (!arg->data) { /* only toggle via shortcut */
			if (tag->grid2v)
				tag->grid2v = 0;
			else
				tag->grid2v = 1;
		}

		if (tag->grid2v) { /* vertical split */
			cw = tag->space.w / 2;
			ch = tag->space.h;
		} else { /* horizontal split */
			cw = tag->space.w;
			ch = tag->space.h / 2;
		}
	} else if (!cell_size(n, &cw, &ch)) {
		return;
	}

	ii("%d cells size of (%u,%u)\n", n, cw, ch);
	i = x = y = 0;
	ts = 0;

	list_walk(cur, &tag->clients) {
		int16_t yy;
		int16_t xx;
		uint16_t hh;
		uint16_t ww;
		struct client *cli = list2cli(cur);

		if (ts) { /* equalize timestamps so toolbox will not be hidden */
			cli->ts = ts;
		} else {
			timestamp(cli);
			ts = cli->ts;
		}

		if (tag->anchor == cli)
			continue;
		else if (cli->flags & CLI_FLG_POPUP)
			continue;
		if (window_status(cli->win) != WIN_STATUS_VISIBLE)
			continue;

		xx = tag->space.x + x;
		ww = (tag->space.x + tag->space.w) - (xx + cw);
		(ww > 2 * BORDER_WIDTH) ? (ww = cw) : (ww += cw);
		ww -= 2 * BORDER_WIDTH - WINDOW_PAD;

		yy = tag->space.y + y;
		hh = (tag->space.y + tag->space.h) - (yy + ch);
		(hh > 2 * BORDER_WIDTH) ? (hh = ch) : (hh += ch);
		hh -= 2 * BORDER_WIDTH - WINDOW_PAD;

		if (++i >= n && n != 2) { /* last window occupies remaining space */
			ww = tag->space.w - 2 * BORDER_WIDTH;
			ww -= xx - tag->space.x;
		}

		client_moveresize(cli, xx, yy, ww, hh);
		x += cw;

		if (x > tag->space.w - cw) {
			x = 0;
			y += ch;
		}

		unfocus_window(cli->win);
	}

	focus_any(0);
	xcb_flush(dpy);
}

static void flag_window(struct arg *arg)
{
	struct toolbar_item *cur;
	struct toolbar_item *end;

	if (curscr->tag->anchor)
		border_color(curscr->tag->anchor->win, active_bg);

	if (arg->cli == curscr->tag->anchor) {
		curscr->tag->anchor = NULL;
		curscr->tag->space.x = curscr->x;
		curscr->tag->space.y = curscr->top;
		curscr->tag->space.w = curscr->w;
		curscr->tag->space.h = curscr->h;
	} else if ((curscr->tag->anchor = pointer2cli())) {
		curscr->tag->anchor->div = POS_DIV_MAX - 1;

		if ((arg->cli = curscr->tag->anchor)) {
			switch (arg->cli->pos) {
			case WIN_POS_LEFT_FILL:
			case WIN_POS_RIGHT_FILL:
			case WIN_POS_TOP_FILL:
			case WIN_POS_BOTTOM_FILL:
				break;
			default:
				arg->cli->pos = WIN_POS_BOTTOM_FILL;
			}

			border_color(arg->cli->win, notice_bg);
		}

		last_winpos = arg->cli->pos; /* to avoid div reset */
		arg->kmap = NULL;
		place_window(arg);
	}

	xcb_flush(dpy);
	show_toolbox(arg->cli);

	if (!toolbar.cli)
		return;

	cur = toolbar_items;
	end = toolbar_items + ARRAY_SIZE(toolbar_items);

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

static void window_halfh(uint16_t *w, uint16_t *h, float div)
{
	*w = curscr->w - 2 * BORDER_WIDTH;
	*h = curscr->h / div - 2 * BORDER_WIDTH - WINDOW_PAD;

	if (div == 2 && curscr->h % (uint8_t) div)
		(*h)++;
}

static void window_halfw(uint16_t *w, uint16_t *h, float div)
{
	*w = curscr->w / div - 2 * BORDER_WIDTH;
	*h = curscr->h - 2 * BORDER_WIDTH;
}

static void window_halfwh(uint16_t *w, uint16_t *h, uint8_t div)
{
	*w = curscr->w / div - 2 * BORDER_WIDTH - WINDOW_PAD;
	*h = curscr->h / div - 2 * BORDER_WIDTH - WINDOW_PAD;

	if (curscr->h % 2)
		(*h)++;
}

static void update_flagged_window(struct arg *arg, int16_t x, int16_t y,
 uint16_t w, uint16_t h, enum winpos pos)
{
	raise_client(arg);

	client_moveresize(arg->cli, x, y, w, h);
	move_toolbox(arg->cli);

	if (curscr->tag->anchor == arg->cli) {
		recalc_space(curscr, pos);
		arg->data = 1; /* disable vert/horiz toggle */
		make_grid(arg);
	}

	if (arg->cli != toolbar.cli)
		center_pointer(arg->cli);

	if (arg->cli->div == POS_DIV_MAX || arg->cli->div <= GROW_STEP_GLOW)
		border_color(arg->cli->win, alert_bg);

	if (!(arg->cli->flags & CLI_FLG_CENTER))
		arg->cli->inc = 0;

	store_client(arg->cli, 0);
	xcb_flush(dpy);
}

static void grow_window(struct arg *arg)
{
	int16_t x, y;
	uint16_t w, h;

	if ((arg->cli->flags & CLI_FLG_CENTER) ||
	 (arg->cli->flags & CLI_FLG_FULLSCREEN)) {
		return; /* ignore */
	}

	if (!arg->cli && curscr->tag->anchor)
		arg->cli = curscr->tag->anchor;
	else if (!arg->cli && !(arg->cli = pointer2cli()))
		return;

	switch (last_winpos) {
	case WIN_POS_LEFT_FILL:
		arg->cli->div -= GROW_STEP;
		if (arg->cli->div < GROW_STEP_MIN)
			arg->cli->div = 2;

		x = curscr->x;
		y = curscr->top;
		window_halfw(&w, &h, arg->cli->div);
		break;
	case WIN_POS_RIGHT_FILL:
		arg->cli->div -= GROW_STEP;
		if (arg->cli->div < GROW_STEP_MIN)
			arg->cli->div = 2;

		x = curscr->x + curscr->w - curscr->w / arg->cli->div;
		y = curscr->x;
		window_halfw(&w, &h, arg->cli->div);
		break;
	case WIN_POS_TOP_FILL:
		arg->cli->div -= GROW_STEP;
		if (arg->cli->div < GROW_STEP_MIN)
			arg->cli->div = 2;

		x = curscr->x;
		y = curscr->top;
		window_halfh(&w, &h, arg->cli->div);
		break;
	case WIN_POS_BOTTOM_FILL:
		arg->cli->div -= GROW_STEP;
		if (arg->cli->div < GROW_STEP_MIN)
			arg->cli->div = 2;

		x = curscr->x;
		y = curscr->top + curscr->h - curscr->h / arg->cli->div;
		window_halfh(&w, &h, arg->cli->div);
		break;
	default:
		return;
	}

	update_flagged_window(arg, x, y, w, h, last_winpos);
}

static void place_window(struct arg *arg)
{
	int16_t x, y;
	uint16_t w, h;
	enum winpos pos;

	if (!arg->cli && curscr->tag->anchor)
		arg->cli = curscr->tag->anchor;
	else if (!arg->cli && !(arg->cli = pointer2cli()))
		return;

	if (arg->kmap)
		pos = (enum winpos) arg->kmap->arg;
	else
		pos = (enum winpos) arg->cli->pos;

	tt("scr %d, win %#x, where %d, div %d\n", curscr->id, arg->cli->win,
	   pos, arg->cli->div);

	arg->cli->flags &= ~CLI_FLG_CENTER;
	arg->cli->flags &= ~CLI_FLG_FULLSCREEN;

	switch (pos) {
	case WIN_POS_FILL:
		tt("WIN_POS_FILL\n");
		arg->cli->flags |= CLI_FLG_FULLSCREEN;
		x = curscr->x;
		y = curscr->top;
		w = curscr->w - 2 * BORDER_WIDTH;
		h = curscr->h - 2 * BORDER_WIDTH;
		curscr->tag->anchor = NULL;
		break;
	case WIN_POS_CENTER:
		curscr->tag->anchor = NULL;
		arg->cli->flags |= CLI_FLG_CENTER;
		x = curscr->x + curscr->w / 2 - curscr->w / 4;
		y = curscr->top + curscr->h / 2 - curscr->h / 4;
		window_halfwh(&w, &h, 2);
		x -= arg->cli->inc;
		y -= arg->cli->inc;
		w += 2 * arg->cli->inc;
		h += 2 * arg->cli->inc;

		if (w > arg->cli->scr->w || h > arg->cli->scr->h)
			arg->cli->inc = 0;
		else
			arg->cli->inc += WIN_INC_STEP;

		break;
	case WIN_POS_LEFT_FILL:
		tt("WIN_POS_LEFT_FILL\n");

		if (last_winpos != pos || ++arg->cli->div > POS_DIV_MAX)
			arg->cli->div = 2;

		x = curscr->x;
		y = curscr->top;
		window_halfw(&w, &h, arg->cli->div);
		break;
	case WIN_POS_RIGHT_FILL:
		tt("WIN_POS_RIGHT_FILL\n");

		if (last_winpos != pos || ++arg->cli->div > POS_DIV_MAX)
			arg->cli->div = 2;

		x = curscr->x + curscr->w - curscr->w / arg->cli->div;
		y = curscr->x;
		window_halfw(&w, &h, arg->cli->div);
		break;
	case WIN_POS_TOP_FILL:
		tt("WIN_POS_TOP_FILL\n");

		if (last_winpos != pos || ++arg->cli->div > POS_DIV_MAX)
			arg->cli->div = 2;

		x = curscr->x;
		y = curscr->top;
		window_halfh(&w, &h, arg->cli->div);
		break;
	case WIN_POS_BOTTOM_FILL:
		tt("WIN_POS_BOTTOM_FILL\n");

		if (last_winpos != pos || ++arg->cli->div > POS_DIV_MAX)
			arg->cli->div = 2;

		x = curscr->x;
		y = curscr->top + curscr->h - curscr->h / arg->cli->div;
		window_halfh(&w, &h, arg->cli->div);
		break;
	case WIN_POS_TOP_LEFT:
		tt("WIN_POS_TOP_LEFT\n");

		if (last_winpos != pos || ++arg->cli->div > POS_DIV_MAX)
			arg->cli->div = 2;

		x = curscr->x;
		y = curscr->top;
		window_halfwh(&w, &h, arg->cli->div);
		break;
	case WIN_POS_TOP_RIGHT:
		tt("WIN_POS_TOP_RIGHT\n");

		if (last_winpos != pos || ++arg->cli->div > POS_DIV_MAX)
			arg->cli->div = 2;

		window_halfwh(&w, &h, arg->cli->div);
		x = curscr->x + curscr->w - w - BORDER_WIDTH * 2;
		y = curscr->top;
		break;
	case WIN_POS_BOTTOM_LEFT:
		tt("WIN_POS_BOTTOM_LEFT\n");

		if (last_winpos != pos || ++arg->cli->div > POS_DIV_MAX)
			arg->cli->div = 2;

		window_halfwh(&w, &h, arg->cli->div);
		x = curscr->x;
		y = curscr->top + curscr->h - h - BORDER_WIDTH * 2;
		break;
	case WIN_POS_BOTTOM_RIGHT:
		tt("WIN_POS_BOTTOM_RIGHT\n");

		if (last_winpos != pos || ++arg->cli->div > POS_DIV_MAX)
			arg->cli->div = 2;

		window_halfwh(&w, &h, arg->cli->div);
		x = curscr->x + curscr->w - w - BORDER_WIDTH * 2;
		y = curscr->top + curscr->h - h - BORDER_WIDTH * 2;
		break;
	default:
		return;
	}

	last_winpos = pos;
	update_flagged_window(arg, x, y, w, h, pos);
}

static void next_window(unused(struct arg *arg))
{
	switch_window(curscr, DIR_NEXT);
}

static void prev_window(unused(struct arg *arg))
{
	switch_window(curscr, DIR_PREV);
}

static void show_windows(struct tag *tag, uint8_t focus)
{
	struct arg arg = {0};
	struct list_head *cur;

	print_title(curscr, XCB_WINDOW_NONE); /* empty titlebar */

	if (list_empty(&tag->clients)) {
		focus_root();
		return;
	}

	list_walk(cur, &tag->clients) {
		struct client *cli = list2cli(cur);
		window_state(cli->win, XCB_ICCCM_WM_STATE_NORMAL);
		xcb_map_window_checked(dpy, cli->win);
	}

	if (!focus || !(arg.cli = front_client(tag)) ||
	    window_status(arg.cli->win) != WIN_STATUS_VISIBLE) {
		focus_root();
		return;
	}

	raise_client(&arg);
}

static void hide_windows(struct tag *tag)
{
	struct list_head *cur;

	list_walk(cur, &tag->clients) {
		struct client *cli = list2cli(cur);
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

	if (!(scr->flags & SCR_FLG_CLIENT_RETAG))
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

static void walk_tags(struct arg *arg)
{
	if (list_single(&curscr->tags))
		return;

	switch_tag(curscr, (enum dir) arg->kmap->arg);
	xcb_flush(dpy);
}

static void retag_client(struct arg *arg)
{
	struct tag *tag;
	struct list_head *cur;

	if (list_single(&curscr->tags))
		return;
	else if (!arg->cli && !(arg->cli = pointer2cli()))
		return;

	tag = arg->cli->tag;
	list_del(&arg->cli->head); /* remove from current tag */
	walk_tags(arg); /* switch to next tag */
	list_add(&curscr->tag->clients, &arg->cli->head); /* re-tag */
	arg->cli->scr = curscr;
	arg->cli->tag = curscr->tag;
	center_pointer(arg->cli);
	raise_client(arg);
	xcb_flush(dpy);

	if (!tag)
		return;

	list_walk(cur, &tag->clients) {
		struct client *cli = list2cli(cur);

		if (cli->win == XCB_WINDOW_NONE)
			continue;

		tag->front = cli;
		tag->visited = cli;
		return;
	}

	tag->front = NULL;
	tag->visited = NULL;
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

	get_sprop(&class, win, XCB_ATOM_WM_CLASS, UCHAR_MAX);
	if (!class.ptr) {
		ww("unable to detect window class\n");
		return NULL;
	}

	tt("scr %d win %#x class '%s'\n", scr->id, win, class.str);

	tag = NULL;
	class.len += homelen + sizeof("/screens/255/tags/255/");
	path = calloc(1, class.len);

	if (!path)
		goto out;

	list_walk(cur, &scr->tags) {
		tag = list2tag(cur);
		snprintf(path, class.len, "%s/screens/%d/tags/%d/%s", homedir,
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

static void arrange_dock(struct screen *scr)
{
	struct list_head *cur, *tmp;
	int16_t x, y;
	struct client *lanchor = NULL;
	struct client *ranchor = NULL;

	scr->items[PANEL_AREA_DOCK].x = scr->x + scr->w;
	scr->items[PANEL_AREA_DOCK].w = 0;

	x = scr->items[PANEL_AREA_DOCK].x;
	y = scr->panel.y + ITEM_V_MARGIN;

	list_walk_safe(cur, tmp, &scr->dock) { /* find l and r docks */
		struct client *cli = list2cli(cur);

		if (window_status(cli->win) == WIN_STATUS_UNKNOWN) { /* gone */
			list_del(&cli->head);
			list_del(&cli->list);
			free(cli);
			continue;
		}

		if (cli->flags & CLI_FLG_LANCHOR)
			lanchor = cli;
		else if (cli->flags & CLI_FLG_RANCHOR)
			ranchor = cli;
	}

	if (lanchor) {
		list_del(&lanchor->head);
		list_add(&scr->dock, &lanchor->head);
	}

	if (ranchor) {
		list_del(&ranchor->head);
		list_top(&scr->dock, &ranchor->head);
	}

	list_walk(cur, &scr->dock) {
		struct client *cli = list2cli(cur);

		x -= (cli->w + space_width + space_width * BORDER_WIDTH);
		client_moveresize(cli, x, y, cli->w, cli->h);
	}

	scr->items[PANEL_AREA_DOCK].x = x;
	scr->items[PANEL_AREA_DOCK].w = scr->items[PANEL_AREA_DOCK].x - x;
	title_width(scr);
	print_title(scr, XCB_WINDOW_NONE);
}

static void del_dock(struct client *cli)
{
	list_del(&cli->head);
	list_del(&cli->list);
	arrange_dock(cli->scr);
}

static void add_dock(struct client *cli, uint8_t bw)
{
	uint16_t h;

	if (dockscr)
		cli->scr = dockscr;

	if (cli->flags & CLI_FLG_LDOCK)
		list_add(&cli->scr->dock, &cli->head);
	else
		list_top(&cli->scr->dock, &cli->head);

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
		border_width(cli->win, BORDER_WIDTH);
		border_color(cli->win, border_fg);
		cli->flags |= CLI_FLG_BORDER;
	}

	arrange_dock(cli->scr);
	raise_window(cli->win);
	xcb_map_window_checked(dpy, cli->win);
	xcb_flush(dpy);
}

static uint8_t rescan_;

static void del_window(xcb_window_t win)
{
	struct screen *scr;
	struct client *cli;

	dd("delete win %#x\n", win);

	if (toolbar.cli && toolbar.cli->win == win)
		hide_toolbar();

	if (toolbox.cli && toolbox.cli->win == win)
		hide_toolbox();

	if (curscr->tag->front && curscr->tag->front->win == win)
		curscr->tag->front = NULL;

	if (curscr->tag->visited && curscr->tag->visited->win == win)
		curscr->tag->visited = NULL;

	if (curscr->tag->prev && curscr->tag->prev->win == win)
		curscr->tag->prev = NULL;

	scr = curscr;

	if (!(cli = win2cli(win))) {
		ii("deleted unmanaged win %#x\n", win);
		xcb_unmap_subwindows_checked(dpy, win);
		goto flush;
	}

	if (cli->flags & CLI_FLG_DOCK) {
		del_dock(cli);
		goto out;
	}

	if (cli->leader != XCB_WINDOW_NONE)
		rescan_ = 1;

	scr = cli2scr(cli); /* do it before client is freed */
	free_client(&cli);
	ii("deleted win %#x\n", win);

	if (scr)
		print_title(scr, XCB_WINDOW_NONE);

out:
	if (scr && scr->tag->prev && scr->tag->prev->win != XCB_WINDOW_NONE) {
		struct arg arg = { .cli = scr->tag->prev, .kmap = NULL, };
		raise_client(&arg);
		center_pointer(arg.cli);
	} else if (save_x_ < 0 || save_y_ < 0) {
		focus_any(0);
	} else {
		focus_window(rootscr->root);
		warp_pointer(rootscr->root, save_x_, save_y_);
	}

flush:
	update_client_list();
	xcb_flush(dpy);
}

static uint32_t window_exclusive(xcb_window_t win)
{
	uint32_t crc = 0;
	struct client *cli;
	struct list_head *cur, *tmp;

	specialcrc(win, "exclusive", sizeof("exclusive"), &crc);

	list_walk_safe(cur, tmp, &clients) {
		cli = glob2client(cur);

		if (cli->win == win) {
			ii("cleanup cli %p win %#x\n", cli, win);
			free_client(&cli);
		} else if (crc && cli->crc == crc && cli->win != win) {
			xcb_window_t old = cli->win;
			close_client(&cli);
			del_window(old);
			ii("exclusive win %#x crc %#x\n", win, crc);
		}
	}

	return crc;
}

static void map_window(xcb_window_t win)
{
	xcb_get_geometry_reply_t *g;

	g = xcb_get_geometry_reply(dpy, xcb_get_geometry(dpy, win), NULL);

	if (g) {
		warp_pointer(win, g->width / 2, g->height / 2);
		ii("map win %#x geo %ux%u+%d+%d\n", win, g->width, g->height,
		   g->x, g->y);
		free(g);
	}

	xcb_map_window_checked(dpy, win);
	raise_window(win);
	focus_window(win);
	xcb_flush(dpy);
}

static uint8_t ignore_window(xcb_window_t win)
{
	struct stat st;
	char path[1024];
	struct sprop title;

	if (get_window_title(win, &title)) {
		snprintf(path, sizeof(path), "%s/ignore/%s",
		  homedir, title.str);
		free_window_title(&title);
		if (stat(path, &st) == 0) {
			ww("ignore user defined win %#x %s\n", win,
			  path);
			map_window(win);
			return 1;
		}
	}

	return 0;
}

#if 0
static uint8_t ignore_systray(xcb_window_t win)
{
	struct stat st;
	char path[1024];
	struct sprop title;

	if (get_window_title(win, &title)) {
		char str[title.len + 1];
		memset(str, 0, title.len + 1);
		memcpy(str, title.str, title.len);
		free_window_title(&title);
		snprintf(path, sizeof(path), "%s/nosystray/%s",
		  homedir, str);
		ww("check nosystray for user defined win %#x %s\n", win, path);
		if (stat(path, &st) == 0) {
			ww("skip systray for user defined win %#x %s\n", win,
			  path);
			return 1;
		}
	}

	return 0;
}
#endif

static struct client *add_window(xcb_window_t win, uint8_t winflags)
{
	struct list_head *cur, *tmp;
	uint32_t flags;
	struct tag *tag;
	struct screen *scr;
	struct client *cli;
	uint32_t val[1];
	uint32_t crc;
	uint32_t grav;
	xcb_window_t leader;
	xcb_get_geometry_reply_t *g;
	xcb_get_window_attributes_cookie_t c;
	xcb_get_window_attributes_reply_t *a;

	/* save current pointer coords */
	pointer2coord(&save_x_, &save_y_, NULL);

	if (win == rootscr->root || win == toolbar.panel.win ||
	    win == toolbox.win || panel_window(win))
		return NULL;

	cli = NULL;
	a = NULL;
	g = xcb_get_geometry_reply(dpy, xcb_get_geometry(dpy, win), NULL);

	if (!g) {
		ee("failed to get geometry for win %#x\n", win);
		store_window(win, NULL, 0, 1);
		goto out;
	}

	c = xcb_get_window_attributes(dpy, win);
	a = xcb_get_window_attributes_reply(dpy, c, NULL);

	if (!a) {
		ee("xcb_get_window_attributes() failed\n");
		store_window(win, NULL, 0, 1);
		goto out;
	}

	leader = window_leader(win);

	if (leader != XCB_WINDOW_NONE)
		rescan_ = 1;

	if (!g->depth && !a->colormap) {
		tt("win %#x, root %#x, colormap=%#x, class=%u, depth=%u\n", win,
		   g->root, a->colormap, a->_class, g->depth);
		store_window(win, NULL, 0, 1);
		tt("ignore window %#x with unknown colormap\n", win);
		goto out;
	}

	flags = 0;

	if (ignore_window(win))
		goto out;
	else if ((grav = special(win, "dock", sizeof("dock"))))
		flags |= grav;
	else if (special(win, "center", sizeof("center")))
		flags |= CLI_FLG_CENTER;
	else if (special(win, "top-left", sizeof("top-left")))
		flags |= CLI_FLG_TOPLEFT;
	else if (special(win, "top-right", sizeof("top-right")))
		flags |= CLI_FLG_TOPRIGHT;
	else if (special(win, "bottom-left", sizeof("bottom-left")))
		flags |= CLI_FLG_BOTLEFT;
	else if (special(win, "bottom-right", sizeof("bottom-right")))
		flags |= CLI_FLG_BOTRIGHT;

#if 0
	if (!ignore_systray(win) &&
	  (tray_window(win) || winflags & WIN_FLG_TRAY)) {
		ii("win %#x provides embed info\n", win);
		flags |= CLI_FLG_TRAY;
	}
#else
	if (winflags & WIN_FLG_TRAY) {
		ii("win %#x has tray flag set\n", win);
		flags |= CLI_FLG_TRAY;
	}
#endif

	if (special(win, "popup", sizeof("popup"))) {
		flags &= ~(CLI_FLG_DOCK | CLI_FLG_TRAY);
		flags |= CLI_FLG_POPUP;
	}

	if (window_status(win) == WIN_STATUS_UNKNOWN) {
		ww("attempted to add invalid win %#x | %d\n", win, __LINE__);
		goto out;
	}

	if (!(flags & (CLI_FLG_DOCK | CLI_FLG_TRAY)))
		crc = window_exclusive(win);
	else
		crc = 0;

	if (g->width <= WIN_WIDTH_MIN || g->height <= WIN_HEIGHT_MIN) {
		/* FIXME: current assumption is that window is not supposed
		 * to be shown
		 */
		tt("ignore tiny window %#x geo %ux%u%+d%+d\n", win, g->width,
		   g->height, g->x, g->y);
		xcb_change_property_checked(dpy, XCB_PROP_MODE_APPEND,
					    rootscr->root, a_client_list,
					    XCB_ATOM_WINDOW, 32, 1, &win);
		xcb_flush(dpy);
		goto out;
	}

	if (leader != XCB_WINDOW_NONE && !win2cli(leader) &&
	    !(winflags & WIN_FLG_USER)) {
		ww("ignore win %#x with hidden leader %#x\n", win, leader);
		map_window(win);
		goto out;
	}

	scr = NULL;
	tag = NULL;

	if ((winflags & WIN_FLG_SCAN) && !(flags & (CLI_FLG_TRAY | CLI_FLG_DOCK)))
		restore_window(win, &scr, &tag);

	if (!scr && !(scr = pointer2scr())) {
		if ((cli = win2cli(win))) {
			ii("win %#x already on clients list\n", win);
			list_del(&cli->head);
			list_del(&cli->list);
		}
		scr = defscr;
	} else {
		if ((cli = dock2cli(scr, win))) {
			dd("destroy dock win %#x\n", win);
			close_client(&cli);
		} else if ((cli = tag2cli(scr->tag, win))) {
			ii("win %#x already on [%s] list\n", win,
			   scr->tag->name);
			list_del(&cli->head);
			list_del(&cli->list);
		}
	}

	tt("screen %d, win %#x, geo %ux%u+%d+%d\n", scr->id, win, g->width,
	   g->height, g->x, g->y);

	scr->flags &= ~SCR_FLG_SWITCH_WINDOW;

	if (!(flags & CLI_FLG_TRAY) && a->override_redirect) {
		tt("ignore redirected window %#x\n", win);
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
	cli->inc = 0;
	cli->scr = scr;
	cli->win = win;
	cli->leader = leader;
	cli->crc = crc;
	cli->flags = flags;
	cli->pid = win2pid(win);

#define DONT_CENTER (\
	CLI_FLG_TOPLEFT |\
	CLI_FLG_TOPRIGHT |\
	CLI_FLG_BOTLEFT |\
	CLI_FLG_BOTRIGHT |\
	CLI_FLG_TRAY |\
	CLI_FLG_DOCK\
)

	if (!(winflags & WIN_FLG_SCAN) &&
	    g->width < scr->w / 2 && g->height < scr->h / 2 &&
	    !(flags & DONT_CENTER)) {
		cli->flags |= CLI_FLG_CENTER; /* show such windows in center */
	}

	ignore_panel = 0;

	if (cli->flags & CLI_FLG_CENTER && !(winflags & WIN_FLG_SCAN)) {
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
		ii("add dock win %#x pid %u\n", cli->win, cli->pid);
		add_dock(cli, g->border_width);
		ignore_panel = 1; /* do not handle panel visibility */
		goto out;
	} else if (!(winflags & WIN_FLG_SCAN)) {
		g->x += scr->x;
		g->y += scr->y;
	}

	if (!(cli->flags & (CLI_FLG_TRAY | CLI_FLG_DOCK)))
		cli->tag = configured_tag(win); /* read tag from configuration */

	if (!cli->tag && tag) /* not configured, restore from last session */
		cli->tag = tag;
	else if (!cli->tag) /* nothing worked, map to current tag */
		cli->tag = scr->tag;

	border_width(cli->win, BORDER_WIDTH);

	/* subscribe events */
	val[0] = XCB_EVENT_MASK_ENTER_WINDOW;
	val[0] |= XCB_EVENT_MASK_PROPERTY_CHANGE;
	val[0] |= XCB_EVENT_MASK_STRUCTURE_NOTIFY;

	if (cli->flags & CLI_FLG_POPUP)
		val[0] |= XCB_EVENT_MASK_LEAVE_WINDOW;

	xcb_change_window_attributes_checked(dpy, win, XCB_CW_EVENT_MASK, val);
	unfocus_clients(curscr->tag);
	list_add(&cli->tag->clients, &cli->head);
	list_add(&clients, &cli->list); /* also add to global list of clients */
	client_moveresize(cli, g->x, g->y, g->width, g->height);

	if (scr->tag != cli->tag) {
		window_state(cli->win, XCB_ICCCM_WM_STATE_ICONIC);
		xcb_unmap_window_checked(dpy, cli->win);
	} else {
		window_state(cli->win, XCB_ICCCM_WM_STATE_NORMAL);
		xcb_map_window_checked(dpy, cli->win);
		center_pointer(cli);
	}

	list_walk_safe(cur, tmp, &configs) {
		struct config *cfg = list2cfg(cur);

		if (!cfg || !cfg->win || cfg->win != cli->win)
			continue;

		tt("remove win %#x from config list\n", cli->win);
		list_del(&cfg->head);
		free(cfg);
	}

	ii("added win %#x scr %d tag '%s' pid %d geo %ux%u%+d%+d cli %p leader %#x\n",
	   cli->win, scr->id, cli->tag->name, cli->pid, cli->w, cli->h,
	   cli->x, cli->y, cli, cli->leader);

	if (winflags & WIN_FLG_SCAN) {
		timestamp(cli);
		if (cli->tag == curscr->tag)
			curscr->tag->front = cli;
	} else if (!(flags & (CLI_FLG_TRAY | CLI_FLG_DOCK)) &&
		   !(winflags & WIN_FLG_SCAN)) {
		struct arg arg = { .cli = cli, .kmap = NULL, };
		arg.data = 1; /* do not update saved pointer coords */
		raise_client(&arg);
	}

	update_client_list();
	xcb_flush(dpy);

	/* workaround for some windows being not mapped at once */
	if (!cli->pid) {
		ww("== remap win %#x ==\n", cli->win);
		xcb_map_window_checked(dpy, cli->win);
		raise_window(cli->win);
		focus_window(cli->win);
		xcb_flush(dpy);
	}

out:
	free(a);
	free(g);
	return cli;
}

static void raise_client(struct arg *arg)
{
	if (!arg->cli && arg->kmap) {
		xcb_window_t win = pointer2win();
		if (win)
			add_window(win, WIN_FLG_USER);
		return;
	} else if (!arg->cli) {
		if ((arg->cli = front_client(curscr->tag)))
			unfocus_window(arg->cli->win);

		if (!(arg->cli = pointer2cli()))
			return;
	}

	if (curscr->tag->front) {
		curscr->tag->prev = curscr->tag->front;
		unfocus_window(curscr->tag->front->win);
	}

	if (curscr->tag->visited)
		unfocus_window(curscr->tag->visited->win);

	curscr->tag->visited = arg->cli;
	curscr->tag->front = arg->cli;
	raise_window(arg->cli->win);
	focus_window(arg->cli->win);
	timestamp(arg->cli);
	store_client(arg->cli, 0);

	if (arg->kmap && arg->kmap->arg == 1)
		toolbox.visible ? hide_toolbox() : show_toolbox(arg->cli);
	else
		show_toolbox(arg->cli);

	if (!arg->data)
		pointer2coord(&save_x_, &save_y_, NULL);
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

		if (cli)
			free_client(&cli);

		xcb_unmap_window_checked(dpy, leader);
	}
}

static void scan_clients(uint8_t rescan)
{
	int i, n, nn;
	xcb_query_tree_cookie_t c;
	xcb_query_tree_reply_t *tree;
	xcb_window_t *wins;
	struct client *cli;
	uint8_t flags;

	/* walk through windows tree */
	c = xcb_query_tree(dpy, rootscr->root);
	if (!(tree = xcb_query_tree_reply(dpy, c, 0))) {
		ee("xcb_query_tree_reply(...) failed\n");
		goto out;
	}

	nn = xcb_query_tree_children_length(tree);
	if (!(wins = xcb_query_tree_children(tree))) {
		n = 0;
		ee("xcb_query_tree_children(...) failed\n");
		goto out;
	}

	if (rescan)
		flags = WIN_FLG_USER;
	else
		flags = WIN_FLG_SCAN | WIN_FLG_USER;

	/* map clients onto the current screen */

	for (i = 0, n = 0; i < nn; i++) {
		if (screen_panel(wins[i]))
			continue;
		else if (wins[i] == toolbox.win)
			continue;
		else if (wins[i] == toolbar.panel.win)
			continue;
		else if (rescan && win2cli(wins[i]))
			continue;

		add_window(wins[i], flags);
		n++;

		if (rescan)
			continue;

		/* gotta do this otherwise empty windows are being shown
		 * in certain situations e.g. when adding systray clients
		 */
		hide_leader(wins[i]);
	}

out:
	ii("%d/%d windows added\n", n, nn);

	if (!rescan && (cli = front_client(curscr->tag))) {
		struct arg arg = { .cli = cli, .kmap = NULL, };
		raise_client(&arg);
		center_pointer(cli);
	}

	free(tree);
	xcb_flush(dpy);
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
	struct client *tmp;
	struct list_head *cur;
	struct tag *prev;

	toolbar.cli = NULL;
	hide_toolbar();

	if (scr->tag && tag_pointed(scr->tag, x, y)) {
		curscr->flags |= SCR_FLG_SWITCH_WINDOW_NOWARP;
		switch_window(curscr, DIR_NEXT);
		return;
	} else if (scr->tag) { /* deselect current tag instantly */
		print_tag(scr, scr->tag, ITEM_FLG_NORMAL);
	}

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

	if ((tmp = front_client(scr->tag)))
		show_toolbox(tmp);
	else
		hide_toolbox();

	xcb_set_input_focus(dpy, XCB_NONE, scr->panel.win, XCB_CURRENT_TIME);
}

static struct tag *get_tag(struct screen *scr, const char *name, uint8_t id)
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

static int add_tag(struct screen *scr, const char *name, uint8_t id,
		   uint16_t pos)
{
	uint8_t flg;
	struct tag *tag;
	uint16_t h;

	tag = get_tag(scr, name, id);
	if (!tag)
		return 0;

	text_exts(name, tag->nlen, &tag->w, &h, font1);
	tag->w += space_width * 2;
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
 * /<homedir>/screens/<screennumber>/tags/<tagnumber>/{.name,<winclass1>,<winclassN>}
 */

static int init_tags(struct screen *scr)
{
	uint16_t pos;
	uint8_t i;
	strlen_t len = homelen + sizeof("/screens/255/tags/255/.name");
	char path[len];
	char name[TAG_NAME_MAX + 1] = "0";
	int fd;
	struct stat st;
	uint8_t delete;
	struct list_head *cur;
	struct list_head *tmp;

	list_walk_safe(cur, tmp, &scr->tags) { /* reset tag list */
		struct tag *tag = list2tag(cur);
		list_del(&tag->head);
		free(tag->name);
		free(tag);
	}

	list_init(&scr->tags);
	list_init(&scr->dock);
	list_init(&clients);
	list_init(&configs);

	pos = scr->items[PANEL_AREA_TAGS].x;

	for (i = 0; i < UCHAR_MAX; i++ ) {
		delete = 0;
		st.st_mode = 0;
		sprintf(path, "%s/screens/%d/tags/%d", homedir, scr->id, i);

		if (stat(path, &st) < 0)
			delete = 1;
		if ((st.st_mode & S_IFMT) != S_IFDIR)
			delete = 1;

		if (delete && tag_del(scr, i))
			continue;

		sprintf(path, "%s/screens/%d/tags/%d/.name", homedir, scr->id, i);
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
		pos = add_tag(scr, name, i,  pos);
		memset(name, 0, TAG_NAME_MAX);
	}

	if (pos == scr->items[PANEL_AREA_TAGS].x) /* add default tag */
		pos = add_tag(scr, "*", 0, pos);

	return pos;
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
	redraw_panel(scr, NULL, 1);
	xcb_flush(dpy);
}

static void reinit_panel(struct screen *scr)
{
	int16_t x = 0;
	uint16_t h, w;

	/* clean panel */
	fill_rect(scr->panel.win, scr->panel.gc, color2ptr(NORMAL_BG), 0, 0,
		  scr->w, panel_height);
	move_panel(scr);

	text_exts(menu_icon, menu_icon_len, &w, &h, menu_font);

	scr->items[PANEL_AREA_MENU].x = TAG_GAP;
	scr->items[PANEL_AREA_MENU].w = w + 2 * space_width;
	x += scr->items[PANEL_AREA_MENU].w + 2 * TAG_GAP;
	print_menu(scr);

	scr->items[PANEL_AREA_TAGS].x = x;
	scr->items[PANEL_AREA_TAGS].w = init_tags(scr);

	scr->items[PANEL_AREA_DIV].x = scr->items[PANEL_AREA_TAGS].w;
	text_exts(DIV_ICON, sizeof(DIV_ICON) - 1, &w, &h, font1);
	w += scr->items[PANEL_AREA_DIV].x + 2 * space_width;
	scr->items[PANEL_AREA_DIV].w = w;
	print_div(scr);

	scr->items[PANEL_AREA_TITLE].x = scr->items[PANEL_AREA_DIV].w;

	arrange_dock(scr);
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
		reinit_panel(scr);
		scan_clients(0);
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
		ee("xcb_key_symbols_get_keycode(sym=%#x) failed\n", kmap->sym);
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
		ii("grab mod %#x + key %#x (sym=%#x)\n", kmap->mod,
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
		ee("xcb_key_symbols_get_keycode(sym=%#x) failed\n", XK_Left);
		return;
	}

	toolbar.kprev = *key;
	free(key);

	key = xcb_key_symbols_get_keycode(syms, XK_Right);
	if (!key) {
		ee("xcb_key_symbols_get_keycode(sym=%#x) failed\n", XK_Right);
		return;
	}

	toolbar.knext = *key;
	free(key);

	key = xcb_key_symbols_get_keycode(syms, XK_Return);
	if (!key) {
		ee("xcb_key_symbols_get_keycode(sym=%#x) failed\n", XK_Return);
		return;
	}

	toolbar.kenter = *key;
	free(key);

	key = xcb_key_symbols_get_keycode(syms, XK_Escape);
	if (!key) {
		ee("xcb_key_symbols_get_keycode(sym=%#x) failed\n", XK_Escape);
		return;
	}

	toolbar.kclose = *key;
	free(key);
}

static void init_keys(void)
{
	int tmp;
	uint16_t len = homelen + sizeof("/keys/") + UCHAR_MAX;
	char path[len], *ptr;
	char buf[actname_max];
	struct stat st;
	xcb_key_symbols_t *syms;
	uint8_t i;

	syms = xcb_key_symbols_alloc(dpy);
	if (!syms) {
		ee("xcb_key_symbols_alloc() failed\n");
		return;
	}

	snprintf(path, len, "%s/keys/", homedir);
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
			    sizeof("panel") - 1, "panel");

	xcb_change_property(dpy, XCB_PROP_MODE_REPLACE, scr->panel.win,
			    XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
			    sizeof("panel") - 1, "panel");

	scr->panel.gc = xcb_generate_id(dpy);
	mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND;
	val[0] = val[1] = color2int(NORMAL_BG);
	xcb_create_gc(dpy, scr->panel.gc, scr->panel.win, mask, val);

	xcb_map_window_checked(dpy, scr->panel.win);

	scr->panel.draw = XftDrawCreate(xdpy, scr->panel.win,
					DefaultVisual(xdpy, xscr),
					DefaultColormap(xdpy, xscr));

	ii("screen %d, panel %#x geo %ux%u+%d+%d\n", scr->id, scr->panel.win,
	   scr->w, panel_height, scr->x, scr->panel.y);
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

	xcb_send_event_checked(dpy, 0, rootscr->root, 0xffffff, (void *) &e);
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
		ww("systray owned by win %#x scr %d\n", r->owner, defscr->id);
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
	struct list_head *cur;
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

	chdir(homedir);

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
	list_walk(cur, &screens) {
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

	/* force refresh all panels and adjust each screen height */
	list_walk(cur, &screens) {
		scr = list2screen(cur);
		scr->h -= panel_height + PANEL_SCREEN_GAP;
		reinit_panel(scr);
	}

	trace_screens();
	init_tray();
	init_toolbox();
	focus_root();
	scan_clients(0);
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
			fprintf(f, "%#x", val);
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
	uint16_t len = homelen + sizeof("/colors/") + UCHAR_MAX;
	char path[len];
	struct color *ptr = defcolors;

	while (ptr->fname) {
		snprintf(path, len, "%s/colors/%s", homedir, ptr->fname);
		load_color(path, ptr++);
	}
}

static void focus_screen(uint8_t id)
{
	struct list_head *cur;

	list_walk(cur, &screens) {
		struct screen *scr = list2screen(cur);
		if (scr->id == id) {
			ii("focus screen %u tag %s\n", id, scr->tag->name);
			focus_tag(scr, scr->tag);
			curscr = scr;
			warp_pointer(rootscr->root, curscr->x + curscr->w / 2,
				     curscr->top + curscr->h / 2);
			return;
		}
	}
}

static void focus_tagwin_req(uint8_t id, xcb_window_t win)
{
	struct list_head *curscr;

	list_walk(curscr, &screens) {
		struct screen *scr = list2screen(curscr);
		struct list_head *curtag;

		list_walk(curtag, &scr->tags) {
			struct tag *tag = list2tag(curtag);
			struct client *cli;
			struct arg arg;

			if (tag->id != id) {
				continue;
			} else if ((cli = tag2cli(tag, win))) {
				arg.cli = cli;
				arg.kmap = NULL;
				raise_client(&arg);
				center_pointer(cli);
			}

			focus_tag(scr, tag);
			dd("focus scr %u tag %s win %#x\n", scr->id, tag->name,
			 win);
			return;
		}
	}
}

static void focus_window_req(xcb_window_t win)
{
	struct list_head *cur;

	if (win == XCB_WINDOW_NONE)
		return;

	list_walk(cur, &clients) {
		struct arg arg = { .cli = glob2client(cur), .kmap = NULL, };

		if (arg.cli->win == win) {
			struct screen *scr;

			if (!(scr = cli2scr(arg.cli))) {
				ww("cannot focus offscreen win %#x\n", win);
				return;
			}

			if (!arg.cli->tag) {
				ww("cannot focus tagless win %#x\n", win);
				return;
			}

			curscr = scr;
			focus_tag(curscr, arg.cli->tag);
			raise_client(&arg);
			center_pointer(arg.cli);

			ii("focus screen %u tag %s win %#x\n",
			   curscr->id,
			   arg.cli->tag ? arg.cli->tag->name : "<nil>",
			   win);
			return;
		}
	}
}

static uint32_t clients_list_seq;

static void update_seq()
{
	char path[homelen + sizeof("/tmp/.seq")];
	FILE *f;

	sprintf(path, "%s/tmp/.seq", homedir); /* NOTE: path storage re-used */

	if (!(f = fopen(path, "w+"))) {
		ee("fopen(%s) failed, %s\n", path, strerror(errno));
		return;
	}

	fprintf(f, "%u\n", clients_list_seq++);
	fclose(f);
}

static uint16_t count_clients(struct tag *tag)
{
	uint16_t clicnt = 0;
	struct list_head *cur;

	list_walk(cur, &tag->clients) {
		struct client *cli = list2cli(cur);
		enum winstatus stat = window_status(cli->win);

		if (stat != WIN_STATUS_UNKNOWN) {
			clicnt++;
		} else if (tag->front == cli) {
			tag->front = validate_client(cur);
			tag->visited = tag->front;
		}
	}

	return clicnt;
}

static void dump_tags(void)
{
	struct list_head *cur;
	char path[homelen + sizeof("/tmp/tags")];
	int fd;
	char buf[256] = {0};

	sprintf(path, "%s/tmp/tags", homedir);

	if ((fd = open(path, O_WRONLY | O_TRUNC | O_CREAT)) < 0) {
		ee("failed to open file %s, %s\n", path, strerror(errno));
		return;
	}

	list_walk(cur, &screens) {
		struct list_head *curtag;
		struct screen *scr = list2screen(cur);

		list_walk(curtag, &scr->tags) {
			struct tag *tag = list2tag(curtag);
			uint8_t current;
			uint16_t clicnt = count_clients(tag);
			struct client *cli;
			xcb_window_t win;

			if ((cli = front_client(tag)))
				win = cli->win;
			else
				win = XCB_WINDOW_NONE;

			curscr->tag == tag ? (current = 1) : (current = 0);
			snprintf(buf, sizeof(buf),
			  "%u\t%u\t%s\t%ux%u%+d%+d\t%u\t%u\t%#x\n", scr->id,
			  tag->id, tag->name, tag->w, panel_height,
			  tag->x, scr->panel.y, clicnt, current, win);
			write(fd, buf, strlen(buf));
			memset(buf, 0, sizeof(buf));
		}
	}

	close(fd);
	update_seq();
}

static void dump_screens(void)
{
	struct list_head *cur;
	char path[homelen + sizeof("/tmp/screens")];
	FILE *f;

	sprintf(path, "%s/tmp/screens", homedir);

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
	char path[homelen + sizeof("/tmp/clients")];
	struct list_head *cur;
	FILE *f;

	sprintf(path, "%s/tmp/clients", homedir);

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

		snprintf(temp, sizeof(temp), "%c\t%u\t%s\t%#x\t%d\t",
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
	char req[64] = {0};
	struct sprop name;

	if (fd < 0) {
		get_sprop(&name, rootscr->root, XCB_ATOM_WM_NAME, UCHAR_MAX);
		if (!name.ptr) {
			get_sprop(&name, rootscr->root, a_net_wm_name, UINT_MAX);
			if (!name.ptr)
				return;
		}
	} else {
		if ((name.len = read(fd, req, sizeof(req))) < 1) {
			ee("read(%d) failed, %s\n", fd, strerror(errno));
			return;
		}

		req[name.len] = '\0';
		name.str = req;
		name.ptr = NULL;
	}

	ii("handle request '%s' len %u\n", name.str, name.len);

	if (match(name.str, "reload-keys")) {
		init_keys();
	} else if (match(name.str, "lock")) {
		run(strdup("xscreensaver-command -lock"));
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
	} else if (match(name.str, "focus-tag")) {
		const char *tagstr = &name.str[sizeof("focus-tag")];
		char *winstr = NULL;
		uint8_t id;

		errno = 0;
		id = strtol(tagstr, &winstr, 10);
		if (!errno) {
			xcb_window_t win = strtol(winstr, NULL, 16);
			if (!errno)
				focus_tagwin_req(id, win);
		}
	} else if (match(name.str, "focus-window")) {
		const char *str = &name.str[sizeof("focus-window")];

		errno = 0;
		xcb_window_t win = strtol(str, NULL, 16);
		if (!errno)
			focus_window_req(win);
	} else if (match(name.str, "make-grid")) {
		struct arg arg = { .data = 0, };
		make_grid(&arg);
	} else if (match(name.str, "reload-colors")) {
		struct list_head *cur;

		init_colors();

		list_walk(cur, &screens) {
			redraw_panel(list2screen(cur), NULL, 1);
		}
	} else if (match(name.str, "update-dock")) {
		char *arg = &name.str[sizeof("update-dock")];

		if (arg) {
			char *msg;
			pid_t pid = strtol(arg, &msg, 10);

			if (pid && msg && *msg == ' ' && strlen(msg) > 1)
				update_dock(pid, ++msg);
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

static void toolbar_button_press(void)
{
	struct arg arg;

	if (!focused_item) {
		hide_toolbar();
		return;
	}

	arg.cli = toolbar.cli;
	arg.kmap = NULL;

	if (focused_item->str == (const char *) BTN_CLOSE) {
		if (toolbar.cli)
			close_window(toolbar.cli->win);
	} else if (focused_item->str == (const char *) BTN_LEFT) {
		arg.cli->pos = WIN_POS_LEFT_FILL;
		place_window(&arg);
	} else if (focused_item->str == (const char *) BTN_RIGHT) {
		arg.cli->pos = WIN_POS_RIGHT_FILL;
		place_window(&arg);
	} else if (focused_item->str == (const char *) BTN_TOP) {
		arg.cli->pos = WIN_POS_TOP_FILL;
		place_window(&arg);
	} else if (focused_item->str == (const char *) BTN_BOTTOM) {
		arg.cli->pos = WIN_POS_BOTTOM_FILL;
		place_window(&arg);
	} else if (focused_item->str == (const char *) BTN_CENTER) {
		arg.cli->pos = WIN_POS_CENTER;
		place_window(&arg);
	} else if (focused_item->str == (const char *) BTN_EXPAND) {
		arg.cli->pos = WIN_POS_FILL;
		place_window(&arg);
	} else if (focused_item->str == (const char *) BTN_FLAG) {
		if (toolbar.cli &&
		    (!curscr->tag->anchor ||
		    curscr->tag->anchor != toolbar.cli)) {
			curscr->tag->anchor = toolbar.cli;
			curscr->tag->anchor->div = 1;
			focused_item->flags |= ITEM_FLG_LOCKED;
			arg.cli = curscr->tag->anchor;
			arg.cli->pos = WIN_POS_LEFT_FILL;
			place_window(&arg);
		} else {
			curscr->tag->anchor = NULL;
			focused_item->flags &= ~ITEM_FLG_LOCKED;
			recalc_space(curscr, 0);
		}
	} else if (focused_item->str == (const char *) BTN_MOVE) {
		toolbar_pressed = 1; /* client window handles button release */
		arg.cli->flags |= CLI_FLG_MOVE;
		hide_toolbar(); /* special case */
		center_pointer(arg.cli);
		init_motion(arg.cli->win);
		raise_panel(curscr);
		xcb_flush(dpy);
		return; /* special case */
	}

	hide_toolbar();
}

static void panel_button_press(xcb_button_press_event_t *e)
{
	motion_cli = NULL;
	dd("screen %d, press at %d,%d\n", curscr->id, e->event_x, e->event_y);
	dump_coords(curscr, e->event_x);

	if area(curscr, PANEL_AREA_TAGS, e->event_x) {
		dd("panel press time %u\n", e->time);
		tag_time = e->time;
		select_tag(curscr, e->event_x, e->event_y);
		xcb_flush(dpy);
	} else if area(curscr, PANEL_AREA_TITLE, e->event_x) {
		curscr->flags |= SCR_FLG_SWITCH_WINDOW_NOWARP;
		switch_window(curscr, DIR_NEXT);
	} else if area(curscr, PANEL_AREA_DOCK, e->event_x) {
		ii("dock\n");
	} else if area(curscr, PANEL_AREA_DIV, e->event_x) {
		struct arg arg = { .cli = toolbox.cli, };
		show_toolbar(&arg);
	}
}

static void handle_button_release(xcb_button_release_event_t *e)
{
	struct client *cli;

	if (toolbar_pressed) {
		toolbar_pressed = 0; /* release toolbar press */
		return;
	} else if (e->event == rootscr->root && motion_cli &&
		   e->child == motion_cli->win) {
			motion_cli->flags &= ~CLI_FLG_MOVE;
	} else if (e->event == toolbox.win || e->child == toolbox.win) {
		if (e->time - toolbox_time < TAG_LONG_PRESS) {
			struct arg arg = { .cli = toolbox.cli, };
			show_toolbar(&arg);
		} else if (toolbox.cli) {
			struct arg arg = { .cli = toolbox.cli, .kmap = NULL, };
			arg.cli->flags |= CLI_FLG_MOVE;
			motion_cli = toolbox.cli;
			hide_toolbar();
			raise_client(&arg);
			center_pointer(arg.cli);
			init_motion(arg.cli->win);
			xcb_flush(dpy);
		}

		return;
	}

	cli = win2cli(e->child);

	if (cli && cli->flags & CLI_FLG_MOVE) {
		cli->flags &= ~CLI_FLG_MOVE;
		return;
	}

	curscr = coord2scr(e->root_x, e->root_y);

	if (curscr->panel.win == e->event || curscr->panel.win == e->child) {
		if area(curscr, PANEL_AREA_TAGS, e->event_x) {
			dd("panel release time %u\n", e->time - tag_time);
			if (e->time - tag_time > TAG_LONG_PRESS)
				ii("panel long press\n");
			motion_retag(e->event_x, e->event_y);
		} else if area(curscr, PANEL_AREA_MENU, e->event_x) {
			show_menu(); /* show menu on button release otherwise
					app will fail to grab pointer */
		}
	} else if (cli && cli->pos) {
		struct arg arg = { .data = 0, .cli = cli, };
		place_window(&arg);
	}

	if (toolbar.panel.win != e->event && toolbar.panel.win != e->child)
		toolbar_ungrab_input();

	xcb_flush(dpy);
}

static void handle_button_press(xcb_button_press_event_t *e)
{
	struct client *cli;

	te("XCB_BUTTON_PRESS: root %#x, pos %d,%d; event %#x, pos %d,%d; "
	   "child %#x, detail %d\n", e->root, e->root_x, e->root_y, e->event,
	   e->event_x, e->event_y, e->child, e->detail);

	if (toolbox.win &&
	    (toolbox.win == e->event || toolbox.win == e->child)) {
		ii("toolbox press time %u\n", e->time);
		toolbox_time = e->time;
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
			toolbar_button_press();
			return;
		} else {
			if ((cli = win2cli(e->child)))
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

	if (!(cli = tag2cli(curscr->tag, e->child))) {
		raise_window(e->child);
	} else {
		struct arg arg = { .cli = cli, .kmap = NULL, };
		raise_client(&arg);
		curscr->tag->front = cli;
	}

	raise_panel(curscr);

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
	border_color(cli->win, color);
}

static void handle_motion_notify(xcb_motion_notify_event_t *e)
{
	uint16_t mask;
	uint32_t val[2];
	struct client *cli;

	te("XCB_MOTION_NOTIFY: root %+d%+d, event %#x %+d%+d, child %#x\n",
	   e->root_x, e->root_y, e->event, e->event_x, e->event_y, e->child);

	if (e->event == toolbar.panel.win) {
		focus_toolbar_items(e->event_x, e->event_y);
		return;
	} else if (e->child == toolbox.win) {
		return;
	}

	curscr = coord2scr(e->root_x, e->root_y);

	if (!curscr)
		return;

	trace_screen_metrics(curscr);
	cli = win2cli(e->child); /* window is being moved so search in global list */

	if (cli && cli->flags & (CLI_FLG_DOCK | CLI_FLG_TRAY)) {
		ww("win %#x is not moveable\n", cli->win);
		return;
	} else if (curscr && curscr->panel.win == e->child) {
		/* panel window is a child in motion with modifier key */
		handle_panel_motion(e->event_x, e->event_y);
		return;
	} else if (!e->child || !cli) {
		if (e->child)
			ww("win %#x is not managed\n", e->child);
		return;
	}

	motion_place(cli, e->root_x, e->root_y);

	/* save initial coords on the first move */

	if (!motion_init_x)
		motion_init_x = cli->x;

	if (!motion_init_y)
		motion_init_y = cli->y;

	if (!hintbox_pos_)
		hide_toolbox();

	if (motion_cli && !(motion_cli->flags & CLI_FLG_MOVE))
		motion_cli = NULL;
	else
		motion_cli = cli;

	cli->x = e->root_x - cli->w / 2 - BORDER_WIDTH;
	cli->y = e->root_y - cli->h / 2 - BORDER_WIDTH;

	mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
	val[0] = cli->x;
	val[1] = cli->y;
	xcb_configure_window_checked(dpy, cli->win, mask, val);
	timestamp(cli);

	if (cli->scr != curscr && !(cli->flags & CLI_FLG_DOCK)) { /* retag */
		list_del(&cli->head);
		list_add(&curscr->tag->clients, &cli->head);
		cli->tag = curscr->tag;
		cli->scr = curscr;
		ii("win %#x now on tag %s screen %d\n", e->child,
		   curscr->tag->name, curscr->id);
		store_client(cli, 0);
	}

	show_hintbox(cli->pos);
	xcb_flush(dpy);
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
		toolbar_button_press();
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
		if ((--focused_item) + 1 == toolbar_items)
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

static void handle_key_release(xcb_key_release_event_t *e)
{
	te("XCB_KEY_RELEASE: root %#x, win %#x, child %#x, key %d\n",
	  e->root, e->event, e->child, e->detail);

	curscr->flags &= ~SCR_FLG_SWITCH_WINDOW;

	if (curscr->flags & SCR_FLG_CLIENT_RETAG) {
		curscr->flags &= ~SCR_FLG_CLIENT_RETAG;
		show_windows(curscr->tag, 1);
	}
}

static void handle_key_press(xcb_key_press_event_t *e)
{
	struct list_head *cur;

	if (toolbar.panel.win &&
	    (e->event == toolbar.panel.win || e->child == toolbar.panel.win)) {
		toolbar_key_press(e);
		return;
	}

	tt("screen %d, event %#x, child %#x, key %#x, state %#x, pos %d,%d\n",
	   curscr->id, e->event, e->child,
	   e->detail, e->state, e->root_x, e->root_y);

	if (e->event == rootscr->root)
		e->event = e->child;

	list_walk(cur, &keymap) {
		struct keymap *kmap = list2keymap(cur);

		if (kmap->key == e->detail && kmap->mod == e->state) {
			struct arg arg = {
				.kmap = kmap,
				.data = 0,
				.cli = win2cli(e->event),
			};

			curscr = coord2scr(e->root_x, e->root_y);
			curscr->tag->front = arg.cli;

			if (kmap->action == retag_client)
				curscr->flags |= SCR_FLG_CLIENT_RETAG;

			kmap->action(&arg);
			return;
		}
	}
}

static void handle_visibility(xcb_window_t win)
{
	struct list_head *cur;

	if (win == toolbar.panel.win) {
		draw_toolbar();
	} else if (!ignore_panel) {
		list_walk(cur, &screens) {
			struct screen *scr = list2screen(cur);

			if (scr->panel.win == win) {
				redraw_panel(scr, front_client(curscr->tag), 0);
				struct client *cli = pointer2cli();
				if (cli) {
					unfocus_clients(scr->tag);
					focus_window(cli->win);
				}
				xcb_flush(dpy);
				return;
			}
		}
	}
}

static void handle_unmap_notify(xcb_unmap_notify_event_t *e)
{
	struct list_head *cur;
	xcb_window_t leader;

	if (window_status(e->window) == WIN_STATUS_UNKNOWN) {
		tt("window %#x gone\n", e->window);
		del_window(e->window);
		return;
	}

	if (!(leader = window_leader(e->window)))
		return;

	list_walk(cur, &configs) {
		struct config *cfg = list2cfg(cur);

		if (!cfg || !cfg->win || window_leader(cfg->win) != leader)
			continue;

		add_window(cfg->win, WIN_FLG_USER);
		break;
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

	if (curscr->tag->visited)
		unfocus_window(curscr->tag->visited->win);

	if (curscr->tag->front)
		unfocus_window(curscr->tag->front->win);

	if (!(cli = pointer2cli())) {
		if (!(cli = win2cli(e->event))) {
			ww("enter unmanaged window %#x\n", e->event);
			return;
		}
	}

	if (e->mode == MOD)
		raise_window(e->event);

	curscr->tag->visited = cli;
	show_toolbox(cli);
	unfocus_clients(curscr->tag);
	focus_window(cli->win);
	redraw_panel(cli->scr, cli, 1);
	xcb_flush(dpy);
}

static void handle_leave_notify(xcb_leave_notify_event_t *e)
{
	struct client *cli;

	if (e->event == toolbox.win)
		return;

	if ((cli = win2cli(e->event)) && cli->flags & CLI_FLG_POPUP) {
		close_client(&cli);
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
				font1, space_width);
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
			dd("_NET_WM_USER_TIME from win %#x time %u state %u\n",
			   e->window, e->time, e->state);
	} else if (e->atom == a_has_vt) {
		struct list_head *cur;

		list_walk(cur, &screens) {
			redraw_panel(list2screen(cur), NULL, 1);
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
#endif

static void handle_configure_notify(xcb_configure_notify_event_t *e)
{
	print_configure_notify(e);

	if (e->event == rootscr->root && e->window == rootscr->root) {
		struct screen *scr = pointer2scr();
		if (scr) /* update because screen geo could change */
			reinit_panel(scr);
	} else if (e->window != rootscr->root && e->border_width) {
		border_width(e->window, BORDER_WIDTH);
		xcb_flush(dpy);
	}
}

static void tray_add(xcb_window_t win)
{
	struct client *cli;

	ii("%s: win %#x\n", __func__, win);

	if (!(cli = win2cli(win))) {
		if (!(cli = add_window(win, WIN_FLG_TRAY))) {
			ee("add_window(%#x) failed\n", win);
			return;
		}
	}
}

#define SYSTEM_TRAY_REQUEST_DOCK 0
#define SYSTEM_TRAY_BEGIN_MESSAGE 1
#define SYSTEM_TRAY_CANCEL_MESSAGE 2

static void handle_client_message(xcb_client_message_event_t *e)
{
	struct arg arg;

	print_atom_name(e->type);
	tt("win %#x data[] = { %u, %u, %u, ... }, format %d type %d\n",
	   e->window, e->data.data32[0], e->data.data32[1], e->data.data32[2],
	   e->format, e->type);
	print_atom_name(e->data.data32[0]);
	print_atom_name(e->data.data32[1]);

	if (e->type == a_net_wm_state &&  e->format == 32 &&
	    e->data.data32[1] == a_maximize_win) {
		tt("maximize win %#x\n", e->window);
		if ((arg.cli = win2cli(e->window))) {
			arg.cli->pos = WIN_POS_FILL;
			place_window(&arg);
		}
	} else if (e->type == a_net_wm_state &&  e->format == 32 &&
		   e->data.data32[1] == a_fullscreen) {
		tt("fullscreen win %#x\n", e->window);
		if ((arg.cli = win2cli(e->window))) {
			arg.cli->pos = WIN_POS_FILL;
			place_window(&arg);
		}
	} else if (e->type == a_net_wm_state &&  e->format == 32 &&
		   e->data.data32[1] == a_hidden) {
		tt("_NET_WM_STATE_HIDDEN from win %#x\n", e->window);
	} else if (e->type == a_protocols && e->format == 32 &&
		   e->data.data32[0] == a_ping) {
		tt("pong win %#x time %u\n", e->data.data32[2],
		   e->data.data32[1]);
	} else if (e->type == a_systray && e->format == 32 &&
		   e->data.data32[1] == SYSTEM_TRAY_REQUEST_DOCK) {
		tray_add(e->data.data32[2]);
	} else if (e->type == a_active_win && e->format == 32) {
		arg.cli = win2cli(e->window);

		if (!arg.cli || (arg.cli->flags & CLI_FLG_DOCK))
			return;

		focus_tag(arg.cli->scr, arg.cli->tag);
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
	   " parent=%#x\n"
	   " window=%#x\n"
	   " sibling=%#x\n"
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
	struct config *cfg;
	struct list_head *cur;
	struct client *cli = NULL;
	uint32_t val[7] = { 0 };
	int i = 0;
	uint16_t w, h, mask = 0;
	int16_t x, y;

	list_walk(cur, &defscr->dock) {
		cli = list2cli(cur);
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
		val[i++] = y;
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
		usleep(WIN_DELAY_US);
		add_window(e->window, WIN_FLG_USER);
		return;
	}

	if (!(cfg = calloc(1, sizeof(*cfg)))) {
		ee("failed to track win %#x\n", e->window);
	} else {
		cfg->win = e->window;
		list_add(&configs, &cfg->head);
	}

	tt("config win %#x geo orig %ux%u+%d+%d ? new %ux%u+%d+%d\n",
	   e->window,
	   e->width, e->height, e->x, e->y,
	   w, h, x, y);
	xcb_configure_window_checked(dpy, e->window, mask, val);
	xcb_flush(dpy);
}

static void handle_randr_notify(xcb_randr_screen_change_notify_event_t *e)
{
	/* Do not call init_outputs() here just print message for information.
         ^
	 * The reason for this is that CRTC info is not always available upon
	 * this notification which causes some not so very nice side effects
	 * during output initialization.
	 *
	 * Instead allow user to refresh outputs info in controlled way by
	 * issuing 'reinit-outputs' command via IPC
	 */
	ii("root %#x, win %#x, sizeID %u, w %u, h %u, mw %u, mh %u\n",
	   e->root, e->request_window, e->sizeID, e->width, e->height,
	   e->mwidth, e->mheight);
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
	dd("got event %d (%d)\n", e->response_type, type);

#define WIN(struct_ptr) ((struct_ptr *) e)->window

	switch (type) {
	case 0: break; /* NO EVENT */
	case XCB_VISIBILITY_NOTIFY:
		te("XCB_VISIBILITY_NOTIFY: win %#x, state %d\n",
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
		te("XCB_EXPOSE: win %#x\n", WIN(xcb_expose_event_t));
		handle_visibility(WIN(xcb_expose_event_t));
		break;
	case XCB_BUTTON_PRESS:
		handle_button_press((xcb_button_press_event_t *) e);
		break;
	case XCB_BUTTON_RELEASE:
		te("XCB_BUTTON_RELEASE: pos %d,%d, event %#x, child %#x\n",
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
		te("XCB_CONFIGURE_REQUEST: win %#x\n",
		   ((xcb_configure_request_event_t *) e)->window);
		print_configure_request((xcb_configure_request_event_t *) e);
		handle_configure_request((xcb_configure_request_event_t *) e);
		break;
	case XCB_DESTROY_NOTIFY:
		te("XCB_DESTROY_NOTIFY: event %#x, win %#x\n",
		   ((xcb_destroy_notify_event_t *) e)->event,
		   ((xcb_destroy_notify_event_t *) e)->window);
		del_window(((xcb_destroy_notify_event_t *) e)->window);
		break;
	case XCB_ENTER_NOTIFY:
		te("XCB_ENTER_NOTIFY: root %#x, event %#x, child %#x\n",
		   ((xcb_enter_notify_event_t *) e)->root,
		   ((xcb_enter_notify_event_t *) e)->event,
		   ((xcb_enter_notify_event_t *) e)->child);
		te("detail %#x, state %#x, mode %#x\n",
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
		te("XCB_KEY_PRESS: root %#x, win %#x, child %#x, key %d\n",
		   ((xcb_key_press_event_t *) e)->root,
		   ((xcb_key_press_event_t *) e)->event,
		   ((xcb_key_press_event_t *) e)->child,
		   ((xcb_key_press_event_t *) e)->detail);
		handle_key_press((xcb_key_press_event_t *) e);
		break;
	case XCB_KEY_RELEASE:
		handle_key_release((xcb_key_release_event_t *) e);
		break;
	case XCB_CREATE_NOTIFY:
		te("XCB_CREATE_NOTIFY: parent %#x, window %#x, "
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
		te("XCB_MAP_NOTIFY: event %#x, win %#x, redirect %u\n",
		   ((xcb_map_notify_event_t *) e)->event,
		   ((xcb_map_notify_event_t *) e)->window,
		   ((xcb_map_notify_event_t *) e)->override_redirect);
		break;
	case XCB_MAP_REQUEST:
		te("XCB_MAP_REQUEST: parent %#x, win %#x\n",
		   ((xcb_map_request_event_t *) e)->parent,
		   ((xcb_map_request_event_t *) e)->window);
		usleep(WIN_DELAY_US);
		add_window(((xcb_map_notify_event_t *) e)->window, 0);
		break;
	case XCB_PROPERTY_NOTIFY:
		te("XCB_PROPERTY_NOTIFY: win %#x, atom %d\n",
		   ((xcb_property_notify_event_t *) e)->window,
		   ((xcb_property_notify_event_t *) e)->atom);
		handle_property_notify((xcb_property_notify_event_t *) e);
		break;
	case XCB_UNMAP_NOTIFY:
		te("XCB_UNMAP_NOTIFY: event %#x, window %#x\n",
		   ((xcb_unmap_notify_event_t *) e)->event,
		   ((xcb_unmap_notify_event_t *) e)->window);
		handle_unmap_notify((xcb_unmap_notify_event_t *) e);
		break;
	case XCB_CLIENT_MESSAGE:
		te("XCB_CLIENT_MESSAGE: win %#x, type %d\n",
		   ((xcb_client_message_event_t *) e)->window,
		   ((xcb_client_message_event_t *) e)->type);
		handle_client_message((xcb_client_message_event_t *) e);
		break;
	case XCB_CONFIGURE_NOTIFY:
		te("XCB_CONFIGURE_NOTIFY: event %#x, window %#x, above %#x\n",
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
	char *env = getenv("FWM_SCALE");
	float font_scale;

	env ? (font_scale = atof(env)) : (font_scale = 1);
	space_width = ITEM_H_MARGIN * font_scale;
	ii("space width %u font scale %f\n", space_width, font_scale);

	font1 = XftFontOpen(xdpy, xscr, XFT_FAMILY, XftTypeString,
			    FONT1_NAME, XFT_SIZE, XftTypeDouble,
			    FONT1_SIZE * font_scale, NULL);
	if (!font1)
		panic("XftFontOpen(%s)\n", FONT1_NAME);

	text_yoffs = font1->ascent + ITEM_V_MARGIN;
	toolbox.xdiv = 2 * font_scale;

	font2 = XftFontOpen(xdpy, xscr, XFT_FAMILY, XftTypeString,
			    FONT2_NAME, XFT_SIZE, XftTypeDouble,
			    FONT2_SIZE * font_scale, NULL);
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
	char path[homelen + sizeof("/keys/") + UCHAR_MAX];
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

		tmp = strlen(kmap->actname);
		if (tmp > actname_max)
			actname_max = tmp;

		sprintf(path, "%s/keys/%s", homedir, kmap->keyname);

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

static void init_homedir(void)
{
	int mode = S_IRWXU;

	if (!(homedir = getenv("FWM_HOME")))
		homedir = ".";

	homelen = strlen(homedir);

	if (chdir(homedir) < 0)
		ee("chdir(%s) failed\n", homedir);

	if (mkdir(".session", mode) < 0 && errno != EEXIST)
		ee("mkdir(%s.session) failed\n", homedir);

	if (mkdir("screens", mode) < 0 && errno != EEXIST)
		ee("mkdir(%sscreens) failed\n", homedir);

	if (mkdir("panel", mode) < 0 && errno != EEXIST)
		ee("mkdir(%spanel) failed\n", homedir);

	if (mkdir("keys", mode) < 0 && errno != EEXIST)
		ee("mkdir(%skeys) failed\n", homedir);

	if (mkdir("colors", mode) < 0 && errno != EEXIST)
		ee("mkdir(%scolors) failed\n", homedir);
}

enum fdtypes {
	FD_SRV,
	FD_CTL,
	FD_MAX,
};

static int open_control(int fd)
{
	char path[MAX_PATH];

	snprintf(path, sizeof(path), "%s/.control:%u", homedir, disp);
	dd("open control %s\n", path);
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
	if (pfd->revents & POLLIN) {
		handle_user_request(pfd->fd);
		pfd->fd = open_control(pfd->fd); /* and reset pipe */
	}

	pfd->revents = 0;
}

static uint8_t getdisplay(void)
{
	char *str = getenv("DISPLAY");

	if (!str) {
		ee("DISPLAY variable is not set\n");
		return 0;
	}

	return atoi(str + 1); /* skip ':' */
}

int main()
{
	struct pollfd pfds[FD_MAX];
	const char *logfile;

	if (setsid() < 0) {
		ee("setsid failed\n");
		return 1;
	}

	close(STDIN_FILENO);
	logfile = getenv("FWM_LOG");

	if (logfile) {
		close(STDOUT_FILENO);
		close(STDERR_FILENO);

		if (!freopen(logfile, "a+", stdout)) {
			ee("failed to reopen %s as stdout\n", logfile);
		} else {
			if (!freopen(logfile, "a+", stderr)) {
				ee("failed to reopen %s as stderr\n", logfile);
			}
		}

		time_t t = time(NULL);
		ii("logfile %s, session %s", logfile, asctime(localtime(&t)));
	}

	init_homedir();

	if (signal(SIGCHLD, spawn_cleanup) == SIG_ERR)
		panic("SIGCHLD handler failed\n");

	disp = getdisplay();
	xdpy = XOpenDisplay(NULL);

	if (!xdpy) {
		ee("XOpenDisplay(%u) failed\n", disp);
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

	ii("root %#x, size %dx%d\n", rootscr->root, rootscr->width_in_pixels,
	   rootscr->height_in_pixels);

	list_init(&screens);
	list_init(&clients);

	init_keys_def();
	init_keys();
	init_rootwin();
	init_randr();
	init_outputs();
	xcb_flush(dpy);

	autostart();

	pfds[FD_SRV].fd = xcb_get_file_descriptor(dpy);
	pfds[FD_SRV].events = POLLIN;
	pfds[FD_SRV].revents = 0;

	pfds[FD_CTL].fd = open_control(-1);
	pfds[FD_CTL].events = POLLIN;
	pfds[FD_CTL].revents = 0;

	ii("defscr %d curscr %d display %u\n", defscr->id, curscr->id, disp);
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

		handle_server_event(&pfds[FD_SRV]);
		handle_control_event(&pfds[FD_CTL]);

		if (logfile) {
			fflush(stdout);
			fflush(stderr);
		}

		/* FIXME: since ICCCM is not supported by intention this HACK
		 * allows to discover clients that were created by group leaders;
		 * for some reason xcb does not catch such events explicitly */

		if (rescan_) {
			scan_clients(1);
			rescan_ = 0;
		}
	}

	xcb_set_input_focus(dpy, XCB_NONE, XCB_INPUT_FOCUS_POINTER_ROOT,
			    XCB_CURRENT_TIME);
	xcb_flush(dpy);

	clean();
	return 0;
}
