/* menu.c: rudimentary menu dialog
 *
 * Copyright (c) 2015, Aliaksei Katovich <aliaksei.katovich at gmail.com>
 *
 * Released under the GNU General Public License, version 2
 */

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <poll.h>

#include <math.h>

#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>

#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-x11.h>

#include <X11/Xlib-xcb.h>
#include <X11/Xft/Xft.h>

#include "yawm.h"
#include "misc.h"

#ifdef DEBUG
#undef dd
#define dd(...) fprintf(stderr, __VA_ARGS__)
#else /* ! DEBUG */
#undef dd
#define dd(...) ;
#endif /* DEBUG */

#define FONT_H_MARGIN 2
#define COLOR_DISTANCE 5

#define PROMPT_LEN 2

static int xscr_;
static Display *xdpy_;
static xcb_connection_t *dpy_;
static xcb_drawable_t win_;
static xcb_gcontext_t gc_;
static xcb_key_symbols_t *syms_;
static uint8_t done_;

static char search_buf_[UCHAR_MAX];
static uint8_t search_idx_ = PROMPT_LEN; /* '> ' */
static int16_t found_idx_;

static uint16_t page_w_;
static uint16_t page_h_;

static uint8_t row_len_ = 80; /* characters */
static uint8_t rows_per_page_ = 25;
static uint8_t pages_num_;
static uint8_t rows_rem_;

struct page {
	char *rowptr;
	uint16_t rowidx;
};

static struct page pages_[UCHAR_MAX + 1];

static uint8_t search_bar_;
static uint8_t append_;
static uint8_t interlace_;
static uint32_t fg_ = 0xa0a0a0;
static uint32_t bg_ = 0x050505;
static uint32_t selfg_ = 0xe0e0e0;
static uint32_t selbg_ = 0x303030;
static XftFont *font1_;
static XftFont *font2_;
static XftFont *font_; /* current font */
static XftDraw *draw_;
static XftColor selfg_xft_;
static XftColor fg_xft_;

static char *font1_name_ = FONT1_NAME;
static float font1_size_ = FONT1_SIZE;
static char *font2_name_ = FONT2_NAME;
static float font2_size_ = FONT1_SIZE;

static struct xkb_context *xkb_;
static struct xkb_keymap *keymap_;
static struct xkb_state *state_;
static uint8_t level_;
static uint8_t control_;

static uint16_t x_pad_;
static uint16_t y_pad_;
static uint16_t row_h_;
static int16_t text_y_;

struct column {
	char *str;
	uint8_t len;
};

struct row {
	char *str;
	uint8_t len;
	int16_t row_pos;
	uint8_t idx;
	struct column *cols;
};

static char *data_;
static size_t data_size_;

static struct row *selrow_;
static struct row *rows_;

static struct row **view_;

static uint16_t *cols_px_;
static uint8_t *cols_len_;

static uint8_t cols_per_row_;
static uint16_t rows_num_; /* lines in file */
static uint8_t swap_col_idx_;
static uint8_t search_col_idx_;

static uint8_t selidx_;
static uint8_t page_idx_;
static uint8_t follow_;

static const char *path_;
static const char *name_ = "menu";

static void text_size(const char *text, int len, uint16_t *w, uint16_t *h)
{
	XGlyphInfo ext;

	XftTextExtentsUtf8(xdpy_, font_, (XftChar8 *) text, len, &ext);
	ext.width % 2 ? (*w = ext.width + 1) : (*w = ext.width);
	ext.height % 2 ? (*h = ext.height + 1) : (*h = ext.height);
}

static void fill_rect(uint32_t *c, int16_t x, int16_t y, uint16_t w, uint16_t h)
{
	xcb_rectangle_t rect = { x, y, w, h, };
	xcb_change_gc(dpy_, gc_, XCB_GC_FOREGROUND, c);
	xcb_poly_fill_rectangle(dpy_, win_, gc_, 1, &rect);
}

static int16_t draw_rect(uint8_t idx, uint8_t focus)
{
	int16_t y = idx * row_h_ + y_pad_;
	uint32_t bg;

	focus ? (bg = selbg_) : (bg = bg_);

	if (interlace_ && !focus && idx % 2) {
		bg = ((((bg >> 16) & 0xff) + COLOR_DISTANCE) << 16) |
		     ((((bg >> 8) & 0xff) + COLOR_DISTANCE) << 8) |
		     ((bg & 0xff) + COLOR_DISTANCE);
	}

	fill_rect(&bg, x_pad_, y, page_w_, row_h_);

	return y;
}

static int16_t draw_col(int16_t x, int16_t y, const char *str, uint8_t len,
			uint8_t focus)
{
	XftColor *fg;
	uint16_t w;
	uint16_t h;

	if (!str || !len)
		return 0;

	if (*str == '\a' && font2_) {
		font_ = font2_;
		str++;
		len--;
	}

	focus ? (fg = &selfg_xft_) : (fg = &fg_xft_);

	XftDrawStringUtf8(draw_, fg, font_, x, y, (XftChar8 *) str, len);
	XSync(xdpy_, 0);

	text_size(str, len, &w, &h);
	font_ = font1_; /* restore default */

	return x + w + x_pad_;
}

static void draw_search_bar(void)
{
	if (!search_bar_)
		return;

	int16_t x = x_pad_ * 2;
	int16_t y = draw_rect(rows_per_page_, 0) + text_y_;
	uint8_t len = search_idx_ + 1;
	char *str = search_buf_;
	XftColor *fg = &selfg_xft_;

	search_buf_[0] = '>';
	search_buf_[1] = ' ';
	search_buf_[search_idx_] = '_';

	XftDrawStringUtf8(draw_, fg, font_, x, y, (XftChar8 *) str, len);
	XSync(xdpy_, 0);
}

static void swap_cols(struct row *row, uint8_t dst, uint8_t src)
{
	char *str;
	uint8_t len;
	uint16_t px;

	str = row->cols[dst].str;
	len = row->cols[dst].len;
	row->cols[dst].str = row->cols[src].str;
	row->cols[dst].len = row->cols[src].len;
	row->cols[src].str = str;
	row->cols[src].len = len;

	len = cols_len_[dst];

	if (cols_len_[dst] < cols_len_[src])
		cols_len_[dst] = cols_len_[src];

	cols_len_[src] = len;
	px = cols_px_[dst];

	if (cols_px_[dst] < cols_px_[src])
		cols_px_[dst] = cols_px_[src];

	cols_px_[src] = px;
}

static void draw_row(struct row *row, uint8_t idx, uint8_t focus)
{
	struct column *col = row->cols;
	char *start = row->str;
	char *ptr = start;
	const char *end = ptr + row->len;
	int16_t x;
	int16_t y;
	uint8_t i;
	uint8_t len;

	col->str = start;

	while (ptr < end) {
		if (*ptr == '\t' || *ptr == '\n') {
			col->len = ptr - start;
			col++;
			start = ptr + 1;
			col->str = start;
		}

		ptr++;
	}

	if (ptr == end)
		col->len = ptr - start;

	y = draw_rect(idx, focus) + text_y_;
	x = x_pad_ * 2;

	if (swap_col_idx_)
		swap_cols(row, 0, swap_col_idx_);

	for (i = 0; i < cols_per_row_; i++) {
		uint16_t w;
		uint16_t h;

		text_size(row->cols[i].str, row->cols[i].len, &w, &h);

		if (w > cols_px_[i])
			len = cols_len_[i];
		else
			len = row->cols[i].len;

		draw_col(x, y, row->cols[i].str, len, focus);
		x += cols_px_[i];
	}
}

static void draw_menu(void)
{
	char *start = pages_[page_idx_].rowptr;
	char *end = data_ + data_size_;
	char *ptr = start;
	uint8_t focus = 0;
	uint16_t rowidx = pages_[page_idx_].rowidx;
	uint8_t i = 0;

	while (ptr < end) {
		if (*ptr == '\n') {
			size_t size = ptr - start;
			struct row *row = &rows_[rowidx++];

			row->str = start;
			row->len = size;

			if (!selrow_ && selidx_ && i == selidx_) {
				selrow_ = row;
				focus = 1;
			} else if (!selrow_ && !selidx_) {
				selrow_ = row;
				focus = 1;
			} else if (selrow_ == row) {
				focus = 1;
			} else {
				focus = 0;
			}

			draw_row(row, i, focus);
			view_[i] = row;
			i++;
			start = ptr + 1;

			if (rowidx >= rows_num_) { /* fill dummy rows ... */
				for (; i < rows_per_page_; i++) {
					int16_t y = i * row_h_ + y_pad_;
					uint32_t bg = bg_;
					fill_rect(&bg, x_pad_, y, page_w_, row_h_);
				}

				xcb_flush(dpy_);
				break; /* ... and return */
			}

			if (i >= rows_per_page_)
				break; /* page done */
		}

		ptr++;
	}

	draw_search_bar();
}

static uint8_t match_col(const char *col, uint8_t len)
{
	uint8_t search_idx = search_idx_ - PROMPT_LEN;

	if (len < search_idx)
		return 0;

	if (memcmp(col, search_buf_ + PROMPT_LEN, search_idx) == 0)
		return 1;

	return 0;
}

static uint8_t find_col(const char *rowstr, uint8_t idx)
{
	const char *ptr = rowstr;
	const char *col_start = rowstr;
	uint8_t col_idx = 0;

	if (*ptr == '\a' && idx < cols_per_row_) /* if icon column */
		idx++;

	while (*ptr != '\n') {
		if (*ptr == '\t') {
			if (col_idx == idx) {
				if (match_col(col_start, ptr - col_start))
					return 1;
			}

			col_idx++;
			col_start = ptr + 1;
		}

		ptr++;
	}

	if (*ptr == '\n' && idx == cols_per_row_ - 1) {
		if (match_col(col_start, ptr - col_start))
			return 1;
	}

	return 0;
}

static void find_row(xcb_keysym_t sym)
{
	const char *ptr = data_;
	const char *row = ptr;
	const char *end = data_ + data_size_;
	uint8_t selidx = 0;
	uint8_t rowidx = 0;
	uint8_t pageidx = 0;
	uint8_t tab = 0;

	if (sym == 0x75 && control_) { /* control + u */
		search_idx_ = PROMPT_LEN;
		draw_search_bar();
		return;
	} else if (sym == XK_BackSpace) {
		if (search_idx_ > PROMPT_LEN)
			search_idx_--;
	} else if (sym == XK_Tab) {
		tab = 1;
	} else if ((sym & 0xff00) == 0xff00) { /* not alpha */
		dd("ignore sym 0x%x\n", sym);
		return;
	} else if (search_idx_ < sizeof(search_buf_)) {
		search_buf_[search_idx_++] = sym;
	}

	draw_search_bar();

	while (ptr < end) {
		if (find_col(row, search_col_idx_)) {
			if (!tab || found_idx_ < rowidx) {
				if (page_idx_ != pageidx) {
					page_idx_ = pageidx;
					selrow_ = NULL;
					selidx_ = selidx;
					draw_menu();
				} else {
					draw_row(selrow_, selidx_, 0);
					selrow_ = view_[selidx];
					draw_row(selrow_, selidx, 1);
					selidx_ = selidx;
				}

				found_idx_ = rowidx;
				return;
			}
		}

		if (*ptr == '\n') {
			rowidx++;
			row = ptr + 1;

			if (selidx++ >= rows_per_page_ - 1) {
				selidx = 0;
				if (pages_num_ > 1)
					pageidx++;
			}
		}

		ptr++;
	}

	found_idx_ = -1;
}

static void page_up(void)
{
	selidx_ = rows_per_page_ - 1;
	selrow_ = NULL;
	page_idx_--;
	draw_menu();
}

static void page_down(void)
{
	selidx_ = 0;
	selrow_ = NULL;
	page_idx_++;
	draw_menu();
}

#if 0
static void get_keysym(xcb_keycode_t code)
{
	xkb_keycode_t keycode;
	xkb_keysym_t keysym;

	keycode = code;
	keysym = xkb_state_key_get_one_sym(state_, keycode);

	char name[64];

	xkb_keysym_get_name(keysym, name, sizeof(name) - 1);

	size_t len;

	// First find the needed size; return value is the same as snprintf(3).
	len = xkb_state_key_get_utf8(state_, keycode, NULL, 0) + 1;

	if (len <= 1)
		return;

	char *buf = calloc(1, len);
	xkb_state_key_get_utf8(state_, keycode, buf, len);

	dd("keysym name '%s', utf len %u str '%s'\n", name, len, buf);
	free(buf);
}
#endif

static xcb_keysym_t get_keysyms(xcb_keycode_t code)
{
	const xkb_keysym_t *syms;
	int len;
	int i;

	if (!keymap_)
		return (xcb_keysym_t) 0;

	len = xkb_keymap_key_get_syms_by_level(keymap_, code, 0, level_, &syms);

#ifdef DEBUG
	for (i = 0; i < len; i++) {
		char name[64];
		const xkb_keysym_t sym = syms[i];
		xkb_keysym_get_name(sym, name, sizeof(name) - 1);
		dd("[%d] sym 0x%x name '%s'\n", i, sym, name);
	}
#endif

	if (len)
		return (xcb_keysym_t) syms[0];

	return (xcb_keysym_t) 0;
}

static void key_release(xcb_key_press_event_t *e)
{
	xcb_keysym_t sym;

	if (!syms_) {
		ww("key symbols not available\n");
		return;
	}

	sym = xcb_key_release_lookup_keysym(syms_, e, 0);

	if (sym == XK_Shift_L || sym == XK_Shift_R) {
		level_ = 0; /* expect lower-case symbols */
	} else if (sym == XKB_KEY_ISO_Level3_Shift) {
		level_ = 0;
	} else if (sym == XK_Control_L || sym == XK_Control_R) {
		control_ = 0; /* cancel command */
	}
}

static void key_press(xcb_key_press_event_t *e)
{
	xcb_keysym_t sym;

#ifdef DEBUG
	get_keysyms(e->detail);
#endif

	if (!syms_) {
		ww("key symbols not available\n");
		return;
	}

	sym = xcb_key_press_lookup_keysym(syms_, e, 0);

	if (sym == XK_Shift_L || sym == XK_Shift_R) {
		level_ = 1; /* expect upper-case symbols */
		return;
	} else if (sym == XKB_KEY_ISO_Level3_Shift) {
		/* FIXME: AltGr does not correspond level 3 on some systems */
		level_ = 2; /* expect AltGr symbols */
		return;
	} else if (sym == XK_Control_L || sym == XK_Control_R) {
		control_ = 1; /* expect command */
		return;
	} else if (sym == XK_Escape) {
		done_ = 1;
	} else if (sym == XK_Next && pages_num_ > 1 &&
		   page_idx_ < pages_num_ - 1) {
		page_down();
	} else if (sym == XK_Prior && pages_num_ > 1 && page_idx_ > 1) {
		page_up();
	} else if (sym == XK_Up) {
		if (selidx_ == 0 && page_idx_ == 0) {
			return;
		} else if (selidx_ == 0 && page_idx_ > 0) {
			page_up();
			return;
		}

		draw_row(selrow_, selidx_, 0);
		selidx_--;
		selrow_ = view_[selidx_];
		draw_row(selrow_, selidx_, 1);
	} else if (sym == XK_Down) {
		uint8_t rem = (selidx_ + rows_rem_) == rows_per_page_ - 1;

		if (page_idx_ == pages_num_ - 1 && rem) {
			return; /* last not full page */
		} else if (selidx_ == rows_per_page_ && page_idx_ == pages_num_) {
			return; /* last page */
		} else if (selidx_ == rows_per_page_ && !pages_num_) {
			return; /* single page */
		} else if (selidx_ == rows_per_page_ - 1) {
			page_down();
			return;
		}

		draw_row(selrow_, selidx_, 0);
		selidx_++;
		selrow_ = view_[selidx_];
		draw_row(selrow_, selidx_, 1);
	} else if (sym == XK_Right) {
		struct column *col = &selrow_->cols[0];
		search_idx_ = sizeof(search_buf_) - PROMPT_LEN - 2;

		if (col->len < search_idx_)
			search_idx_ = col->len + 2;

		memcpy(&search_buf_[PROMPT_LEN], col->str, search_idx_);
		draw_search_bar();
	} else if (sym == XK_Return) {
		char *str = selrow_->str;
		*(str + selrow_->len) = '\0';
		printf("%s", str);

		if (append_ && search_idx_) {
			search_buf_[search_idx_] = '\0';
			printf("\t%s\n", &search_buf_[PROMPT_LEN]);
		} else {
			printf("\n");
		}

		done_ = 1;
	} else {
		if ((sym = get_keysyms(e->detail)))
			find_row(sym);
	}

	follow_ = 0;
}

static void follow_pointer(xcb_motion_notify_event_t *e)
{
	uint8_t i;
	xcb_query_pointer_cookie_t c;
	xcb_query_pointer_reply_t *r;
	int16_t x;
	uint8_t rows_num;

	if (pages_num_ && page_idx_ == pages_num_ - 1)
		rows_num = rows_per_page_ - rows_rem_;
	else
		rows_num = rows_per_page_;

	for (i = 0; i < rows_num; i++) {
		int16_t lolim = i * row_h_ + y_pad_;
		int16_t uplim = lolim + row_h_;

		if (e->event_y >= lolim && e->event_y <= uplim) {
			struct row *row = view_[i];

			if (selrow_ != row) {
				draw_row(selrow_, selidx_, 0);
				selidx_ = i;
				selrow_ = row;
				draw_row(selrow_, selidx_, 1);
			}

			break;
		}
	}

	if (!pages_num_)
		return;

	c = xcb_query_pointer(dpy_, win_);
	r = xcb_query_pointer_reply(dpy_, c, NULL);

	if (!r) {
		x = 0;
	} else {
		x = r->win_x;
		free(r);
	}

	if (e->event_y >= page_h_ + y_pad_) { /* follow down */
		if (page_idx_ >= pages_num_ - 1)
			return;

		xcb_warp_pointer(dpy_, XCB_NONE, win_, 0, 0,
				 0, 0, x, y_pad_);
		page_down();
	} else if (e->event_y < y_pad_) { /* follow up */
		if (!page_idx_)
			return;

		xcb_warp_pointer(dpy_, XCB_NONE, win_, 0, 0,
				 0, 0, x, page_h_ - y_pad_);
		page_up();
	}
}

static void warp_pointer(void)
{
	xcb_grab_pointer(dpy_, XCB_NONE, win_,
			 XCB_EVENT_MASK_POINTER_MOTION |
			 XCB_EVENT_MASK_BUTTON_PRESS |
			 XCB_EVENT_MASK_BUTTON_RELEASE,
			 XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
			 win_, XCB_NONE, XCB_CURRENT_TIME);
	xcb_warp_pointer_checked(dpy_, XCB_NONE, win_, 0, 0, 0, 0, 2 * x_pad_,
				 2 * y_pad_);
	xcb_flush(dpy_);
	return;
}

static void adjust_width(char *row, uint8_t len)
{
	uint16_t w;
	uint16_t h;
	uint8_t i;
	uint8_t acc = 0;

	for (i = 0; i < cols_per_row_ - 1; i++) {
		acc += cols_len_[i];
	}

	acc = row_len_ - acc;
	text_size(row, acc, &w, &h);
	cols_len_[i] = acc;
	cols_px_[i] = w;
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

	dd("loaded font %s size %f\n", info, size);
	return font;
}

static int init_rows(void)
{
	uint16_t i;
	char *ptr;
	char *col_start;
	char *row_start;
	char *longest_row;
	char *end = data_ + data_size_;
	uint16_t w;
	uint16_t h;
	uint8_t col_max_len;
	uint8_t row_max_len;

	x_pad_ = font_->max_advance_width / 2;
	y_pad_ = x_pad_;
	row_h_ = font_->ascent + font_->descent + 2 * FONT_H_MARGIN;
	text_y_ = font_->ascent + FONT_H_MARGIN;
	ptr = data_;

	while (ptr < end) { /* calc number of items in row */
		if (*ptr == '\t') {
			cols_per_row_++;
		} else if (*ptr == '\n') {
			cols_per_row_++;
			break;
		}

		ptr++;
	}

	if (*ptr != '\n') {
		errno = 0;
		ee("end of line not found\n");
		return -1;
	}

	if (!cols_per_row_) {
		ee("failed to find number of items in row\n");
		return -1;
	}

	cols_len_ = calloc(sizeof(*cols_len_), cols_per_row_);

	if (!cols_len_) {
		ee("calloc(%lu) failed\n", sizeof(*cols_len_) * cols_per_row_);
		return -1;
	}

	cols_px_ = calloc(sizeof(*cols_px_), cols_per_row_);

	if (!cols_px_) {
		ee("calloc(%lu) failed\n", sizeof(*cols_px_) * cols_per_row_);
		return -1;
	}

	if (search_col_idx_ >= cols_per_row_)
		search_col_idx_ = 0;

	col_max_len = row_len_ / cols_per_row_;
	dd("max col len %u\n", col_max_len);
	ptr = data_;
	col_start = ptr;
	row_start = ptr;
	longest_row = data_;
	i = 0;
	row_max_len = 0;
	pages_[0].rowptr = data_;
	pages_[0].rowidx = 0;

	while (ptr < end) { /* get everything done in one pass */
		if (*ptr == '\t' || *ptr == '\n') {
			cols_len_[i] = ptr - col_start;
			text_size(col_start, cols_len_[i], &w, &h);

			if (w > cols_px_[i])
				cols_px_[i] = w;

			if (*ptr == '\t') {
				i++;
			} else if (*ptr == '\n') {
				rows_num_++;
				if (i == cols_per_row_ - 1) {
					uint8_t len = ptr - col_start;

					if (w > cols_px_[i])
						cols_px_[i] = w;

					len = ptr - row_start;

					if (len > row_max_len) {
						row_max_len = len;
						longest_row = row_start;
					}

					row_start = ptr + 1;
					i = 0; /* reset lengths counter */

					uint8_t page = rows_num_ / rows_per_page_;

					if (!pages_[page].rowptr) {
						pages_[page].rowptr = row_start;
						pages_[page].rowidx = rows_num_;
					}
				}
			}

			col_start = ptr + 1;
		}

		ptr++;
	}

	if (!longest_row) {
		errno = 0;
		ee("file parse error, longest row not found\n");
		return -1;
	}

	pages_[UCHAR_MAX].rowptr = end;
	pages_[UCHAR_MAX].rowidx = 0;
	adjust_width(longest_row, row_max_len);

	if (!rows_num_) {
		if (cols_per_row_) {
			rows_num_ = 1;
		} else {
			errno = 0;
			ee("failed to detect number of rows\n");
			return -1;
		}
	}

	if (rows_per_page_ > rows_num_)
		rows_per_page_ = rows_num_;

	dd("items per row %u page cols %u page rows %u/%u\n",
	   cols_per_row_, row_len_, rows_per_page_, rows_num_);

	rows_ = calloc(sizeof(*rows_), rows_num_);

	if (!rows_) {
		ee("calloc(%lu) failed\n", sizeof(*rows_) * rows_num_);
		return -1;
	}

	view_ = calloc(sizeof(*view_), rows_per_page_);

	if (!view_) {
		ee("calloc(%lu) failed\n", sizeof(*view_) * rows_per_page_);
		return -1;
	}

	for (i = 0; i < rows_num_; i++) {
		struct row *row = &rows_[i];

		row->cols = calloc(sizeof(*row->cols), cols_per_row_);

		if (!row->cols) {
			ee("calloc(%lu) failed\n",
			   sizeof(*row->cols) * cols_per_row_);
			return -1;
		}
	}

	pages_num_ = rows_num_ / rows_per_page_;

	if (rows_num_ % rows_per_page_)
		pages_num_++;

	rows_rem_ = pages_num_ * rows_per_page_ - rows_num_;
	page_w_ = 0;

	for (i = 0; i < cols_per_row_; i++) {
		cols_px_[i] += x_pad_;
		page_w_ += cols_px_[i];
	}

	dd("text y: %u row width %u pages num %u\n", text_y_, page_w_,
		pages_num_);

	return 0;
}

static int init_xkb(void)
{
	int32_t dev;

	if (!(xkb_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS))) {
		ee("xkb_context_new() failed\n");
		return -1;
	}

	if ((dev = xkb_x11_get_core_keyboard_device_id(dpy_)) < 0) {
		ee("xkb_x11_get_core_keyboard_device_id() failed\n");
		return -1;
	}

	keymap_ = xkb_x11_keymap_new_from_device(xkb_, dpy_, dev,
						 XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!keymap_) {
		ee("xkb_x11_keymap_new_from_device() failed\n");
		return -1;
	}

	if (!(state_ = xkb_x11_state_new_from_device(keymap_, dpy_, dev))) {
		ee("xkb_x11_state_new_from_device() failed\n");
		return -1;
	}

	return 0;
}

static int init_menu(void)
{
	int fd;
	struct stat st;

	if ((fd = open(path_, O_RDONLY)) < 0) {
		ee("open(%s) failed\n", path_);
		return -1;
	}

	if (fstat(fd, &st) < 0) {
		ee("fstat(%s) failed\n", path_);
		goto err;
	}

#define MMAP_PROT (PROT_READ | PROT_WRITE)

	data_ = mmap(NULL, st.st_size, MMAP_PROT, MAP_PRIVATE, fd, 0);

	if (data_ == MAP_FAILED) {
		ee("mmap(%s) failed\n", path_);
		data_ = NULL;
		goto err;
	}

	data_size_ = st.st_size;

	if (init_rows() == 0)
		return fd;

	/* skip rows cleanup in init just unmap memory */

	munmap(data_, data_size_);
err:
	data_ = NULL;
	close(fd);
	return -1;
}

static void help(const char *name)
{
	ii("Usage: %s [options] <file>\n"
	   "\nOptions:\n"
	   "  -h, --help                   print this message\n"
	   "  -c, --cols <width>           menu width in characters\n"
	   "  -r, --rows <rows>            menu height in rows\n"
	   "  -f, --font <font>            menu font (%s:%f)\n"
	   "  -i, --icon <font>            font for icon columns (%s:%f)\n"
	   "  -s, --search-column <index>  search column (shown first)\n"
	   "  -n, --name <name>            window name and class (def '%s')\n"
	   "  -d, --delineate              alternate color from row to row\n"
	   "  -a, --append                 append entered text to result\n"
	   "  -b, --search-bar             show search bar\n"
	   "  -0, --normalfg <hex>         rgb color, default 0x%x\n"
	   "  -1, --normalbg <hex>         rgb color, default 0x%x\n"
	   "  -2, --activefg <hex>         rgb color, default 0x%x\n"
	   "  -3, --activebg <hex>         rgb color, default 0x%x\n"
	   "\nFile format:\n"
	   "  tab-separated values (max cols %u, max rows %u)\n"
	   "  font icon columns start with '\\a'\n"
	   "\nKey bindings:\n"
	   "  Down/Up    navigate rows\n"
	   "  PgDn/PgUp  navigate pages\n"
	   "  Tab        goto next row matching search pattern\n"
	   "  Right      copy 1st column of selected row to search bar\n"
	   "  Backspace  delete character before cursor in search bar\n"
	   "  Ctrl-u     clear search bar\n"
	   "  Return     print selected row to standard output\n"
	   "  Esc        exit without result\n\n",
	   name, font1_name_, font1_size_, font2_name_, font2_size_, name_,
	   fg_, bg_, selfg_, selbg_, UCHAR_MAX, INT16_MAX);
}

static int opt(const char *arg, const char *args, const char *argl)
{
	return (strcmp(arg, args) == 0 || strcmp(arg, argl) == 0);
}

static void opts(int argc, char *argv[])
{
	int i;

	if (argc < 2) {
		help(argv[0]);
		exit(0);
	}

	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];

		if (opt(arg, "-h", "--help")) {
			help(argv[0]);
			exit(0);
		} else if (opt(arg, "-c", "--cols")) {
			i++;
			row_len_ = atoi(argv[i]);
		} else if (opt(arg, "-r", "--rows")) {
			i++;
			rows_per_page_ = atoi(argv[i]);
		} else if (opt(arg, "-f", "--font")) {
			i++;
			font1_name_ = argv[i];
		} else if (opt(arg, "-i", "--icon")) {
			i++;
			font2_name_ = argv[i];
		} else if (opt(arg, "-s", "--search-column")) {
			i++;
			search_col_idx_ = atoi(argv[i]);
			swap_col_idx_ = search_col_idx_;
		} else if (opt(arg, "-n", "--name")) {
			i++;
			name_ = argv[i];
		} else if (opt(arg, "-d", "--delineate")) {
			interlace_ = 1;
		} else if (opt(arg, "-a", "--append")) {
			append_ = 1;
		} else if (opt(arg, "-b", "--search-bar")) {
			search_bar_ = 1;
		} else if (opt(arg, "-0", "--normal-foreground")) {
			i++;
			if (argv[i])
				fg_ = strtol(argv[i], NULL, 16);
			fg_ &= 0xffffff;
		} else if (opt(arg, "-1", "--normal-background")) {
			i++;
			if (argv[i])
				bg_ = strtol(argv[i], NULL, 16);
			bg_ &= 0xffffff;
		} else if (opt(arg, "-2", "--active-foreground")) {
			i++;
			if (argv[i])
				selfg_ = strtol(argv[i], NULL, 16);
			selfg_ &= 0xffffff;
		} else if (opt(arg, "-3", "--active-background")) {
			i++;
			if (argv[i])
				selbg_ = strtol(argv[i], NULL, 16);
			selbg_ &= 0xffffff;
		}
	}

	if (!search_bar_)
		append_ = 0;

	if (!(path_ = argv[i - 1])) {
		ee("path is not set\n");
		help(argv[0]);
		exit(0);
	}
}

static xcb_atom_t get_atom(const char *str, uint8_t len)
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

#ifdef STAY_OBSCURED
static void wait(void)
{
	int state;
	xcb_generic_event_t *e;

	while (1) {
		e = xcb_wait_for_event(dpy_);
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
#endif

static uint8_t events(uint8_t wait)
{
	xcb_generic_event_t *e;

	if (wait)
		e = xcb_wait_for_event(dpy_);
	else
		e = xcb_poll_for_event(dpy_);

	if (!e)
		return 0;

	switch (e->response_type & ~0x80) {
	case XCB_VISIBILITY_NOTIFY:
		switch (((xcb_visibility_notify_event_t *) e)->state) {
		case XCB_VISIBILITY_PARTIALLY_OBSCURED: /* fall through */
			dd("XCB_VISIBILITY_PARTIALLY_OBSCURED\n");
		case XCB_VISIBILITY_FULLY_OBSCURED:
			dd("XCB_VISIBILITY_FULLY_OBSCURED\n");
#ifdef STAY_OBSCURED
			wait();
#else
			done_ = 1;
#endif
			break;
		case XCB_VISIBILITY_UNOBSCURED:
			dd("XCB_VISIBILITY_UNOBSCURED\n");
			break;
		}
		break;
	case XCB_UNMAP_NOTIFY:
		dd("XCB_UNMAP_NOTIFY\n");
		done_ = 1;
		break;
	case XCB_DESTROY_NOTIFY:
		dd("XCB_DESTROY_NOTIFY\n");
		done_ = 1;
		break;
	case XCB_LEAVE_NOTIFY:
		dd("XCB_LEAVE_NOTIFY\n");
		done_ = 1;
		break;
	case XCB_EXPOSE:
		dd("XCB_EXPOSE\n");
		draw_menu();
		warp_pointer();
		break;
	case XCB_KEY_PRESS:
		key_press((xcb_key_press_event_t *) e);
		break;
	case XCB_KEY_RELEASE:
		key_release((xcb_key_press_event_t *) e);
		break;
	case XCB_BUTTON_PRESS:
		dd("XCB_BUTTON_PRESS\n");
		char *str = selrow_->str;
		*(str + selrow_->len) = '\0';
		printf("%s\n", str);
		done_ = 1;
		break;
	case XCB_MOTION_NOTIFY:
		follow_pointer((xcb_motion_notify_event_t *) e);
		break;
	default:
		dd("event %d (%d)\n", e->response_type & ~0x80, e->response_type);
	}

	free(e);
	return 1;
}

int main(int argc, char *argv[])
{
	struct pollfd pfd;
	int fd;
	uint32_t mask;
	xcb_screen_t *scr;
	uint32_t val[2];
	XRenderColor ref;

	opts(argc, argv);

	xdpy_ = XOpenDisplay(NULL);

	if (!xdpy_) {
		ee("XOpenDisplay() failed\n");
		goto out;
	}

	xscr_ = DefaultScreen(xdpy_);
	dpy_ = XGetXCBConnection(xdpy_);

	if (!dpy_) {
		ee("xcb_connect() failed\n");
		goto out;
	}

	if (!(font1_ = load_font(font1_name_, font1_size_))) {
		ee("XftFontOpen(%s) failed\n", font1_name_);
		goto out;
	}

	font_ = font1_; /* default */

	if (!(font2_ = load_font(font2_name_, font2_size_))) {
		ee("XftFontOpen(%s) failed\n", font2_name_);
		/* ignore error */
		font2_ = font1_;
	}

	dd("font: ascent %d descent %d height %d max width %d\n",
	       font_->ascent, font_->descent, font_->height,
	       font_->max_advance_width);

	ref.alpha = 0xffff;
	ref.red = (fg_ & 0xff0000) >> 8;
	ref.green = fg_ & 0xff00;
	ref.blue = (fg_ & 0xff) << 8;
	XftColorAllocValue(xdpy_, DefaultVisual(xdpy_, xscr_),
			DefaultColormap(xdpy_, xscr_), &ref,
			&fg_xft_);

	ref.alpha = 0xffff;
	ref.red = (selfg_ & 0xff0000) >> 8;
	ref.green = selfg_ & 0xff00;
	ref.blue = (selfg_ & 0xff) << 8;
	XftColorAllocValue(xdpy_, DefaultVisual(xdpy_, xscr_),
			DefaultColormap(xdpy_, xscr_), &ref,
			&selfg_xft_);

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
		goto out;
	}

	if ((fd = init_menu()) < 0)
		goto out;

	dd("padding %u,%u\n", x_pad_, y_pad_);

	page_h_ = row_h_ * rows_per_page_;

	dd("menu %ux%u cols %u rows %u/%u glyphs per row %u row height %u\n",
	   page_w_, page_h_, row_len_, rows_per_page_, rows_num_,
	   page_w_ / x_pad_, row_h_);

	mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	val[0] = bg_;
	val[1] = XCB_EVENT_MASK_EXPOSURE;
	val[1] |= XCB_EVENT_MASK_VISIBILITY_CHANGE;
	val[1] |= XCB_EVENT_MASK_KEY_PRESS;
	val[1] |= XCB_EVENT_MASK_KEY_RELEASE;
	val[1] |= XCB_EVENT_MASK_BUTTON_PRESS;
	val[1] |= XCB_EVENT_MASK_POINTER_MOTION;
	val[1] |= XCB_EVENT_MASK_LEAVE_WINDOW;

	uint16_t tmp;
	search_bar_ ? (tmp = row_h_) : (tmp = 0);

	xcb_create_window(dpy_, XCB_COPY_FROM_PARENT, win_, scr->root, 0, 0,
			  page_w_ + 2 * x_pad_, page_h_ + 2 * y_pad_ + tmp,
			  0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  scr->root_visual, mask, val);

        xcb_change_property(dpy_, XCB_PROP_MODE_REPLACE, win_,
			    XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                            strlen(name_), name_);

        xcb_change_property(dpy_, XCB_PROP_MODE_REPLACE, win_,
			    XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
                            strlen(name_), name_);

	xcb_atom_t atom = get_atom("_NET_WM_PID", sizeof("_NET_WM_PID") - 1);
	uint32_t data = getpid();
	xcb_change_property_checked(dpy_, XCB_PROP_MODE_REPLACE, win_,
				    atom, XCB_ATOM_CARDINAL, 32, 1, &data);

	xcb_map_window(dpy_, win_);
	xcb_flush(dpy_);

	if (!(syms_ = xcb_key_symbols_alloc(dpy_)))
		ww("xcb_key_symbols_alloc() failed\n");

	init_xkb();

	while (!done_)
		events(1);

	/* DUNNO: this trick is needed to deliver events in case another
	 * menu window grabs pointer
	 */

	pfd.fd = xcb_get_file_descriptor(dpy_);
	pfd.events = POLLIN;
	pfd.revents = 0;

	while (!done_) {
		errno = 0;
		poll(&pfd, 1, -1);

		if (errno == EINTR)
			continue;

		if (pfd.revents & POLLIN)
			while (events(0)) {};

		pfd.revents = 0;
	}

out:
	if (state_)
		xkb_state_unref(state_);

	if (keymap_)
		xkb_keymap_unref(keymap_);

	if (xkb_)
		xkb_context_unref(xkb_);

	if (draw_)
		XftDrawDestroy(draw_);

	if (xdpy_) {
		if (font1_)
			XftFontClose(xdpy_, font1_);
		if (font2_ && font2_ != font1_)
			XftFontClose(xdpy_, font2_);
	}

	if (win_ != XCB_WINDOW_NONE) {
		xcb_destroy_window(dpy_, win_);
		xcb_flush(dpy_);
	}

	if (data_)
		munmap(data_, data_size_);

	close(fd);
	return 0;
}
