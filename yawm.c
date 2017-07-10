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
#define TAG_GAP 2

#define BORDER_WIDTH 1
#define WINDOW_PAD 0

#define WIN_WIDTH_MIN 2
#define WIN_HEIGHT_MIN 2

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

#define SCR_FLG_SWITCH_WINDOW (1 << 1)
#define SCR_FLG_SWITCH_WINDOW_NOWARP (1 << 2)

#define TAG_NAME_MAX 32

#define MOUSE_BTN_LEFT 1
#define MOUSE_BTN_MID 2
#define MOUSE_BTN_RIGHT 3

#define DIV_ICON "::"

#define MENU_ICON1 "="
#define MENU_ICON2 ""
//#define MENU_ICON2 " "

#define ALT XCB_MOD_MASK_1
#ifndef MOD
#define MOD XCB_MOD_MASK_1
#endif
#define SHIFT XCB_MOD_MASK_SHIFT
#define CTRL XCB_MOD_MASK_CONTROL

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
	uint8_t pad; /* text padding */
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
//#define BTN_LEFT "  "
//#define BTN_CENTER "  "
//#define BTN_RIGHT "  "
#define BTN_LEFT ""
#define BTN_RIGHT ""
#define BTN_TOP ""
#define BTN_BOTTOM ""
#define BTN_CENTER ""
#define BTN_EXPAND ""
#define BTN_RETAG ""
//#define BTN_PIN ""
#define BTN_FLAG ""

#define TOOL_FLG_LOCK (1 << 0)

struct toolbar_item {
	const char *str;
	uint8_t len;
	uint16_t x;
	uint16_t w;
	uint8_t flags;
};

static struct toolbar_item toolbar_items[] = {
	{ BTN_MOUSE, slen(BTN_MOUSE), },
	{ BTN_MOVE, slen(BTN_MOVE), },
	{ BTN_CENTER, slen(BTN_CENTER), },
	{ BTN_LEFT, slen(BTN_LEFT), },
	{ BTN_RIGHT, slen(BTN_RIGHT), },
	{ BTN_TOP, slen(BTN_TOP), },
	{ BTN_BOTTOM, slen(BTN_BOTTOM), },
	{ BTN_EXPAND, slen(BTN_EXPAND), },
	{ BTN_RETAG, slen(BTN_RETAG), },
	{ BTN_FLAG, slen(BTN_FLAG), },
	{ BTN_CLOSE, slen(BTN_CLOSE), },
};

struct toolbar {
	int16_t x;
	int16_t y;
	struct panel panel;
	struct screen *scr;
	struct client *cli;
	xcb_keycode_t knext; /* item's navigation */
	xcb_keycode_t kprev;
	xcb_keycode_t kclose; /* hide toolbar */
	uint8_t iprev; /* previous item index */
	uint8_t ithis; /* current item index */
};

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
struct tag *target_tag; /* target tag for interactive window re-tagging */
struct screen *target_scr; /* target screen for interactive window re-tagging */
struct screen *curscr;
struct screen *defscr;
static xcb_screen_t *rootscr; /* root window details */
static struct toolbar toolbar; /* window toolbar */
static uint8_t focus_root_;

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
	struct client *anchor;
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
	int16_t prev_x, prev_y;
	uint16_t prev_w, prev_h;
	uint8_t div; /* for position calculation */
	xcb_window_t win;
	pid_t pid;
	struct screen *scr;
	struct tag *tag;
	uint16_t flags;
	uint32_t crc; /* based on class name */
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
	{ MOD, XK_Tab, 0, "mod_tab", "_next_window",
	  next_window, },
	{ MOD, XK_BackSpace, 0, "mod_backspace", "_prev_window",
	  prev_window, },
	{ MOD, XK_Return, 0, "mod_return", "_raise_client",
	  raise_client, },
	{ MOD, XK_Home, 0, "mod_home", "_retag_next",
	  retag_client, DIR_NEXT, },
	{ MOD, XK_End, 0, "mod_end", "_retag_prev",
	  retag_client, DIR_PREV, },
	{ MOD, XK_Page_Up, 0, "mod_pageup", "_tag_next",
	  walk_tags, DIR_NEXT, },
	{ MOD, XK_Page_Down, 0, "mod_pagedown", "_tag_prev",
	  walk_tags, DIR_PREV, },
	{ SHIFT, XK_F5, 0, "shift_f5", "_top_left",
	  place_window, WIN_POS_TOP_LEFT, },
	{ SHIFT, XK_F6, 0, "shift_f6", "_top_right",
	  place_window, WIN_POS_TOP_RIGHT, },
	{ SHIFT, XK_F7, 0, "shift_f7", "_bottom_left",
	  place_window, WIN_POS_BOTTOM_LEFT, },
	{ SHIFT, XK_F8, 0, "shift_f8", "_bottom_right",
	  place_window, WIN_POS_BOTTOM_RIGHT, },
	{ SHIFT, XK_F10, 0, "shift_f10", "_center",
	  place_window, WIN_POS_CENTER, },
	{ MOD, XK_F5, 0, "mod_f5", "_left_fill",
	  place_window, WIN_POS_LEFT_FILL, },
	{ MOD, XK_F6, 0, "mod_f6", "_right_fill",
	  place_window, WIN_POS_RIGHT_FILL, },
	{ MOD, XK_F7, 0, "mod_f7", "_top_fill",
	  place_window, WIN_POS_TOP_FILL, },
	{ MOD, XK_F8, 0, "mod_f8", "_bottom_fill",
	  place_window, WIN_POS_BOTTOM_FILL, },
	{ MOD, XK_F9, 0, "mod_f9", "_full_screen",
	  place_window, WIN_POS_FILL, },
	{ MOD, XK_F3, 0, "mod_f3", "_make_grid",
	  make_grid, },
	{ MOD, XK_F4, 0, "mod_f4", "_toggle_toolbar",
	  toggle_toolbar, },
	{ MOD, XK_F2, 0, "mod_f2", "_flag_window",
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

static uint32_t border_docked;
static uint32_t border_normal;
static uint32_t border_active;
static XftColor textfg_normal;
static XftColor textfg_active;
static uint32_t textbg_normal;
static uint32_t textbg_active;
static uint32_t panelbg;
static XftColor titlefg;
static XftColor selectfg;
static uint32_t selectbg;
static XftColor alertfg;
static uint32_t alertbg;
static XftColor focusfg;
static XftColor toolfg;
static uint32_t focusbg;

enum colortype {
	COLOR_TYPE_INT,
	COLOR_TYPE_XFT,
};

enum coloridx {
	BORDER_DOCKED,
	BORDER_NORMAL,
	BORDER_ACTIVE,
	TEXTFG_NORMAL,
	TEXTFG_ACTIVE,
	TEXTBG_NORMAL,
	TEXTBG_ACTIVE,
	PANELBG,
	TITLEFG,
	SELECTFG,
	SELECTBG,
	TOOLFG,
	ALERTFG,
	ALERTBG,
	FOCUSFG,
	FOCUSBG,
};

static struct color defcolors[] = {
	{ "border_docked", &border_docked, 0x303030, COLOR_TYPE_INT, },
	{ "border_normal", &border_normal, 0x404040, COLOR_TYPE_INT, },
	{ "border_active", &border_active, 0x84aad2, COLOR_TYPE_INT, },
	{ "textfg_normal", &textfg_normal, 0xa0a0a0, COLOR_TYPE_XFT, },
	{ "textfg_active", &textfg_active, 0xc0c0c0, COLOR_TYPE_XFT, },
	{ "textbg_normal", &textbg_normal, 0x202020, COLOR_TYPE_INT, },
	{ "textbg_active", &textbg_active, 0x101010, COLOR_TYPE_INT, },
	{ "panelbg", &panelbg, 0x202020, COLOR_TYPE_INT, },
	{ "titlefg", &titlefg, 0xc0c0c0, COLOR_TYPE_XFT, },
	{ "selectfg", &selectfg, 0x000000, COLOR_TYPE_XFT, },
	{ "selectbg", &selectbg, 0x84aad2, COLOR_TYPE_INT, },
	{ "toolfg", &toolfg, 0x505050, COLOR_TYPE_XFT, },
	{ "alertfg", &alertfg, 0xc32d2d, COLOR_TYPE_XFT, },
	{ "alertbg", &alertbg, 0x90ae2b, COLOR_TYPE_INT, },
	{ "focusfg", &focusfg, 0x90abba, COLOR_TYPE_XFT, },
	{ "focusbg", &focusbg, 0x404040, COLOR_TYPE_INT, },
	{ NULL, NULL, 0, 0, },
};

#define color2int(idx) *((uint32_t *) defcolors[idx].val)
#define color2xft(idx) *((XftColor *) defcolors[idx].val)
#define color2ptr(idx) &defcolors[idx]

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

	if (ext.width % 2)
		*w = ext.width + 1;
	else
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
			    const char *text, int len, XftFont *font)
{
	fill_rect(panel->win, panel->gc, bg, x, ITEM_V_MARGIN, w + TAG_GAP,
		  panel_height - 2 * ITEM_V_MARGIN);

	x += panel->pad;

	XftDrawStringUtf8(panel->draw, fg->val, font, x, text_yoffs,
			  (XftChar8 *) text, len);
	XSync(xdpy, 0);
}

static void print_title(struct screen *scr, xcb_window_t win)
{
	struct sprop title;
	uint16_t w, h;

	/* clean area */
	fill_rect(scr->panel.win, scr->panel.gc, color2ptr(PANELBG),
		  scr->items[PANEL_AREA_TITLE].x, 0,
		  scr->items[PANEL_AREA_DOCK].x, panel_height);

	if (win == XCB_WINDOW_NONE) {
		draw_panel_text(&scr->panel, color2ptr(TEXTFG_NORMAL),
				color2ptr(PANELBG),
				scr->items[PANEL_AREA_TITLE].x,
				scr->items[PANEL_AREA_TITLE].w, NULL, 0,
				font1);
		return;
	}

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

	draw_panel_text(&scr->panel, color2ptr(TITLEFG),
			color2ptr(PANELBG),
			scr->items[PANEL_AREA_TITLE].x,
			scr->items[PANEL_AREA_TITLE].w,
			title.str, title.len, font1);
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
	} else if (cli->flags & CLI_FLG_POPUP) {
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
		    scr->y <= y && y <= (scr->y + scr->h)) {
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

static struct screen *pointer2scr(void)
{
	xcb_query_pointer_cookie_t c;
	xcb_query_pointer_reply_t *r;

	c = xcb_query_pointer(dpy, rootscr->root);
        r = xcb_query_pointer_reply(dpy, c, NULL);
        if (!r) {
		curscr = list2screen(screens.prev);
	} else {
		curscr = coord2scr(r->root_x, r->root_y);
		free(r);
	}

        return curscr;
}

static xcb_window_t pointer2win(void)
{
	xcb_window_t win;
	xcb_query_pointer_cookie_t c;
	xcb_query_pointer_reply_t *r;

	c = xcb_query_pointer(dpy, rootscr->root);
        r = xcb_query_pointer_reply(dpy, c, NULL);
        if (!r)
		return XCB_WINDOW_NONE;

	curscr = coord2scr(r->root_x, r->root_y);
	win = r->child;
	free(r);
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
		window_border_color(win, alertbg);
	else
		window_border_color(win, border_normal);

	xcb_window_t tmp = XCB_WINDOW_NONE;
	xcb_change_property(dpy, XCB_PROP_MODE_REPLACE, rootscr->root,
			    a_active_win, XCB_ATOM_WINDOW, 32, 1, &tmp);
}

static void focus_window(xcb_window_t win)
{
	if (win == toolbar.panel.win)
		return;
	else if (toolbar.cli && toolbar.cli->win == win)
		window_border_color(win, alertbg);
	else
		window_border_color(win, border_active);

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

static int focus_any(xcb_window_t del)
{
	struct client *cli;

	curscr = pointer2scr();

	if (focus_root_) {
		focus_root_ = 0;
		focus_root();
		return 0;
	}

	if (!(cli = front_client(curscr->tag)))
		cli = pointer2cli();

	if (cli) {
		if (window_status(cli->win) != WIN_STATUS_VISIBLE) {
			ww("invisible front win 0x%x\n", cli->win);
			cli = NULL;
		} else if (cli->win == del) {
			return -1; /* deleted window is not gone, try again */
		} else {
			ii("front win 0x%x\n", cli ? cli->win : 0);
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

	return 0;
}

static void close_client(struct client *cli)
{
	ii("terminate pid %d win 0x%x\n", cli->pid, cli->win);

	if (cli->pid)
		kill(cli->pid, SIGTERM);

	free_client(cli);
}

static void close_window(xcb_window_t win)
{
	struct client *cli = win2cli(win);

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

	/* make sure that window is really gone */

	if (focus_any(win) < 0) {
		xcb_kill_client_checked(dpy, win);
		xcb_flush(dpy);
	}

	if (focus_any(win) < 0) { /* still open - terminate client */
		if (cli) {
			close_client(cli);
			return;
		}
	}

	if (cli)
		free_client(cli);
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

static void draw_toolbar_text(struct toolbar_item *item, struct color *fg)
{
	struct color *bg = color2ptr(PANELBG);

	draw_panel_text(&toolbar.panel, fg, bg, item->x, item->w,
			item->str, item->len, font2);
}

static struct toolbar_item *focused_item;

static void focus_toolbar_item(int16_t x, int16_t y, uint8_t left)
{
	struct color *fg;
	struct toolbar_item *ptr = toolbar_items;
	struct toolbar_item *end = toolbar_items + ARRAY_SIZE(toolbar_items);
	uint16_t xx = 0;

	while (ptr < end) {
		xx = ptr->x + ptr->w + 2 * toolbar.panel.pad;

		if (x < ptr->x || x > xx || y > panel_height) {
			ptr++;
			continue;
		}

		if (focused_item == ptr) {
			ptr++;
			continue;
		}

		/* reset keyboard focus */
		struct toolbar_item *tmp = &toolbar_items[toolbar.ithis];
		if (tmp->flags & TOOL_FLG_LOCK)
			fg = color2ptr(FOCUSFG);
		else
			fg = color2ptr(TOOLFG);

		draw_toolbar_text(tmp, fg);

		ii("pointer at %d, select item '%s' [%u,%u]\n", x, ptr->str,
		    ptr->x, ptr->w);

		if (focused_item && focused_item->flags & TOOL_FLG_LOCK)
			draw_toolbar_text(focused_item, color2ptr(FOCUSFG));
		else if (focused_item)
			draw_toolbar_text(focused_item, color2ptr(TOOLFG));

		if (ptr->str == (const char *) BTN_CLOSE)
			fg = color2ptr(ALERTFG);
		else
			fg = color2ptr(FOCUSFG);

		draw_toolbar_text(ptr, fg);

		focused_item = ptr++;
	}
}

static void draw_toolbar(void)
{
	struct toolbar_item *ptr = toolbar_items;
	struct toolbar_item *end = toolbar_items + ARRAY_SIZE(toolbar_items);
	uint8_t locked = !!(toolbar.cli == curscr->tag->anchor);

	focused_item = NULL;

	while (ptr < end) {
		if (ptr->str == (const char *) BTN_FLAG && locked)
			draw_toolbar_text(ptr, color2ptr(FOCUSFG));
		else
			draw_toolbar_text(ptr, color2ptr(TOOLFG));

		ptr++;
	}

	warp_pointer(toolbar.panel.win, 5 * toolbar.panel.pad, panel_height / 2);
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

static void disable_events(xcb_window_t win)
{
	uint32_t val = 0;
	xcb_change_window_attributes_checked(dpy, win, XCB_CW_EVENT_MASK, &val);
	xcb_flush(dpy);
}

static void clean_toolbar(void)
{
	/* no memset becuase of key mapping is done at start up */
	toolbar.cli = NULL;
	toolbar.scr = NULL;
	toolbar.panel.win = XCB_WINDOW_NONE;
}

static void hide_toolbar(void)
{
	xcb_ungrab_pointer(dpy, XCB_CURRENT_TIME);

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

	xcb_flush(dpy);
	clean_toolbar();
}

static uint16_t toolbar_width;

static int setup_toolbar(void)
{
	uint32_t val[2], mask;

	clean_toolbar();

	if (list_empty(&curscr->tag->clients)) {
		ww("nothing to show on empty tag\n");
		return -1;
	}

	toolbar.cli = front_client(curscr->tag);

	if (!toolbar.cli) {
		ww("no front client on scr %u tag '%s'\n", curscr->id,
		   curscr->tag->name);
		return -1;
	}

	toolbar.iprev = 0;
	toolbar.ithis = 0;
	toolbar.scr = curscr;
	toolbar.x = curscr->items[PANEL_AREA_TITLE].x;
	toolbar.y = curscr->y;
	toolbar.panel.w = toolbar_width;
	toolbar.panel.h = panel_height;
	toolbar.panel.win = xcb_generate_id(dpy);

	mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	val[0] = color2int(PANELBG);
	val[1] = XCB_EVENT_MASK_VISIBILITY_CHANGE;
	val[1] |= XCB_EVENT_MASK_PROPERTY_CHANGE;
	val[1] |= XCB_EVENT_MASK_STRUCTURE_NOTIFY;
	val[1] |= XCB_EVENT_MASK_ENTER_WINDOW;
	val[1] |= XCB_EVENT_MASK_LEAVE_WINDOW;
	val[1] |= XCB_EVENT_MASK_BUTTON_PRESS;
	val[1] |= XCB_EVENT_MASK_KEY_PRESS;
	val[1] |= XCB_EVENT_MASK_POINTER_MOTION;

	window_border_color(toolbar.cli->win, alertbg);
	xcb_create_window(dpy, XCB_COPY_FROM_PARENT, toolbar.panel.win,
			  rootscr->root,
			  0, 0, toolbar.panel.w, toolbar.panel.h,
			  0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  rootscr->root_visual, mask, val);
	xcb_flush(dpy);

	toolbar.panel.gc = xcb_generate_id(dpy);
	mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND;
	val[0] = val[1] = color2int(PANELBG);
        xcb_create_gc(dpy, toolbar.panel.gc, toolbar.panel.win, mask, val);

	toolbar.panel.draw = XftDrawCreate(xdpy, toolbar.panel.win,
					   DefaultVisual(xdpy, xscr),
					   DefaultColormap(xdpy, xscr));

	ii("toolbar 0x%x geo %ux%u+%d+%d\n", toolbar.panel.win,
	   toolbar.panel.w, toolbar.panel.h, toolbar.x, toolbar.y);
	return 0;
}

static void show_toolbar(void)
{
	uint32_t val[4];
	uint32_t mask;

	if (toolbar.cli) {
		hide_toolbar();
		return;
	}

	if (setup_toolbar() < 0)
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
	raise_window(toolbar.panel.win);
	focus_window(toolbar.panel.win);
	draw_toolbar();
	xcb_flush(dpy);
}

static void toggle_toolbar(unused(void *ptr))
{
	show_toolbar();
}

static void init_toolbar(void)
{
	struct toolbar_item *ptr = toolbar_items;
	struct toolbar_item *end = toolbar_items + ARRAY_SIZE(toolbar_items);
	uint16_t h;

	toolbar_width = FONT2_SIZE / 2;

	while (ptr < end) {
		text_exts(ptr->str, ptr->len, &ptr->w, &h, font2);
		ptr->x = toolbar_width;
		toolbar_width += ptr->w + FONT2_SIZE / 2;
		ptr++;
	}

	ii("toolbar width %u\n", toolbar_width);
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

static void make_grid(void *ptr)
{
	struct arg *arg = (struct arg *) ptr;
	struct list_head *cur;
	uint16_t i, n;
	uint16_t cw, ch; /* cell size */
	int16_t x, y;

	curscr = coord2scr(arg->x, arg->y);

	if (!curscr->tag->anchor) {
		curscr->tag->space.x = curscr->x;
		curscr->tag->space.y = curscr->top;
		curscr->tag->space.w = curscr->w;
		curscr->tag->space.h = curscr->h;
	}

	n = 0;
	list_walk(cur, &curscr->tag->clients) {
		struct client *cli = list2client(cur);
		if (curscr->tag->anchor == cli)
			continue;
		else if (window_status(cli->win) == WIN_STATUS_VISIBLE)
			n++;
	}

	cw = ch = GRIDCELL_MIN_WIDTH;
	for (i = 1; i < curscr->tag->space.w / GRIDCELL_MIN_WIDTH; i++) {
		if (i * i >= n) {
			cw = curscr->tag->space.w / i;
			ch = curscr->tag->space.h / i;
			goto done;
		} else if (i * (i + 1) >= n) {
			cw = curscr->tag->space.w / (i + 1);
			ch = curscr->tag->space.h / i - BORDER_WIDTH;
			goto done;
		}
	}
	ww("failed to calculate cell size for screen %d\n", curscr->id);
	return;
done:
	ii("%d cells size of (%u,%u)\n", n, cw, ch);
	x = y = 0;
	list_walk(cur, &curscr->tag->clients) {
		struct client *cli = list2client(cur);
		if (curscr->tag->anchor == cli)
			continue;
		if (window_status(cli->win) != WIN_STATUS_VISIBLE)
			continue;
		client_moveresize(cli, curscr->tag->space.x + x,
				  curscr->tag->space.y + y,
				  cw - BORDER_WIDTH - WINDOW_PAD,
				  ch - BORDER_WIDTH - WINDOW_PAD);
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
	struct color *fg;
	struct arg *arg = (struct arg *) ptr;
	struct toolbar_item *cur = toolbar_items;
	struct toolbar_item *end = toolbar_items + ARRAY_SIZE(toolbar_items);
	struct client *cli;

	curscr = coord2scr(arg->x, arg->y);
	cli = curscr->tag->anchor;

	if (curscr->tag->anchor)
		curscr->tag->anchor = NULL;
	else
		curscr->tag->anchor = pointer2cli();

	if (curscr->tag->anchor)
		window_border_color(curscr->tag->anchor->win, alertbg);
	else if (cli)
		window_border_color(cli->win, border_active);

	xcb_flush(dpy);

	if (!toolbar.cli)
		return;

	while (cur < end) {
		if (cur->str == (const char *) BTN_FLAG) {
			if (curscr->tag->anchor) {
				cur->flags |= TOOL_FLG_LOCK;
				fg = color2ptr(FOCUSFG);
			} else {
				cur->flags &= ~TOOL_FLG_LOCK;
				fg = color2ptr(TOOLFG);
			}

			draw_toolbar_text(cur, fg);
			break;
		}
		cur++;
	}
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
		if (cli->flags & CLI_FLG_FULLSCREEN) {
			cli->flags &= ~CLI_FLG_FULLSCREEN;
			x = cli->prev_x;
			y = cli->prev_y;
			w = cli->prev_w;
			h = cli->prev_h;
			goto out;
		}

		cli->prev_x = cli->x;
		cli->prev_y = cli->y;
		cli->prev_w = cli->w;
		cli->prev_h = cli->h;
		cli->flags |= CLI_FLG_FULLSCREEN;

		x = curscr->x;
		y = curscr->top;
		w = curscr->w - 2 * BORDER_WIDTH;
		h = curscr->h - 2 * BORDER_WIDTH;
		curscr->tag->anchor = NULL;
		break;
	case WIN_POS_CENTER:
		if (cli->flags & CLI_FLG_CENTER) {
			cli->flags &= ~CLI_FLG_CENTER;
			x = cli->prev_x;
			y = cli->prev_y;
			w = cli->prev_w;
			h = cli->prev_h;
			goto out;
		}

		cli->prev_x = cli->x;
		cli->prev_y = cli->y;
		cli->prev_w = cli->w;
		cli->prev_h = cli->h;
		cli->flags |= CLI_FLG_CENTER;

		x = curscr->x + curscr->w / 2 - curscr->w / 4;
		y = curscr->top + curscr->h / 2 - curscr->h / 4;
		goto halfwh;
	case WIN_POS_LEFT_FILL:
		tt("WIN_POS_LEFT_FILL\n");

		if (last_winpos != pos || ++cli->div > POS_DIV_MAX)
			cli->div = 2;

		x = curscr->x;
		y = curscr->top;
		goto halfw;
	case WIN_POS_RIGHT_FILL:
		tt("WIN_POS_RIGHT_FILL\n");

		if (last_winpos != pos || ++cli->div > POS_DIV_MAX)
			cli->div = 2;

		x = curscr->x + curscr->w - curscr->w / cli->div;
		y = curscr->x;
		goto halfw;
	case WIN_POS_TOP_FILL:
		tt("WIN_POS_TOP_FILL\n");

		if (last_winpos != pos || ++cli->div > POS_DIV_MAX)
			cli->div = 2;

		x = curscr->x;
		y = curscr->top;
		goto halfh;
	case WIN_POS_BOTTOM_FILL:
		tt("WIN_POS_BOTTOM_FILL\n");

		if (last_winpos != pos || ++cli->div > POS_DIV_MAX)
			cli->div = 2;

		x = curscr->x;
		y = curscr->top + curscr->h - curscr->h / cli->div;
		goto halfh;
	case WIN_POS_TOP_LEFT:
		tt("WIN_POS_TOP_LEFT\n");
		x = curscr->x;
		y = curscr->top;
		goto halfwh;
	case WIN_POS_TOP_RIGHT:
		tt("WIN_POS_TOP_RIGHT\n");
		x = curscr->x + curscr->w / 2;
		y = curscr->top;
		goto halfwh;
	case WIN_POS_BOTTOM_LEFT:
		tt("WIN_POS_BOTTOM_LEFT\n");
		x = curscr->x;
		y = curscr->top + curscr->h / 2;
		goto halfwh;
	case WIN_POS_BOTTOM_RIGHT:
		tt("WIN_POS_BOTTOM_RIGHT\n");
		x = curscr->x + curscr->w / 2;
		y = curscr->top + curscr->h / 2;
		goto halfwh;
	default:
		return;
	}

out:
	last_winpos = pos;
	resort_client(cli);
	raise_window(cli->win);
	focus_window(cli->win);

	if (cli->div == POS_DIV_MAX)
		window_border_color(cli->win, alertbg);

	client_moveresize(cli, x, y, w, h);

	if (cli != toolbar.cli)
		center_pointer(cli);

	xcb_flush(dpy);
	return;
halfwh:
	w = curscr->w / 2 - 2 * BORDER_WIDTH - WINDOW_PAD;
	h = curscr->h / 2 - 2 * BORDER_WIDTH - WINDOW_PAD;

	if (curscr->h % 2)
		h++;

	goto out;
halfw:
	w = curscr->w / cli->div - 2 * BORDER_WIDTH;
	h = curscr->h - 2 * BORDER_WIDTH;

	if (curscr->tag->anchor == cli) {
		if (pos == WIN_POS_LEFT_FILL)
			curscr->tag->space.x = curscr->x + w + 2 * BORDER_WIDTH;
		else
			curscr->tag->space.x = curscr->x;
		curscr->tag->space.y = curscr->top;
		curscr->tag->space.w = curscr->w - curscr->w / cli->div;
		curscr->tag->space.h = curscr->h;
		struct arg arg = { .x = curscr->x, .y = curscr->top, };
		make_grid(&arg);
	}

	goto out;
halfh:
	w = curscr->w - 2 * BORDER_WIDTH;
	h = curscr->h / cli->div - 2 * BORDER_WIDTH - WINDOW_PAD;

	if (cli->div == 2 && curscr->h % cli->div)
		h++;

	if (curscr->tag->anchor == cli) {
		curscr->tag->space.x = curscr->x;

		if (pos == WIN_POS_TOP_FILL)
			curscr->tag->space.y = curscr->top + h + 2 * BORDER_WIDTH;
		else
			curscr->tag->space.y = curscr->top;

		curscr->tag->space.w = curscr->w;
		curscr->tag->space.h = curscr->h - curscr->h / cli->div;
		struct arg arg = { .x = curscr->x, .y = curscr->top, };
		make_grid(&arg);
	}

	goto out;
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

static void print_tag(struct screen *scr, struct tag *tag, struct color *fg)
{
	struct color *bg;

	if (fg == color2ptr(TEXTFG_ACTIVE))
		bg = color2ptr(TEXTBG_ACTIVE);
	else
		bg = color2ptr(TEXTBG_NORMAL);

	draw_panel_text(&scr->panel, fg, bg, tag->x, tag->w, tag->name,
			tag->nlen, font1);
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

	if (focus && (cli = front_client(tag))) {
		if (window_status(cli->win) != WIN_STATUS_VISIBLE) {
			focus_root();
		} else {
			raise_window(cli->win);
			focus_window(cli->win);
			center_pointer(cli);
		}
	}
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
	hide_toolbar();

	if (scr->tag) {
		print_tag(scr, scr->tag, color2ptr(TEXTFG_NORMAL));
		hide_windows(scr->tag);
	}

	scr->tag = tag;
	print_tag(scr, scr->tag, color2ptr(TEXTFG_ACTIVE));

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

static void retag_window(struct arg *arg)
{
	struct client *cli = toolbar.cli;

	if (!cli) {
		cli = win2cli((xcb_window_t) arg->data);
		if (!cli)
			return;
	}

	curscr = coord2scr(arg->x, arg->y);
	switch_window(curscr, DIR_NEXT); /* focus window */
	list_del(&cli->head);
	list_add(&target_tag->clients, &cli->head); /* re-tag window */

	if (list_empty(&cli->tag->clients)) {
		xcb_set_input_focus(dpy, XCB_NONE, XCB_INPUT_FOCUS_POINTER_ROOT,
				    XCB_CURRENT_TIME);
	}

	cli->scr = target_scr;
	cli->tag = target_tag;

	if (cli->scr != curscr) {
		client_moveresize(cli, cli->scr->x, cli->scr->top, cli->w,
				  cli->h);
	}

	window_state(cli->win, XCB_ICCCM_WM_STATE_ICONIC);
	xcb_unmap_window_checked(dpy, cli->win);
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

static void calc_title_width(struct screen *scr)
{
	int16_t end = scr->items[PANEL_AREA_DOCK].x;
	uint16_t x, h, i;
	char *tmp;

	i = 1;
	x = 0;
	while (x + scr->items[PANEL_AREA_TITLE].x < end) {
		tmp = calloc(1, i + 1);
		memset(tmp, 'w', i);
		text_exts(tmp, strlen(tmp), &x, &h, font1);
		free(tmp);
		i++;
	}

	scr->items[PANEL_AREA_TITLE].w = x - scr->panel.pad;
	print_title(scr, XCB_WINDOW_NONE);
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
		x -= (cli->w + scr->panel.pad + 2 * BORDER_WIDTH);
		client_moveresize(cli, x, y, cli->w, cli->h);
	}

	scr->items[PANEL_AREA_DOCK].x = x;
	scr->items[PANEL_AREA_DOCK].w = scr->items[PANEL_AREA_DOCK].x - x;
	calc_title_width(scr);
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
		window_border_color(cli->win, border_docked);
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
	focus_any(XCB_WINDOW_NONE);

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
	xcb_get_geometry_reply_t *g;
	xcb_get_window_attributes_cookie_t c;
	xcb_get_window_attributes_reply_t *a;
	enum winstatus status = window_status(win);

	if (status == WIN_STATUS_UNKNOWN) {
		ww("destroy unknown window 0x%x\n", win);
		xcb_unmap_window_checked(dpy, win);
		xcb_destroy_window_checked(dpy, win);
		xcb_flush(dpy);
		return NULL;
	}

	if (win == rootscr->root || win == toolbar.panel.win)
		return NULL;

	flags = 0;

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

	if (g->width <= 1 || g->height <= 1) {
		/* FIXME: current assumption is that window is not supposed
		 * to be shown
		 */
		ww("ignore tiny window 0x%x geo %ux%u%+d%+d\n", win, g->width,
		   g->height, g->x, g->y);
		return NULL;
	}

	scr = NULL;
	tag = NULL;

	if (scan && !(flags & (CLI_FLG_TRAY | CLI_FLG_DOCK)))
		restore_window(win, &scr, &tag);

	if (!scr && g->x == 0 && g->y == 0) {
		scr = pointer2scr();
	} else if (!scr) {
		/* preserve current location of already existed windows */
		scr = coord2scr(g->x, g->y);
		if (!scr)
			scr = pointer2scr();
	}

	if (!scr) {
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

	if (font2) {
		draw_panel_text(&scr->panel, color2ptr(TEXTFG_NORMAL),
				color2ptr(PANELBG), x, w, MENU_ICON2,
				sizeof(MENU_ICON2) - 1, font2);
	} else {
		draw_panel_text(&scr->panel, color2ptr(TEXTFG_NORMAL),
				color2ptr(PANELBG), x, w, MENU_ICON1,
				sizeof(MENU_ICON1) - 1, font1);
	}
}

static void print_div(struct screen *scr)
{
	uint16_t x, w;

	x = scr->items[PANEL_AREA_DIV].x;
	w = scr->items[PANEL_AREA_DIV].w;

	draw_panel_text(&scr->panel, color2ptr(TEXTFG_NORMAL),
			color2ptr(PANELBG), x, w, DIV_ICON,
			sizeof(DIV_ICON) - 1, font1);
}

static int tag_pointed(struct tag *tag, int16_t x, int16_t y)
{
	if (0 <= y && y <= curscr->panel.y + panel_height &&
	    x >= tag->x && x <= tag->x + tag->w + TAG_GAP) {
		return 1;
	}

	return 0;
}

static void point_tag(struct screen *scr, int16_t x, int16_t y)
{
	struct list_head *cur;

	list_walk(cur, &scr->tags) {
		struct tag *tag = list2tag(cur);

		if (tag == scr->tag) {
			continue;
		} else if (tag_pointed(tag, x, y)) {
			print_tag(scr, tag, color2ptr(FOCUSFG));
			break;
		}
	}
}

static void select_tag(struct screen *scr, int16_t x, int16_t y)
{
	struct list_head *cur;
	struct tag *prev;

	hide_toolbar();

	if (target_tag) { /* un-mark tag if previously marked */
		print_tag(target_scr, target_tag, color2ptr(TEXTFG_NORMAL));
		target_tag = NULL;
		target_scr = NULL;
	}

	if (scr->tag && tag_pointed(scr->tag, x, y)) {
		return;
	} else if (scr->tag) { /* deselect current tag instantly */
		print_tag(scr, scr->tag, color2ptr(TEXTFG_NORMAL));
	}

	prev = scr->tag;

	list_walk(cur, &scr->tags) { /* refresh labels */
		struct tag *tag = list2tag(cur);

		if (tag == scr->tag) {
			continue;
		} else if (!tag_pointed(tag, x, y)) {
			print_tag(scr, tag, color2ptr(TEXTFG_NORMAL));
		} else {
			print_tag(scr, tag, color2ptr(TEXTFG_ACTIVE));
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
				  color2ptr(PANELBG),
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

	return tag;
}

static int tag_add(struct screen *scr, const char *name, uint8_t id,
		   uint16_t pos)
{
	struct color *fg;
	struct tag *tag;
	uint16_t h;

	tag = tag_get(scr, name, id);
	if (!tag)
		return 0;

	text_exts(name, tag->nlen, &tag->w, &h, font1);
	tag->w += scr->panel.pad * 2;
	ii("tag '%s' len %u width %u\n", name, tag->nlen, tag->w);

	if (pos != scr->items[PANEL_AREA_TAGS].x) {
		fg = color2ptr(TEXTFG_NORMAL);
	} else {
		fg = color2ptr(TEXTFG_ACTIVE);
		scr->tag = tag;
	}

	tag->x = pos;
	print_tag(scr, tag, fg);

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
	struct color *fg;
	struct list_head *cur;

	/* clean panel */
	fill_rect(scr->panel.win, scr->panel.gc, color2ptr(PANELBG), scr->x, 0,
		  scr->w, panel_height);

	print_menu(scr);

	list_walk(cur, &scr->tags) {
		struct tag *tag = list2tag(cur);

		if (scr->tag == tag)
			fg = color2ptr(TEXTFG_ACTIVE);
		else
			fg = color2ptr(TEXTFG_NORMAL);

		print_tag(scr, tag, fg);
	}

	print_div(scr);
	print_title(scr, XCB_WINDOW_NONE);
}

static void update_panel_items(struct screen *scr)
{
	int16_t x = 0;
	uint16_t h, w;

	/* clean panel */
	fill_rect(scr->panel.win, scr->panel.gc, color2ptr(PANELBG), scr->x, 0,
		  scr->w, panel_height);

	if (font2)
		text_exts(MENU_ICON2, sizeof(MENU_ICON2) - 1, &w, &h, font2);
	else
		text_exts(MENU_ICON1, sizeof(MENU_ICON1) - 1, &w, &h, font1);

	scr->items[PANEL_AREA_MENU].x = TAG_GAP;
	scr->items[PANEL_AREA_MENU].w = w + 2 * scr->panel.pad;
	x += scr->items[PANEL_AREA_MENU].w + 2 * TAG_GAP;
	print_menu(scr);

	scr->items[PANEL_AREA_TAGS].x = x;
	scr->items[PANEL_AREA_TAGS].w = init_tags(scr);

	scr->items[PANEL_AREA_DIV].x = scr->items[PANEL_AREA_TAGS].w;
	text_exts(DIV_ICON, sizeof(DIV_ICON) - 1, &w, &h, font1);
	w += scr->items[PANEL_AREA_DIV].x + 2 * scr->panel.pad;
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

		print_tag(scr, scr->tag, color2ptr(TEXTFG_NORMAL));
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

	key = xcb_key_symbols_get_keycode(syms, XK_Down);
	if (!key) {
		ee("xcb_key_symbols_get_keycode(sym=0x%x) failed\n", XK_Down);
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

	scr->panel.pad = ITEM_H_MARGIN;
	scr->panel.win = xcb_generate_id(dpy);

	mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	val[0] = color2int(PANELBG);
	val[1] = XCB_EVENT_MASK_BUTTON_PRESS;
	val[1] |= XCB_EVENT_MASK_BUTTON_RELEASE;
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
	val[0] = val[1] = color2int(PANELBG);
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

	if (stat("panel/top", &st) == 0)
		panel_top = 1;
	else
		panel_top = 0;

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

	fd = open(path, O_RDONLY);
	if (fd < 3) {
		val = color->def;
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

#define pointer_inside(scr, area, ex)\
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
		if pointer_inside(scr, i, ex)\
			ii("inside element %d\n", i);\
	}\
}
#endif

static void mark_tag(int16_t x, int16_t y, int16_t tagx)
{
	struct color *bg, *fg;
	struct list_head *head;

	if (target_scr && target_tag)
		print_tag(target_scr, target_tag, color2ptr(TEXTFG_NORMAL));

	/* new selection */

	target_scr = coord2scr(x, y);
	list_walk(head, &target_scr->tags) {
		struct tag *tag = list2tag(head);

		dd("scr %d tag '%s' x (pos:%d scr:%d tag:%d)",
		   target_scr->id, tag->name, tagx, target_scr->x, tag->x);

		if (!tag_pointed(tag, tagx, y)) {
			continue;
		} else if (target_scr->tag == tag) { /* skip active tag */
			continue;
		} else if (target_tag && target_tag == tag) { /* un-mark tag */
			print_tag(target_scr, target_tag,
				  color2ptr(TEXTFG_NORMAL));
			target_tag = NULL;
			target_scr = NULL;
			return;
		}

		target_tag = tag;
		fg = color2ptr(SELECTFG);
		bg = color2ptr(SELECTBG);
		draw_panel_text(&target_scr->panel, fg, bg, tag->x, tag->w,
				tag->name, tag->nlen, font1);
		xcb_flush(dpy);
		return;
	}

	target_tag = NULL; /* not found */
}

static void toolbar_button_press(struct arg *arg)
{
	dd("toolbar focused item '%s'",
	   focused_item ? focused_item->str : "<nil>");

	if (!focused_item) {
		hide_toolbar();
		return;
	}

	if (focused_item->str == (const char *) BTN_CLOSE) {
		if (toolbar.cli)
			close_window(toolbar.cli->win);
		hide_toolbar();
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
	} else if (focused_item->str == (const char *) BTN_RETAG) {
		if (toolbar.cli && target_tag) {
			arg->data = toolbar.cli->win;
			retag_window(arg);
		}
	} else if (focused_item->str == (const char *) BTN_FLAG) {
		curscr = coord2scr(arg->x, arg->y);
		if (!curscr->tag->anchor) {
			curscr->tag->anchor = toolbar.cli;
			focused_item->flags |= TOOL_FLG_LOCK;
		} else if (curscr->tag->anchor != toolbar.cli) {
			curscr->tag->anchor = toolbar.cli;
			focused_item->flags |= TOOL_FLG_LOCK;
		} else {
			curscr->tag->anchor = NULL;
			focused_item->flags &= ~TOOL_FLG_LOCK;
		}
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
}

static void panel_button_press(xcb_button_press_event_t *e)
{
	curscr = coord2scr(e->root_x, e->root_y);

	dd("screen %d, press at %d,%d", curscr->id, e->event_x, e->event_y);

	dump_coords(curscr, e->event_x);
	if pointer_inside(curscr, PANEL_AREA_TAGS, e->event_x) {
		dd("panel press time %u", e->time);
		tag_time = e->time;
		point_tag(curscr, e->event_x, e->event_y);
		xcb_flush(dpy);
	} else if pointer_inside(curscr, PANEL_AREA_TITLE, e->event_x) {
		ii("title\n");
		curscr->flags |= SCR_FLG_SWITCH_WINDOW_NOWARP;
		switch_window(curscr, DIR_NEXT);
	} else if pointer_inside(curscr, PANEL_AREA_DOCK, e->event_x) {
		ii("dock\n");
	}
}

static void handle_button_release(xcb_button_release_event_t *e)
{
	struct client *cli;

	if (!e)
		return;

	cli = win2cli(e->child);

	if (cli && cli->flags & CLI_FLG_MOVE) {
		cli->flags &= ~CLI_FLG_MOVE;
		return;
	}

	curscr = coord2scr(e->root_x, e->root_y);

	if (curscr->panel.win == e->event) {
		if pointer_inside(curscr, PANEL_AREA_TAGS, e->event_x) {
			dd("panel release time %u", e->time - tag_time);
			if (e->time - tag_time > TAG_LONG_PRESS)
				mark_tag(e->root_x, e->root_y, e->event_x);
			else
				select_tag(curscr, e->event_x, e->event_y);
		} else if pointer_inside(curscr, PANEL_AREA_MENU, e->event_x) {
			show_menu();
		} else if pointer_inside(curscr, PANEL_AREA_DIV, e->event_x) {
			show_toolbar();
		}
	}

	xcb_ungrab_pointer(dpy, XCB_CURRENT_TIME);
	xcb_flush(dpy);
}

static void handle_button_press(xcb_button_press_event_t *e)
{
	te("XCB_BUTTON_PRESS: root 0x%x, pos %d,%d; event 0x%x, pos %d,%d; "
	   "child 0x%x, detail %d\n", e->root, e->root_x, e->root_y, e->event,
	   e->event_x, e->event_y, e->child, e->detail);

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

	if (e->event != e->root || e->child == XCB_WINDOW_NONE) {
		return;
	} else if (curscr->panel.win == e->child) {
		mark_tag(e->root_x, e->root_y, e->event_x);
		return;
	} else if (target_tag) { /* re-tag requested */
		struct arg arg = {
			.x = e->root_x,
			.y = e->root_y,
			.data = e->child,
		};
		retag_window(&arg);
		return;
	}

	/* prepare for motion event */

	raise_window(e->child);
	panel_raise(curscr);

	/* subscribe to motion events */

	xcb_grab_pointer(dpy, 0, e->root,
			 XCB_EVENT_MASK_BUTTON_MOTION |
			 XCB_EVENT_MASK_BUTTON_RELEASE,
			 XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
			 e->child, XCB_NONE, XCB_CURRENT_TIME);
	xcb_flush(dpy);
}

static uint16_t pointer_x;

static void handle_motion_notify(xcb_motion_notify_event_t *e)
{
	uint16_t mask;
	uint32_t val[2];
	struct client *cli;

	if (e->event == toolbar.panel.win) {
		focus_toolbar_item(e->event_x, e->event_y,
				   pointer_x > e->event_x);
		pointer_x = e->event_x;
		return;
	}

	curscr = coord2scr(e->root_x, e->root_y);
	if (!curscr)
		return;

	trace_screen_metrics(curscr);
	if (curscr && curscr->panel.win == e->child) {
		return;
	} else if (!e->child) {
		handle_button_release(NULL);
		return;
	}

	/* window is being moved so search in global list */
	cli = win2cli(e->child);
	if (!cli) {
		ww("win 0x%x is not managed\n", e->child);
		return;
	}

	cli->x = e->root_x - cli->w / 2;
	cli->y = e->root_y - cli->h / 2;

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
	uint8_t i;

	ii("toolbar key 0x%x\n", e->detail);
	return;

	if (toolbar.kclose == e->detail) {
		hide_toolbar();
		return;
	}

	for (i = 0; i < ARRAY_SIZE(toolbar_items); i++) {
		if (toolbar.kprev == e->detail) {
			toolbar.iprev = toolbar.ithis--;
			break;
		} else if (toolbar.knext == e->detail) {
			toolbar.iprev = toolbar.ithis++;
			break;
		}
	}

	if (toolbar.ithis == UCHAR_MAX)
		toolbar.ithis = ARRAY_SIZE(toolbar_items) - 1;
	else if (toolbar.ithis > ARRAY_SIZE(toolbar_items) - 1)
		toolbar.ithis = 0;

	draw_toolbar_text(&toolbar_items[toolbar.iprev], color2ptr(TOOLFG));
	draw_toolbar_text(&toolbar_items[toolbar.ithis], color2ptr(FOCUSFG));
}

static void handle_key_press(xcb_key_press_event_t *e)
{
	struct list_head *cur;

	if ((e->event == toolbar.panel.win || e->child == toolbar.panel.win)
	    && (e->detail == toolbar.kprev || e->detail == toolbar.knext ||
	    e->detail == toolbar.kclose) && e->state != MOD) {
		ii("toolbar key 0x%x, prev 0x%x, next 0x%x\n", e->detail,
		   toolbar.kprev, toolbar.knext);
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

	if (e->event == toolbar.panel.win) {
		focus_window(e->event);
		xcb_grab_pointer(dpy, 1, toolbar.panel.win,
			 XCB_EVENT_MASK_BUTTON_MOTION |
			 XCB_EVENT_MASK_BUTTON_PRESS |
			 XCB_EVENT_MASK_BUTTON_RELEASE,
			 XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
			 toolbar.panel.win, XCB_NONE, XCB_CURRENT_TIME);
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
		focus_window(cli->win);
		resort_client(cli);
	}

	if (e->mode == MOD)
		raise_window(e->event);

	xcb_flush(dpy);
}

static void handle_leave_notify(xcb_leave_notify_event_t *e)
{
	struct client *cli = win2cli(e->event);

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
		fg = color2ptr(SELECTFG);
		bg = color2ptr(ALERTBG);
		draw_panel_text(&cli->scr->panel, fg, bg, cli->tag->x,
				cli->tag->w, cli->tag->name, cli->tag->nlen,
				font1);
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
		te("XCB_MOTION_NOTIFY\n");
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
	if (!font2)
		ee("XftFontOpen(%s)\n", FONT2_NAME);
}

static void init_keys_def(void)
{
	strlen_t tmp;
	uint8_t i;
	char path[baselen + sizeof("/keys/") + UCHAR_MAX];
	int fd;
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
		fd = open(path, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
		write(fd, kmap->actname, strlen(kmap->actname));
		close(fd);
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
	pfd->fd = open_fifo(YAWM_CTRL, pfd->fd);
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
	init_toolbar();
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

	autostart();

	pfds[FD_SRV].fd = xcb_get_file_descriptor(dpy);
	pfds[FD_SRV].events = POLLIN;
	pfds[FD_SRV].revents = 0;

	pfds[FD_CTL].fd = open_fifo(YAWM_CTRL, -1);
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
