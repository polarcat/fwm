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

#define sslen(str) (sizeof(str) - 1)

/* defaults */

static char *basedir = "/tmp/yawm";

static int randrbase;
static int snap_margin = 4;
static int border_width = 1;
static unsigned int border_active = 0xff5050;
static unsigned int border_normal = 0x005000;
static unsigned int panel_bg = 0x101010;
static unsigned int panel_fg = 0x909090;
static unsigned int panel_height = 24; /* need to adjust with font height */

/* defines */

//#define FONT_NAME "fixed"
//#define FONT_NAME_FT "Monospace:regular:antohint=true:pixelsize=14"
#define FONT_SIZE_FT 10.
#define FONT_NAME_FT "Monospace"
#define FONT_COLOR_NORMAL { 0x7000, 0x7000, 0x7000, 0xffff, }
//#define FONT_COLOR_ACTIVE { 0x4c00, 0x7800, 0x9900, 0xffff, }
#define FONT_COLOR_ACTIVE { 0x7800, 0x9900, 0x4c00, 0xffff, }

#define FONT_NAME "-misc-fixed-bold-*-*-*-14-*-*-*-*-*-*-*"
#define FONT_ACTIVE 0x00aa00
#define FONT_NORMAL 0x909090

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

#define TIME_STR_DEF "0000-00-00 00 00:00"
#define TIME_STR_MAX sizeof(TIME_STR_DEF)

/* data structures */

struct screen {
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

static struct screen *screen;
static struct list_head screens;

struct tag {
	struct list_head head;
	struct list_head clients;

	struct screen *scr;

	xcb_gcontext_t gc;
	int16_t x;
	uint16_t w;
	int flags;

	char *name; /* visible name */
	int nlen; /* name length */
};

#define list2tag(item) list_entry(item, struct tag, head)

#if 0 /* screen arrays */
struct screens {
	struct screen **scr;
	int max;
};

static struct screens screens0;
#endif

#define list2screen(item) list_entry(item, struct screen, head)

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

/* globals */

static XftColor normal_color;
static XftColor active_color;
static XftFont *font;
static XftDraw *normal_font;
static XftDraw *active_font;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static int xscr;
static Display *xdpy;
static xcb_connection_t *dpy;

static xcb_atom_t a_utf8;
static xcb_atom_t a_name;
static xcb_atom_t a_desktop;
static xcb_atom_t a_delete_window;
static xcb_atom_t a_change_state;
static xcb_atom_t a_state;
static xcb_atom_t a_protocols;
static xcb_atom_t a_client_list;

static uint8_t title_len; /* last known title len */
static char title_buf[STATUS_BUF_MAX];

static time_t prev_time;

/* <panel stuff> */
static char *panel_buf;

static uint16_t panel_edge_xpad;
static uint16_t panel_text_ypos;
static uint16_t panel_time_xpos;
static uint16_t panel_time_width;

static int tags_x = 5; /* x offset of tags bar */
static char *tags_ptr; /* tags string ptr */

static int menu_x;  /* x offset of menu button */
static char *menu_ptr; /* menu string ptr */

static int title_x; /* x offset of title bar */
static char *title_ptr; /* title bar ptr */

static int dock_x; /* x offset of dock bar */

static int clock_x; /* x offset of clock bar */
static char *clock_ptr; /* clock bar ptr */
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

	*w = ext.width;
	*h = ext.height;
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

#define xcb(cookie, func) {\
	cookie = func;\
	xcb_generic_error_t *error__ = xcb_request_check(dpy, cookie);\
        if (error__)\
		panic("%s, err=%d\n", #func, error__->error_code);\
}

static void change_single_value(xcb_window_t win, uint32_t val, uint32_t mask)
{
	uint32_t v[1];

	v[0] = val;
	xcb_change_window_attributes(dpy, win, mask, v);
}

#define PROP_TYPE_ANY XCB_GET_PROPERTY_TYPE_ANY

static xcb_get_property_reply_t *get_prop_any(xcb_window_t w, xcb_atom_t a)
{
	xcb_get_property_cookie_t c;

	c = xcb_get_property(dpy, 0, w, a, PROP_TYPE_ANY, 0, PROP_MAX);
	return xcb_get_property_reply(dpy, c, NULL);
}

#if 0
static int draw_text_utf8(xcb_window_t win, int active, int x, int y,
			  const char *text, int len)
{
#if 0
	XftFont *font;
	XftColor color;
	XftDraw *draw;
	XRenderColor c = { 0x4c00, 0x7800, 0x9900, 0xffff, };

	font = XftFontOpen(dpy, win->scr, XFT_FAMILY, XftTypeString, "Monospace",
			   XFT_SIZE, XftTypeDouble, 10.0, NULL);
        XftColorAllocValue(dpy, root_visual, root_cmap, &blue, &color);
	draw = XftDrawCreate(dpy, win, root_visual, root_cmap);
	XftDrawStringUtf8(draw, &color, font, x, y, (XftChar8 *) text, len);
#endif
	XftColor *color;
	XftDraw *draw;

	draw = XftDrawCreate(dpy, win, font_visual, font_cmap);
	if (active)
		color = &active_color;
	else
		color = &normal_color;

	XftDrawStringUtf8(draw, color, font, x, y, (XftChar8 *) text, len);
	return 0;
}

static int panel_text(int active, int x, int y, const char *text, int len)
{
	if (active)
		color = &active_color;
	else
		color = &normal_color;

	XftDrawStringUtf8(draw, color, font, x, y, (XftChar8 *) text, len);
}
#endif

#if 0
static int draw_text(xcb_window_t win, uint32_t color,
		     int x, int y, const char *text, int len)
{
	uint32_t mask, val[3];
	xcb_gcontext_t gc;
	xcb_font_t font;
	xcb_void_cookie_t c;

	/* get font */
	font = xcb_generate_id(dpy);
	xcb(c, xcb_open_font_checked(dpy, font, sslen(FONT_NAME), FONT_NAME));

        /* create graphics context */
        gc = xcb_generate_id(dpy);
        mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_FONT;
	val[0] = color;
	val[1] = panel_bg;
	val[2] = font;
        xcb(c, xcb_create_gc_checked(dpy, gc, win, mask, val));
        xcb(c, xcb_close_font_checked(dpy, font));

	xcb(c, xcb_image_text_8_checked(dpy, len, win, gc, x, y, text));
	xcb(c, xcb_free_gc(dpy, gc));

	return xcb_query_text_extents_sizeof(text, len * 2);
}
#endif

static void show_title(xcb_window_t win)
{
	char *buf;
	int16_t y;
	uint8_t len;
	xcb_get_property_cookie_t c;
	xcb_icccm_get_text_property_reply_t info;

	c = xcb_icccm_get_wm_name(dpy, win);
	if (!xcb_icccm_get_wm_name_reply(dpy, c, &info, NULL)) {
		ww("xcb_icccm_get_wm_name_reply() failed\n");
		return;
	}

	if (!info.name)
		return;

	y = panel_height - 5;
	len = info.name_len;

	if (len > TITLE_LINE_MAX) {
		len = TITLE_LINE_MAX;
	} else {
		if (title_len > len) {
			char *tmp = calloc(1, title_len);
			memset(tmp, ' ', title_len);
#if 0
			draw_text(screen->panel, FONT_ACTIVE, 0, y, tmp,
				  title_len);
#endif
			XftDrawStringUtf8(screen->draw, &active_color, font,
					  0, y, (XftChar8 *) tmp, title_len);
			free(tmp);
		}
	}

#if 0
	buf = title_buf;
	sprintf(buf, " ^ ");
	buf += sizeof(" ^ ");
	snprintf(buf, len, "%s", info.name);

	title_len = title_buf - buf;
#endif
	XftDrawStringUtf8(screen->draw, &normal_color, font, 0, y,
			  (XftChar8 *) info.name, title_len);
#if 0
	draw_text(screen->panel, FONT_NORMAL, 0, y, info.name, title_len);
#endif
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

#if 0
static void client_name(struct client *cli)
{
	xcb_get_property_cookie_t c;
	xcb_icccm_get_text_property_reply_t info;

	c = xcb_icccm_get_wm_name(dpy, cli->win);
	if (!xcb_icccm_get_wm_name_reply(dpy, c, &info, NULL)) {
		ww("xcb_icccm_get_wm_name_reply() failed\n");
		return;
	}

	dd("%d-bits, %d %s charactes\n", info.format, info.name_len,
	   info.encoding == a_utf8 ? "utf8" : "");
	dd("name: %s, encoding: %d\n", info.name, info.encoding);

	if (!info.name) {
		return;
	} else if (info.format == 8 && info.encoding != a_utf8) {
		cli->name = strdup(info.name);
	} else {
		int size = info.format / 8 * info.name_len;
		cli->name = calloc(1, size + 1);
		if (!cli->name) {
			ee("calloc(1, %d) failed\n", size + 1);
			return;
		}
		memcpy(cli->name, info.name, size);
	}
}
#endif

static void window_raise(xcb_window_t win)
{
	uint32_t val[1] = { XCB_STACK_MODE_ABOVE, };
	uint32_t mask = XCB_CONFIG_WINDOW_STACK_MODE;

	xcb_change_window_attributes(dpy, win, mask, val);
}

static void window_focus(xcb_window_t win, int focus)
{
	uint32_t val[1];
	uint32_t mask = XCB_CW_BORDER_PIXEL;

	tt("win %p, focus %d\n", win, focus);

	if (focus) {
		val[0] = border_active;
		show_title(win);
	} else {
		val[0] = border_normal;
	}

	xcb_change_window_attributes(dpy, win, mask, val);
        xcb_set_input_focus(dpy, XCB_NONE, XCB_INPUT_FOCUS_POINTER_ROOT,
                            XCB_CURRENT_TIME);
	xcb_flush(dpy);
}

#if 0
static void client_tag(struct client *cli)
{
	struct tag *tag = list2tag(screen->tags.next);

	list_add(&tag->clients, &cli->head);
}
#endif

#if 0
static void client_move(struct client *cli, int x, int y)
{
	int val[2], mask;

	cli->x = x;
	cli->y = y;

#if 0
	/* correct location if partially displayed */
	if (cli->x + cli->w >= cli->scr->w)
		cli->x = 0;
	if (cli->y + cli->h >= cli->scr->y)
		cli->y = dy;
#endif

	if (screen->flags & SCR_FLG_PANEL_TOP)
		cli->y += panel_height;

	mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
	val[0] = x;
	val[1] = y;
	xcb_configure_window(dpy, cli->win, mask, val);
	xcb_flush(dpy);

	dd("cli %p geo %dx%d+%d+%d, name %s\n", cli, cli->w, cli->h,
	   cli->x, cli->y, cli->name);
}

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

static void client_moveresize(struct client *cli, int x, int y, int w, int h)
{
	int val[4], mask;

	cli->x = x;
	cli->y = y;
	cli->w = w;
	cli->h = h;

	/* fit into monitor space */
	if (cli->w > cli->scr->w)
		cli->w = cli->scr->w - border_width;
	else if (cli->w < WIN_WIDTH_MIN)
		cli->w = cli->scr->w / 2 - border_width;
	if (cli->h > cli->scr->h)
		cli->h = cli->scr->h - border_width;
	else if (cli->h < WIN_HEIGHT_MIN)
		cli->h = cli->scr->h / 2 - border_width;

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
	mask = XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
	mask |= XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
	xcb_configure_window(dpy, cli->win, mask, val);
	xcb_flush(dpy);

	dd("cli %p, win %p, geo %dx%d+%d+%d\n", cli, cli->win,
	   cli->w, cli->h, cli->x, cli->y);
}

static struct client *win2client(xcb_window_t win)
{
	struct list_head *cur;
	struct tag *tag = list2tag(screen->tags.next);

	list_walk(cur, &tag->clients) {
		struct client *cli = list2client(cur);
		if (cli->win == win)
			return cli;
	}
	return NULL;
}

static void client_add(xcb_window_t win)
{
	struct tag *tag;
	struct client *cli;
	uint32_t val, msk;
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
// && a->map_state != XCB_MAP_STATE_VIEWABLE) {
		ww("ignore window %p");
		goto out;
	}

#if 0
	/* tell x server to restore window upon our sudden exit */
	xcb_change_save_set(dpy, XCB_SET_MODE_INSERT, w);
#endif
	val = border_width;
	msk = XCB_CONFIG_WINDOW_BORDER_WIDTH;
	xcb_change_window_attributes(dpy, win, msk, &val);

	/* subscribe events */
	val = XCB_EVENT_MASK_ENTER_WINDOW |
	      XCB_EVENT_MASK_PROPERTY_CHANGE;
	msk = XCB_CW_EVENT_MASK;
	xcb_change_window_attributes(dpy, win, msk, &val);

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

	tag = list2tag(screen->tags.next);
	list_add(&tag->clients, &cli->head);

#if 0
	client_name(cli);
#endif
	client_moveresize(cli, g->x, g->y, g->width, g->height);
#if 0
	client_tag(cli);
#endif
	window_raise(cli->win);
#if 0 /* FIXME: focus window under cursor instead */
	window_focus(cli->win, 1);
#endif

	xcb_map_window(dpy, cli->win);
	xcb_flush(dpy);

	mm("cli %p, win %p, geo %dx%d+%d+%d\n", cli, cli->win,
	   cli->w, cli->h, cli->x, cli->y);

	update_clients_list();
out:
	free(a);
	free(g);
}

static void client_del(xcb_window_t win, struct client *cli)
{
	if (win == screen->ptr->root) {
		ww("wtf, root window destroyed!\n");
		return;
	}

	if (!cli) {
		cli = win2client(win);
		if (!cli) {
			ww("window %p is not managed\n", win);
			return;
		}
	}

	list_del(&cli->head);
	free(cli);
}

static void clients_scan(void)
{
	int i, n;
	xcb_query_tree_cookie_t c;
	xcb_query_tree_reply_t *tree;
	xcb_query_pointer_reply_t *ptr;
	xcb_window_t *wins;

	struct client *client;
	uint32_t ws;

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

	free(tree);
	update_clients_list();

	tt("done\n");

#if 0
    changeworkspace(0);
    /*
     * Get pointer position so we can set focus on any window which
     * might be under it.
     */
    pointer = xcb_query_pointer_reply(
        conn, xcb_query_pointer(conn, screen->root), 0);

    if (NULL == pointer)
    {
        focuswin = NULL;
    }
    else
    {
        setfocus(findclient(pointer->child));
        free(pointer);
    }
    xcb_flush(conn);
    free(reply);
#endif
}

static void fill_rect(xcb_window_t win, xcb_gcontext_t gc,
		      int16_t x, int16_t y, uint16_t w, uint16_t h)
{
	xcb_rectangle_t rect = { x, y, w, h, };
	xcb_poly_fill_rectangle(dpy, win, gc, 1, &rect);
}

static void print_time(struct screen *scr)
{
	time_t t;
	struct tm *tm;
	char str[TIME_STR_MAX];

	t = time(NULL);
	if (t == prev_time)
		return;

	tm = localtime(&t);
	if (!tm)
		return;

	if (strftime(str, TIME_STR_MAX, "%Y-%m-%d %V %H:%M", tm) == 0)
		strncpy(str, TIME_STR_DEF, TIME_STR_MAX);

	fill_rect(scr->panel, scr->gc, panel_time_xpos, 0, panel_time_width,
		  panel_height);
	XftDrawStringUtf8(scr->draw, &normal_color, font, panel_time_xpos,
			  panel_text_ypos, (XftChar8 *) str, sizeof(str) - 1);
	prev_time = t;
}

static void tag_refresh(struct screen *scr, struct tag *tag, XftColor *color)
{
	fill_rect(scr->panel, scr->gc, tag->x, 0, tag->w, panel_height);
	XftDrawStringUtf8(scr->draw, color, font, tag->x, panel_text_ypos,
			  (XftChar8 *) tag->name, tag->nlen);
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

	if (scr->tag && tag_clicked(scr->tag, x)) {
		return;
	} else if (scr->tag) { /* deselect current instantly */
		scr->tag->flags &= ~TAG_FLG_ACTIVE;
		color = &normal_color;
		tag_refresh(scr, scr->tag, color);
#if 0
		fill_rect(scr->panel, scr->gc, scr->tag->x, 0, scr->tag->w,
			  panel_height);
		XftDrawStringUtf8(scr->draw, color, font, scr->tag->x,
				  panel_text_ypos, (XftChar8 *) scr->tag->name,
				  scr->tag->nlen);
#endif
	}

	list_walk(cur, &screen->tags) {
		struct tag *tag = list2tag(cur);

#if 0
		if (x >= tag->x && x <= tag->x + tag->w) {
#else
		if (tag == scr->tag) {
			continue;
		} else if (!tag_clicked(tag, x)) {
			tag->flags &= ~TAG_FLG_ACTIVE;
			color = &normal_color;
			tag_refresh(scr, tag, color);
		} else {
#endif
			tag->flags |= TAG_FLG_ACTIVE;
			color = &active_color;
			scr->tag = tag;
			tag_refresh(scr, tag, color);
			break;
		}
#if 0
		} else {
			tag->flags &= ~TAG_FLG_ACTIVE;
			color = &normal_color;
		}
#endif
#if 0
		fill_rect(scr->panel, scr->gc, tag->x, 0, tag->w, panel_height);
		XftDrawStringUtf8(scr->draw, color, font, tag->x,
				  panel_text_ypos, (XftChar8 *) tag->name,
				  tag->nlen);
		tag_refresh(scr, tag, color);
#endif
	}

#if 0
	if (scr->tag) {
		fill_rect(scr->panel, scr->gc, scr->tag->x, 0, scr->tag->w,
			  panel_height);
		XftDrawStringUtf8(scr->draw, &active_color, font, sel->x,
				  panel_text_ypos, (XftChar8 *) sel->name,
				  sel->nlen);
	}
#endif

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
	if (panel_text_ypos == 0)
		panel_text_ypos = panel_height - (panel_height - h) / 2;
#if 0
	len = draw_text(scr->panel, FONT_ACTIVE, 5, y, name, len);
#endif

	if (pos != panel_edge_xpad) {
		color = &normal_color;
	} else {
		color = &active_color;
		tag->flags |= TAG_FLG_ACTIVE;
	}

	XftDrawStringUtf8(scr->draw, color, font, pos, panel_text_ypos,
			  (XftChar8 *) tag->name, tag->nlen);

	tag->x = pos;
	tag->w += h; /* h for spacing */
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
 * $HOME/.yawm/3monitors/
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
static void init_tags(struct screen *scr, int idx)
{
	struct list_head *cur;
	int fd, i, pos;
	char path[BASE_PATH_MAX], name[TAG_NAME_MAX + 2], *ptr;

	if (!basedir) {
		ii("no tags configured\n");
		return;
	}

	snprintf(path, sizeof(path), "%s/%d/", basedir, idx);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		ii("no tags for screen %d\n", idx);
		return;
	}
	close(fd);
	ptr = path + strlen(path);

	pos = panel_edge_xpad;
	memset(name, 0, sizeof(name));
	for (i = 0; i < TAGS_MAX; i++) {
		sprintf(ptr, "%d", i);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		read(fd, name, TAG_NAME_MAX);
		mm("scr%d, tag%d: %s\n", idx, i, name);
		close(fd);
		pos = tag_add(scr, name, pos);
		memset(name, 0, TAG_NAME_MAX);
	}
}

static void init_panel(struct screen *scr, int idx)
{
	int y;
	uint16_t h;
	uint32_t val[2], mask;

	scr->panel = xcb_generate_id(dpy);
	dd("panel window %x\n", scr->panel);

	mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	val[0] = panel_bg;
	val[1] = XCB_EVENT_MASK_BUTTON_PRESS;

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
	xcb_flush(dpy);

	/* now correct screen height */
	scr->h -= panel_height;

#if 0
	scr->draw = XftDrawCreate(xdpy, scr->panel, scr->visual,
				  scr->ptr->default_colormap);
#else
	scr->draw = XftDrawCreate(xdpy, scr->panel,
				  DefaultVisual(xdpy, xscr),
				  DefaultColormap(xdpy, xscr));
#endif

	text_exts(TIME_STR_DEF, TIME_STR_MAX, &panel_time_width, &h);
	panel_edge_xpad = h;
	panel_time_xpos = scr->w - panel_time_width - h;

	init_tags(scr, idx);
	print_time(scr);
}

static void click_panel(xcb_button_press_event_t *e)
{
	switch (e->detail) {
	case MOUSE_BTN_LEFT:
		dd("MOUSE_BTN_LEFT\n");
		if (screen->panel == e->event)
			tag_select(screen, e->event_x);
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
}

#if 1
static void screen_add(xcb_screen_t *ptr, int idx)
{
	struct screen *scr;

	tt("\n");

	if (!ptr)
		panic("xcb_screen_t pointer is null\n")

	scr = calloc(1, sizeof(*scr));
	if (!scr)
		panic("calloc(%lu) failed\n", sizeof(*scr));

	/* initialize monitor */
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
		init_panel(scr, idx);
		if (!list_empty(&scr->tags)) {
			ii("current tag %p %s\n", list2tag(scr->tags.next),
			   (list2tag(scr->tags.next))->name);
		}
	}

	list_add(&screens, &scr->head);
}
#else
static void screen_add(xcb_screen_t *ptr, int idx)
{
	struct screen *scr;
	int len;

	if (!ptr)
		panic("xcb_screen_t pointer is null\n")

	tt("screen[%d]\n", idx);

	scr = calloc(1, sizeof(*scr));
	if (!scr && screens0.max == 0)
		panic("calloc(%lu) failed\n", sizeof(*scr));

	/* initialize monitor */
	scr->ptr = ptr;
	scr->num = idx;
	scr->x = 0;
	scr->y = 0;
	scr->w = ptr->width_in_pixels;
	scr->h = ptr->height_in_pixels;
	scr->flags |= SCR_FLG_PANEL;
	strncpy(scr->menustr, "::", sizeof("::") - 1);

	list_init(&scr->tags);
	tag_add(scr, "main", sizeof("main") - 1);

	ii("screen[%d] %p, size %dx%d\n", idx, scr, scr->w, scr->h);
	ii("current tag %p %s\n", list2tag(scr->tags.next),
	   (list2tag(scr->tags.next))->name);

	if (scr->flags & SCR_FLG_PANEL)
		init_panel(scr);

	if (!screens0.scr) {
		len = sizeof(screens0.scr[0]);
		screens0.scr = calloc(1, len);
		if (!screens0.scr)
			panic("calloc(%d) failed\n", len);
	} else {
		len = (screens0.max + 1) * sizeof(*screens0.scr[0]);
		screens0.scr = realloc(screens0.scr, len);
		if (!screens0.scr) {
			ee("realloc(%d) failed\n", len);
			return;
		}
	}

	screens0.scr[screens0.max] = scr;
	screens0.max++;
	dd("max screens %d\n", screens0.max);
}
#endif

static void wait_events(void)
{
	int state;
	xcb_generic_event_t *e;

	tt("\n");
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

static void window_configure(void *arg)
{
	int32_t val[7] = { 0 };
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

#if 1
	e = xcb_wait_for_event(dpy);
#else
	e = xcb_poll_for_event(dpy);
#endif
	if (xcb_connection_has_error(dpy))
		panic("failed to get event\n");

	if (!e)
		return;

	tt("got event %d (%d)\n", e->response_type, XCB_EVENT_RESPONSE_TYPE(e));

#if 1
	switch (e->response_type & ~0x80) {
#else
	switch (e->response_type & ~0x80) {
	case XCB_VISIBILITY_NOTIFY:
		mm("XCB_VISIBILITY_NOTIFY\n");
		switch (((xcb_visibility_notify_event_t *) e)->state) {
		case XCB_VISIBILITY_FULLY_OBSCURED:
			mm("XCB_VISIBILITY_FULLY_OBSCURED\n");
			wait_events();
			break;
		case XCB_VISIBILITY_PARTIALLY_OBSCURED: /* fall through */
		case XCB_VISIBILITY_UNOBSCURED:
			mm("XCB_VISIBILITY_UNOBSCURED\n");
			break;
		}
		break;
#endif
	case 0: break; /* NO EVENT */
	case XCB_VISIBILITY_NOTIFY:
		tt("XCB_VISIBILITY_NOTIFY: win %p\n",
		   ((xcb_visibility_notify_event_t *)e)->window);
		break;
	case XCB_CREATE_NOTIFY:
		tt("XCB_CREATE_NOTIFY: win %p\n",
		   ((xcb_create_notify_event_t *)e)->window);
		break;
	case XCB_BUTTON_PRESS:
		tt("XCB_BUTTON_PRESS: win %p\n",
		   ((xcb_button_press_event_t *)e)->event);
		click_panel((xcb_button_press_event_t *)e);
		break;
	case XCB_BUTTON_RELEASE:
		tt("XCB_BUTTON_RELEASE\n");
		break;
	case XCB_CLIENT_MESSAGE:
		tt("XCB_CLIENT_MESSAGE\n");
		break;
	case XCB_CONFIGURE_REQUEST:
		tt("XCB_CONFIGURE_REQUEST: win %p\n",
		   ((xcb_configure_request_event_t *)e)->window);
		print_configure_request(e);
		window_configure(e);
		break;
	case XCB_CONFIGURE_NOTIFY:
		tt("XCB_CONFIGURE_NOTIFY: win %p\n",
		   ((xcb_configure_notify_event_t *)e)->window);
		break;
	case XCB_DESTROY_NOTIFY:
		tt("XCB_DESTROY_NOTIFY: win %p\n",
		   ((xcb_destroy_notify_event_t *)e)->window);
		client_del(((xcb_destroy_notify_event_t *)e)->window, NULL);
		break;
	case XCB_ENTER_NOTIFY:
		tt("XCB_ENTER_NOTIFY\n");
		dd("root %p, event %p, child %p\n",
		   ((xcb_enter_notify_event_t *)e)->root,
		   ((xcb_enter_notify_event_t *)e)->event,
		   ((xcb_enter_notify_event_t *)e)->child);
		window_focus(((xcb_enter_notify_event_t *)e)->event, 1);
		break;
	case XCB_LEAVE_NOTIFY:
		tt("XCB_LEAVE_NOTIFY\n");
		dd("root %p, event %p, child %p\n",
		   ((xcb_enter_notify_event_t *)e)->root,
		   ((xcb_enter_notify_event_t *)e)->event,
		   ((xcb_enter_notify_event_t *)e)->child);
		window_focus(((xcb_enter_notify_event_t *)e)->event, 0);
		break;
	case XCB_EXPOSE:
		tt("XCB_EXPOSE: win %p\n", ((xcb_expose_event_t *)e)->window);
		break;
	case XCB_FOCUS_IN:
		tt("XCB_FOCUS_IN\n");
		break;
	case XCB_KEY_PRESS:
		tt("XCB_KEY_PRESS\n");
		break;
	case XCB_MAPPING_NOTIFY:
		tt("XCB_MAPPING_NOTIFY\n");
		break;
	case XCB_MAP_NOTIFY:
		mm("XCB_MAP_NOTIFY: win %p\n",
		   ((xcb_map_notify_event_t *)e)->window);
		break;
	case XCB_MAP_REQUEST:
		mm("XCB_MAP_REQUEST: win %p\n",
		   ((xcb_map_request_event_t *)e)->window);
		client_add(((xcb_map_notify_event_t *)e)->window);
		break;
	case XCB_MOTION_NOTIFY:
		tt("XCB_MOTION_NOTIFY\n");
		break;
	case XCB_PROPERTY_NOTIFY:
		tt("XCB_PROPERTY_NOTIFY: %p\n",
		   ((xcb_property_notify_event_t *)e)->window);
		dd("prop:\n"
		   " response_type=%u\n"
		   " sequence=%u\n"
		   " atom=%d\n"
		   " state=%u\n",
		   ((xcb_property_notify_event_t *)e)->response_type,
		   ((xcb_property_notify_event_t *)e)->sequence,
		   ((xcb_property_notify_event_t *)e)->atom,
		   ((xcb_property_notify_event_t *)e)->state);
		switch (((xcb_property_notify_event_t *)e)->atom) {
		case XCB_ATOM_WM_NAME:
			show_title(((xcb_property_notify_event_t *)e)->window);
			break;
		}
		break;
	case XCB_UNMAP_NOTIFY:
		tt("XCB_UNMAP_NOTIFY\n");
		break;
	default:
		tt("unhandled event type %d\n", type);
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
	uint32_t val = XCB_EVENT_MASK_BUTTON_PRESS |
		       XCB_EVENT_MASK_BUTTON_RELEASE;

	xcb_grab_button(dpy, 0, win, val, XCB_GRAB_MODE_ASYNC,
			XCB_GRAB_MODE_ASYNC, win, XCB_NONE, MOUSE_BTN_LEFT,
			XCB_MOD_MASK_1);
	xcb_grab_button(dpy, 0, win, val, XCB_GRAB_MODE_ASYNC,
			XCB_GRAB_MODE_ASYNC, win, XCB_NONE, MOUSE_BTN_MID,
			XCB_MOD_MASK_1);
	xcb_grab_button(dpy, 0, win, val, XCB_GRAB_MODE_ASYNC,
			XCB_GRAB_MODE_ASYNC, win, XCB_NONE, MOUSE_BTN_RIGHT,
			XCB_MOD_MASK_1);

	/* subscribe events */
	val = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
	      XCB_EVENT_MASK_STRUCTURE_NOTIFY |
	      XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
	xcb_change_window_attributes(dpy, win, XCB_CW_EVENT_MASK, &val);
	xcb_flush(dpy);
}

#define atom_by_name(name) get_atom_by_name(name, sizeof(name) - 1)

#if 0
void static init_font(struct screen *scr)
#else
void static init_font(void)
#endif
{
	XRenderColor normal = FONT_COLOR_NORMAL;
	XRenderColor active = FONT_COLOR_ACTIVE;
#if 0
	xcb_visualtype_iterator_t visual_iter;
	xcb_depth_iterator_t depth_iter;

	depth_iter = xcb_screen_allowed_depths_iterator(scr->ptr);
	for (; depth_iter.rem; xcb_depth_next(&depth_iter)) {
		visual_iter = xcb_depth_visuals_iterator(depth_iter.data);
		for (; visual_iter.rem; xcb_visualtype_next(&visual_iter)) {
			xcb_visualid_t tmp = scr->ptr->root_visual;
			if (tmp == visual_iter.data->visual_id) {
				scr->visual = visual_iter.data;
				break;
			}
		}
	}

	if (!scr->visual)
		panic("cannot get font visual\n");
#endif
	font = XftFontOpen(xdpy, xscr, XFT_FAMILY, XftTypeString,
			   FONT_NAME_FT, XFT_SIZE, XftTypeDouble,
			   FONT_SIZE_FT, NULL);
	if (!font)
		panic("XftFontOpen(%s)\n", FONT_NAME_FT);

#if 0
        XftColorAllocValue(xdpy, scr->visual, scr->ptr->default_colormap,
			   &normal, &normal_color);
        XftColorAllocValue(xdpy, scr->visual, scr->ptr->default_colormap,
			   &active, &active_color);
#else
        XftColorAllocValue(xdpy, DefaultVisual(xdpy, xscr),
			   DefaultColormap(xdpy, xscr),
			   &normal, &normal_color);
        XftColorAllocValue(xdpy, DefaultVisual(xdpy, xscr),
			   DefaultColormap(xdpy, xscr),
			   &active, &active_color);
#endif
}

int main()
{
	int val;
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

	a_name = atom_by_name("_NEW_WM_NAME");
	a_desktop = atom_by_name("_NET_WM_DESKTOP");
	a_delete_window = atom_by_name("WM_DELETE_WINDOW");
	a_change_state = atom_by_name("WM_CHANGE_STATE");
	a_state = atom_by_name("WM_STATE");
	a_protocols = atom_by_name("WM_PROTOCOLS");
	a_client_list = atom_by_name("_NET_CLIENT_LIST");
	a_utf8 = atom_by_name("UTF8_STRING");

	list_init(&screens);

	/* create default screen */
	screen_add(xcb_setup_roots_iterator(xcb_get_setup(dpy)).data, 0);
	/* set current screen */
#if 1
	screen = list2screen(screens.next);
#else
	screen = screens0.scr[0];
#endif
	init_root(screen->ptr->root);
#if 0
	init_font(screen);
#endif

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
