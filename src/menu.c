/* menu.c: rudimentary menu dialog
 *
 * Copyright (c) 2015, Aliaksei Katovich <aliaksei.katovich at gmail.com>
 *
 * Released under the GNU General Public License, version 2
 */

#define USE_CRC32
#include "misc.h"
#include "text.h"
#include "fwm.h"

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

#define XK_MISCELLANY
#include <X11/keysymdef.h>

#ifndef UCHAR_MAX
#define UCHAR_MAX 255
#endif

#ifdef DEBUG
#undef dd
#define dd(...) fprintf(stderr, __VA_ARGS__)
#else /* ! DEBUG */
#undef dd
#define dd(...) ;
#endif /* DEBUG */

#define FONT_H_MARGIN 2
#define COLOR_DISTANCE 10

#define PROMPT_LEN 2

#define DEFAULT_FONT_SIZE 10.5
#define DEFAULT_DPI 96

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
	const char *path;
	xcb_drawable_t win;
	xcb_gcontext_t gc;
	xcb_key_symbols_t *syms;
	struct xkb_context *xkb;
	struct xkb_keymap *keymap;
	uint16_t w;
	uint16_t h;
	uint32_t fg;
	uint32_t bg;
	uint32_t selfg;
	uint32_t selbg;
	uint32_t curbg;
	uint8_t done;
	uint16_t hdpi;
	uint16_t vdpi;
	float font_size;
	const char *text_font;
	const char *icon_font;
	int16_t text_y;
	int16_t icon_y;
	uint16_t text_h;
	uint16_t row_h;
	uint8_t space_w;
};

static struct ctx ctx_;
static struct text_info text_;
static struct text_info icon_;

static uint8_t warp_ = 1;

static char search_buf_[UCHAR_MAX];
static uint8_t search_idx_ = PROMPT_LEN; /* '> ' */
static int16_t found_idx_;

static uint16_t page_w_;
static uint16_t page_h_;

static uint8_t row_len_; /* characters */
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
static uint8_t print_input_;
static uint8_t interlace_;

#ifdef DEBUG
static struct xkb_state *state_;
#endif
static uint8_t level_;
static uint8_t control_;

static uint16_t x_pad_;
static uint16_t y_pad_;

struct column {
	char *str;
	uint8_t len;
	struct text *text;
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

static void get_space_width(fontid_t font_id)
{
	uint16_t w;
	uint16_t h;
	struct text *text = create_text();

	if (!text)
		return;

	set_text_font(text, font_id, ctx_.font_size);

	set_text_str(text, "=", sizeof("="));
	get_text_size(text, &w, &h);
	ctx_.space_w = w;

	set_text_str(text, "|", sizeof("|"));
	get_text_size(text, &w, &h);
	ctx_.text_h = h;

	destroy_text(&text);
}

static void fill_rect(uint32_t c, int16_t x, int16_t y, uint16_t w, uint16_t h)
{
	xcb_rectangle_t rect = { x, y, w, h, };
	xcb_change_gc(ctx_.dpy, ctx_.gc, XCB_GC_FOREGROUND, &c);
	xcb_poly_fill_rectangle(ctx_.dpy, ctx_.win, ctx_.gc, 1, &rect);
	xcb_flush(ctx_.dpy);
}

static int16_t draw_rect(uint8_t idx, uint8_t focus)
{
	int16_t y = idx * ctx_.row_h + y_pad_;
	uint32_t bg;

	focus ? (bg = ctx_.selbg) : (bg = ctx_.bg);

	if (interlace_ && !focus && idx % 2) {
		bg = ((((bg >> 16) & 0xff) + COLOR_DISTANCE) << 16) |
		     ((((bg >> 8) & 0xff) + COLOR_DISTANCE) << 8) |
		     ((bg & 0xff) + COLOR_DISTANCE);
	}

	fill_rect(bg, x_pad_, y, page_w_, ctx_.row_h);
	ctx_.curbg = bg;
	return y;
}

static int16_t draw_col(struct column *col, uint8_t i, int16_t x, int16_t y,
 uint8_t focus)
{
	uint16_t w;
	uint16_t h;
	uint32_t fg;
	uint32_t bg;
	uint8_t len;
	struct xcb xcb = { ctx_.dpy, ctx_.win, ctx_.gc };

	if (!col->str || !col->len)
		return 0;
	else if (*col->str == '\a') {
		col->str++;
		col->len--;
	}

	if (focus) {
		fg = ctx_.selfg;
		bg = ctx_.selbg;
	} else {
		fg = ctx_.fg;
		bg = ctx_.bg;
	}

	set_text_str(col->text, col->str, col->len);
	get_text_size(col->text, &w, &h);

	if (w > cols_px_[i])
		len = cols_len_[i];
	else
		len = col->len;

	set_text_str(col->text, col->str, len);
	set_text_pos(col->text, x, y);
	set_text_color(col->text, fg, bg);
	draw_text_xcb(&xcb, col->text);

	return x + w + x_pad_;
}

static void draw_search_bar(void)
{
	struct xcb xcb = { ctx_.dpy, ctx_.win, ctx_.gc };
	int16_t x = x_pad_ * 2;
	int16_t y = draw_rect(rows_per_page_, 0) + ctx_.text_y;
	uint8_t len = search_idx_ + 1;
	char *str = search_buf_;

	search_buf_[0] = '>';
	search_buf_[1] = ' ';
	search_buf_[search_idx_] = '_';

	set_text_str(text_.txt, str, len);
	set_text_pos(text_.txt, x, y);
	set_text_color(text_.txt, ctx_.fg, ctx_.curbg);
	draw_text_xcb(&xcb, text_.txt);
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

static void warp_pointer(int16_t x, int16_t y)
{
	if (warp_) {
		xcb_warp_pointer(ctx_.dpy, XCB_NONE, ctx_.win, 0, 0, 0, 0, x, y);
		xcb_flush(ctx_.dpy);
		warp_ = 0;
	}
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

	col->str = start;
	col->text = text_.txt;

	while (ptr < end) {
		if (*ptr == '\a') {
			col->text = icon_.txt;
		} else if (*ptr == '\t' || *ptr == '\n') {
			col->len = ptr - start;
			col++;
			start = ptr + 1;
			col->str = start;
			col->text = text_.txt;
		}

		ptr++;
	}

	if (ptr == end)
		col->len = ptr - start;

	y = draw_rect(idx, focus) + ctx_.text_y;
	x = x_pad_ * 2;

	if (swap_col_idx_)
		swap_cols(row, 0, swap_col_idx_);

	for (i = 0; i < cols_per_row_; i++) {
		int16_t start_y = y;

		if (row->cols[i].text == icon_.txt)
			start_y--; /* HACK: icon is better aligned ths way */

		draw_col(&row->cols[i], i, x, start_y, focus);
		x += cols_px_[i];
	}

	if (focus)
		warp_pointer(page_w_ + x_pad_, y);
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
					int16_t y = i * ctx_.row_h + y_pad_;
					fill_rect(ctx_.bg, x_pad_, y, page_w_,
					 ctx_.row_h);
				}

				break; /* ... and return */
			}

			if (i >= rows_per_page_)
				break; /* page done */
		}

		ptr++;
	}

	if (search_bar_)
		draw_search_bar();

	warp_pointer(page_w_ + x_pad_, ctx_.row_h - y_pad_);
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

	if (search_bar_)
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

static void line_up(void)
{
	if (selidx_ == 0 && page_idx_ == 0) {
		return;
	} else if (selidx_ == 0 && page_idx_ > 0) {
		page_up();
		return;
	}

	draw_row(selrow_, selidx_, 0);
	selidx_--;
	selrow_ = view_[selidx_];
	warp_ = 1;
	draw_row(selrow_, selidx_, 1);
}

static void line_down(void)
{
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
	warp_ = 1;
	draw_row(selrow_, selidx_, 1);
}

static xcb_keysym_t get_keysyms(xcb_keycode_t code)
{
	const xkb_keysym_t *syms;
	int len;

	if (!ctx_.keymap)
		return (xcb_keysym_t) 0;

	len = xkb_keymap_key_get_syms_by_level(ctx_.keymap, code, 0, level_,
	 &syms);

#ifdef DEBUG
	int i;
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

	if (!ctx_.syms) {
		ww("key symbols not available\n");
		return;
	}

	sym = xcb_key_release_lookup_keysym(ctx_.syms, e, 0);

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

	if (!ctx_.syms) {
		ww("key symbols not available\n");
		return;
	}

	sym = xcb_key_press_lookup_keysym(ctx_.syms, e, 0);

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
		ctx_.done = 1;
	} else if (sym == XK_Next && pages_num_ > 1 &&
		   page_idx_ < pages_num_ - 1) {
		page_down();
	} else if (sym == XK_Prior && pages_num_ > 1 && page_idx_ > 1) {
		page_up();
	} else if (sym == XK_Up) {
		line_up();
	} else if (sym == XK_Down) {
		line_down();
	} else if (sym == XK_Right) {
		struct column *col = &selrow_->cols[0];
		search_idx_ = sizeof(search_buf_) - PROMPT_LEN - 2;

		if (col->len < search_idx_)
			search_idx_ = col->len + 2;

		memcpy(&search_buf_[PROMPT_LEN], col->str, search_idx_);
		if (search_bar_)
			draw_search_bar();
	} else if (sym == XK_Return) {
		char *str = selrow_->str;
		*(str + selrow_->len) = '\0';

		if (!print_input_ && !append_) {
			printf("%s\n", str);
		} else if (print_input_ && !append_) {
			fprintf(stderr, "%s:%d\n", __func__, __LINE__);
			search_buf_[search_idx_] = '\0';
			printf("%s\n", &search_buf_[PROMPT_LEN]);
		} else if (search_idx_) {
			fprintf(stderr, "%s:%d\n", __func__, __LINE__);
			search_buf_[search_idx_] = '\0';
			printf("%s\t%s\n", str, &search_buf_[PROMPT_LEN]);
		} else {
			fprintf(stderr, "%s:%d\n", __func__, __LINE__);
			printf("\n");
		}

		ctx_.done = 1;
	} else {
		if ((sym = get_keysyms(e->detail))) {
			warp_ = 1;
			find_row(sym);
		}
	}

	follow_ = 0;
}

static void follow_pointer(xcb_motion_notify_event_t *e)
{
	uint8_t i;
	uint8_t rows_num;

	if (pages_num_ && page_idx_ == pages_num_ - 1)
		rows_num = rows_per_page_ - rows_rem_;
	else
		rows_num = rows_per_page_;

	for (i = 0; i < rows_num; i++) {
		int16_t lolim = i * ctx_.row_h + y_pad_;
		int16_t uplim = lolim + ctx_.row_h;

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

	if (e->event_y >= page_h_ + y_pad_) { /* follow down */
		if (page_idx_ >= pages_num_ - 1)
			return;

		warp_pointer(page_w_ + x_pad_, y_pad_);
		page_down();
	} else if (e->event_y < y_pad_) { /* follow up */
		if (!page_idx_)
			return;

		warp_pointer(page_w_ + x_pad_, page_h_ - y_pad_);
		page_up();
	}
}

static void grab_pointer(void)
{
	xcb_grab_pointer(ctx_.dpy, XCB_NONE, ctx_.win,
			 XCB_EVENT_MASK_POINTER_MOTION |
			 XCB_EVENT_MASK_BUTTON_PRESS |
			 XCB_EVENT_MASK_BUTTON_RELEASE,
			 XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
			 ctx_.win, XCB_NONE, XCB_CURRENT_TIME);
	xcb_flush(ctx_.dpy);
	return;
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
	uint8_t row_max_len;
	uint8_t icon;
	struct text *text;

	x_pad_ = ctx_.space_w;
	y_pad_ = x_pad_;
	ctx_.row_h = ctx_.text_h + 2 * (ctx_.space_w / 3);
	ctx_.text_y = (ctx_.row_h - ctx_.text_h) / 2;
	ctx_.icon_y = ctx_.text_y;

	if (!(ctx_.text_y % 2))
		ctx_.text_y++;

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

	if (search_col_idx_ >= cols_per_row_) {
		search_col_idx_ = 0;
		swap_col_idx_ = 0;
	}

#ifdef DEBUG
	uint8_t col_max_len = row_len_ / cols_per_row_;
	dd("max col len %u\n", col_max_len);
#endif
	ptr = data_;
	col_start = ptr;
	row_start = ptr;
	longest_row = data_;
	i = 0;
	row_max_len = 0;
	pages_[0].rowptr = data_;
	pages_[0].rowidx = 0;

	icon = 0;
	text = text_.txt;

	while (ptr < end) { /* get everything done in one pass */
		if (*ptr == '\a') {
			icon = 1;
		} else if (*ptr == '\t' || *ptr == '\n') {
			if (!icon) {
				text = text_.txt;
			} else {
				text = icon_.txt;
				col_start++;
				icon = 0;
			}

			cols_len_[i] = ptr - col_start + 1;
			set_text_str(text, col_start, cols_len_[i]);
			get_text_size(text, &w, &h);

			if (w > cols_px_[i])
				cols_px_[i] = w;

			if (*ptr == '\t') {
				if (++i >= cols_per_row_) {
					ww("excpected %u columns in row %u\n",
					   cols_per_row_, rows_num_ + 1);
					break;
				}
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

#ifdef DEBUG
	char tmp[256] = {0};
	snprintf(tmp, sizeof(tmp), "%s", longest_row);
	dd("longstr: '%s'\n", tmp);
#endif

	dd("x pad: %u row len: %u str: '%s'\n", x_pad_, row_len_, longest_row);

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

	if (row_len_ > row_max_len) {
		uint16_t diff = page_w_ - cols_px_[i - 1];
		page_w_ += (row_len_ - row_max_len) * ctx_.space_w;
		cols_px_[i - 1] = page_w_ - diff;
	}

	dd("text y: %u row width %u pages num %u\n", ctx_.text_y, page_w_,
	   pages_num_);

	return 0;
}

static int init_xkb(void)
{
	int32_t dev;

	if (!(ctx_.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS))) {
		ee("xkb_context_new() failed\n");
		return -1;
	}

	if ((dev = xkb_x11_get_core_keyboard_device_id(ctx_.dpy)) < 0) {
		ee("xkb_x11_get_core_keyboard_device_id() failed\n");
		return -1;
	}

	ctx_.keymap = xkb_x11_keymap_new_from_device(ctx_.xkb, ctx_.dpy, dev,
	 XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!ctx_.keymap) {
		ee("xkb_x11_keymap_new_from_device() failed\n");
		return -1;
	}

#ifdef DEBUG
	dd("DEBUG %s:%d\n", __func__, __LINE__);
	state_ = xkb_x11_state_new_from_device(ctx_.keymap, ctx_.dpy, dev);
	if (!state_) {
		ee("xkb_x11_state_new_from_device() failed\n");
		return -1;
	}
#endif

	return 0;
}

static int init_menu(void)
{
	int fd;
	struct stat st;

	if ((fd = open(ctx_.path, O_RDONLY)) < 0) {
		ee("open(%s) failed\n", ctx_.path);
		return -1;
	}

	if (fstat(fd, &st) < 0) {
		ee("fstat(%s) failed\n", ctx_.path);
		goto err;
	}

#define MMAP_PROT (PROT_READ | PROT_WRITE)

	data_ = mmap(NULL, st.st_size, MMAP_PROT, MAP_PRIVATE, fd, 0);

	if (data_ == MAP_FAILED) {
		ee("mmap(%s) failed\n", ctx_.path);
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

static uint8_t init_text(struct text_info *text)
{
	if (!(text->txt = create_text()))
		return 0;

	set_text_font(text->txt, text->font_id, ctx_.font_size);
	return 1;
}

static void help(const char *name)
{
	printf("Usage: %s [options] <file>\n"
	 "\nOptions:\n"
	 "  -h, --help                   print this message\n"
	 "  -c, --cols <width>           menu width in characters\n"
	 "  -r, --rows <rows>            menu height in rows\n"
	 "  -s, --search-column <index>  search column (shown first)\n"
	 "  -n, --name <name>            window name and class (def '%s')\n"
	 "  -d, --delineate              alternate color from row to row\n"
	 "  -a, --append                 append entered text to result\n"
	 "  -p, --print-input            only print entered text\n"
	 "  -b, --search-bar             show search bar\n"
	 "  -0, --normalfg <hex>         rgb color, default 0x%x\n"
	 "  -1, --normalbg <hex>         rgb color, default 0x%x\n"
	 "  -2, --activefg <hex>         rgb color, default 0x%x\n"
	 "  -3, --activebg <hex>         rgb color, default 0x%x\n"
	 "\nFile format:\n"
	 "  Tab-separated values (max cols %u, max rows %u)\n"
	 "  Font icon columns start with '\\a'\n"
	 "\nEnvironment:\n"
	 "  FWM_ICONS=%s\n"
	 "  FWM_FONT=%s\n"
	 "  FWM_FONT_SIZE=%f\n"
	 "  FWM_HDPI=%u\n"
	 "  FWM_VDPI=%u\n"
	 "\nKey bindings:\n"
	 "  Down/Up    navigate rows\n"
	 "  PgDn/PgUp  navigate pages\n"
	 "  Tab        goto next row matching search pattern\n"
	 "  Right      copy 1st column of selected row to search bar\n"
	 "  Backspace  delete character before cursor in search bar\n"
	 "  Ctrl-u     clear search bar\n"
	 "  Return     print selected row to standard output\n"
	 "  Esc        exit without result\n\n",
	 name, ctx_.name, ctx_.fg, ctx_.bg, ctx_.selfg, ctx_.selbg, UCHAR_MAX,
	 INT16_MAX, ctx_.icon_font, ctx_.text_font, ctx_.font_size,
	 ctx_.hdpi, ctx_.vdpi);
}

static int opt(const char *arg, const char *args, const char *argl)
{
	return (strcmp(arg, args) == 0 || strcmp(arg, argl) == 0);
}

static uint8_t opts(int argc, char *argv[])
{
	const char *hdpi_str;
	const char *vdpi_str;
	const char *font_size_str;
	int i;

	/* init these defaults before checkig args */

	ctx_.name = "menu";
	ctx_.fg = 0xa0a0a0;
	ctx_.bg = 0x0f0f0f;
	ctx_.selfg = 0xe0e0e0;
	ctx_.selbg = 0x303030;

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
		} else if (opt(arg, "-s", "--search-column")) {
			i++;
			search_col_idx_ = atoi(argv[i]);
			swap_col_idx_ = search_col_idx_;
		} else if (opt(arg, "-n", "--name")) {
			i++;
			ctx_.name = argv[i];
		} else if (opt(arg, "-d", "--delineate")) {
			interlace_ = 1;
		} else if (opt(arg, "-a", "--append")) {
			append_ = 1;
			print_input_ = 0;
		} else if (opt(arg, "-p", "--print-input")) {
			append_ = 0;
			print_input_ = 1;
		} else if (opt(arg, "-b", "--search-bar")) {
			search_bar_ = 1;
		} else if (opt(arg, "-0", "--normal-foreground")) {
			i++;
			if (argv[i])
				ctx_.fg = strtol(argv[i], NULL, 16);
		} else if (opt(arg, "-1", "--normal-background")) {
			i++;
			if (argv[i])
				ctx_.bg = strtol(argv[i], NULL, 16);
		} else if (opt(arg, "-2", "--active-foreground")) {
			i++;
			if (argv[i])
				ctx_.selfg = strtol(argv[i], NULL, 16);
		} else if (opt(arg, "-3", "--active-background")) {
			i++;
			if (argv[i])
				ctx_.selbg = strtol(argv[i], NULL, 16);
		}
	}

	if (!search_bar_)
		append_ = 0;

	if (!(ctx_.path = argv[i - 1])) {
		ee("path is not set\n");
		help(argv[0]);
		return 0;
	}

	if (!ctx_.icon_font) {
		ww("Icons font is not set\n");
	} else {
		icon_.font_id = open_font(ctx_.icon_font, ctx_.hdpi, ctx_.vdpi);
		if (invalid_font_id(icon_.font_id))
			return 0;
		else if (!init_text(&icon_))
			return 0;

		get_space_width(icon_.font_id);
	}

	if (!ctx_.text_font) {
		ww("Text font is not set\n");
	} else {
		text_.font_id = open_font(ctx_.text_font, ctx_.hdpi, ctx_.vdpi);
		if (invalid_font_id(text_.font_id))
			return 0;
		else if (!init_text(&text_))
			return 0;

		get_space_width(text_.font_id);
	}

	return 1;
}

static xcb_atom_t get_atom(const char *str, uint8_t len)
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

#ifdef STAY_OBSCURED
static void wait(void)
{
	int state;
	xcb_generic_event_t *e;

	while (1) {
		e = xcb_wait_for_event(ctx_.dpy);
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

static void button_press(xcb_button_press_event_t *e)
{
	switch (e->detail) {
	case MOUSE_BTN_LEFT: /* fall through */
	case MOUSE_BTN_MID: /* fall through */
	case MOUSE_BTN_RIGHT:
		*(selrow_->str + selrow_->len) = '\0';
		printf("%s\n", selrow_->str);
		ctx_.done = 1;
		break;
	case MOUSE_BTN_FWD:
		line_up();
		break;
	case MOUSE_BTN_BACK:
		line_down();
		break;
	default:
		break;
	}
}

static uint8_t events(uint8_t wait)
{
	xcb_generic_event_t *e;

	if (wait)
		e = xcb_wait_for_event(ctx_.dpy);
	else
		e = xcb_poll_for_event(ctx_.dpy);

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
			ctx_.done = 1;
#endif
			break;
		case XCB_VISIBILITY_UNOBSCURED:
			dd("XCB_VISIBILITY_UNOBSCURED\n");
			break;
		}
		break;
	case XCB_UNMAP_NOTIFY:
		dd("XCB_UNMAP_NOTIFY\n");
		xcb_ungrab_pointer(ctx_.dpy, XCB_CURRENT_TIME);
		ctx_.done = 1;
		break;
	case XCB_DESTROY_NOTIFY:
		dd("XCB_DESTROY_NOTIFY\n");
		xcb_ungrab_pointer(ctx_.dpy, XCB_CURRENT_TIME);
		ctx_.done = 1;
		break;
	case XCB_ENTER_NOTIFY:
		dd("XCB_ENTER_NOTIFY\n");
		grab_pointer();
		break;
	case XCB_LEAVE_NOTIFY:
		dd("XCB_LEAVE_NOTIFY\n");
		xcb_ungrab_pointer(ctx_.dpy, XCB_CURRENT_TIME);
		ctx_.done = 1;
		break;
	case XCB_EXPOSE:
		dd("XCB_EXPOSE\n");
		grab_pointer();
		draw_menu();
		break;
	case XCB_KEY_PRESS:
		key_press((xcb_key_press_event_t *) e);
		break;
	case XCB_KEY_RELEASE:
		key_release((xcb_key_press_event_t *) e);
		break;
	case XCB_BUTTON_PRESS:
		dd("XCB_BUTTON_PRESS\n");
		button_press((xcb_button_press_event_t *) e);
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
	uint8_t ret = 1;
	struct pollfd pfd;
	int fd;
	uint32_t mask;
	xcb_screen_t *scr;
	uint32_t val[2];
	char *env = getenv("FWM_SCALE");
	float font_scale;

	env ? (font_scale = atof(env)) : (font_scale = 1);

	if (!opts(argc, argv))
		return 1;
	else if (!(ctx_.dpy = xcb_connect(NULL, NULL))) {
		ee("xcb_connect() failed\n");
		return 1;
	}

	scr = xcb_setup_roots_iterator(xcb_get_setup(ctx_.dpy)).data;
	if (!scr) {
		ee("failed to get screen\n");
		return 1;
	}

	xkb_x11_setup_xkb_extension(ctx_.dpy, XKB_X11_MIN_MAJOR_XKB_VERSION,
	 XKB_X11_MIN_MINOR_XKB_VERSION, XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS,
	 NULL, NULL, NULL, NULL);

	ctx_.win = xcb_generate_id(ctx_.dpy);
	ctx_.gc = xcb_generate_id(ctx_.dpy);

	mask = XCB_GC_FOREGROUND;
	val[0] = ctx_.fg;
	mask |= XCB_GC_GRAPHICS_EXPOSURES;
	val[1] = 0;
	xcb_create_gc(ctx_.dpy, ctx_.gc, scr->root, mask, val);

	if ((fd = init_menu()) < 0)
		goto err;

	page_h_ = ctx_.row_h * rows_per_page_;

	dd("menu %ux%u cols %u rows %u/%u glyphs per row %u row height %u\n",
	   page_w_, page_h_, row_len_, rows_per_page_, rows_num_,
	   page_w_ / x_pad_, ctx_.row_h);

	mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	val[0] = ctx_.bg;
	val[1] = XCB_EVENT_MASK_EXPOSURE;
	val[1] |= XCB_EVENT_MASK_VISIBILITY_CHANGE;
	val[1] |= XCB_EVENT_MASK_KEY_PRESS;
	val[1] |= XCB_EVENT_MASK_KEY_RELEASE;
	val[1] |= XCB_EVENT_MASK_BUTTON_PRESS;
	val[1] |= XCB_EVENT_MASK_POINTER_MOTION;
	val[1] |= XCB_EVENT_MASK_LEAVE_WINDOW;
	val[1] |= XCB_EVENT_MASK_ENTER_WINDOW;

	uint16_t tmp;
	search_bar_ ? (tmp = ctx_.row_h) : (tmp = 0);

	xcb_create_window(ctx_.dpy, XCB_COPY_FROM_PARENT, ctx_.win, scr->root,
	 0, 0, page_w_ + 2 * x_pad_, page_h_ + 2 * y_pad_ + tmp, 0,
	 XCB_WINDOW_CLASS_INPUT_OUTPUT, scr->root_visual, mask, val);

        xcb_change_property(ctx_.dpy, XCB_PROP_MODE_REPLACE, ctx_.win,
	 XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, strlen(ctx_.name), ctx_.name);

        xcb_change_property(ctx_.dpy, XCB_PROP_MODE_REPLACE, ctx_.win,
	 XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, strlen(ctx_.name), ctx_.name);

	xcb_atom_t atom = get_atom("_NET_WM_PID", sizeof("_NET_WM_PID") - 1);
	uint32_t data = getpid();
	xcb_change_property(ctx_.dpy, XCB_PROP_MODE_REPLACE, ctx_.win, atom,
	 XCB_ATOM_CARDINAL, 32, 1, &data);

	xcb_map_window(ctx_.dpy, ctx_.win);
	atom = get_atom("_NET_ACTIVE_WINDOW", sizeof("_NET_ACTIVE_WINDOW") - 1);
	xcb_change_property(ctx_.dpy, XCB_PROP_MODE_REPLACE, scr->root, atom,
	 XCB_ATOM_WINDOW, 32, 1, &ctx_.win);
	xcb_set_input_focus_checked(ctx_.dpy, XCB_NONE, ctx_.win,
	 XCB_CURRENT_TIME);
	xcb_flush(ctx_.dpy);

	if (!(ctx_.syms = xcb_key_symbols_alloc(ctx_.dpy)))
		ww("xcb_key_symbols_alloc() failed\n");

	if (init_xkb() < 0)
		goto err;

	while (!ctx_.done)
		events(1);

	/* DUNNO: this trick is needed to deliver events in case another
	 * menu window grabs pointer
	 */

	pfd.fd = xcb_get_file_descriptor(ctx_.dpy);
	pfd.events = POLLIN;
	pfd.revents = 0;

	while (!ctx_.done) {
		errno = 0;
		poll(&pfd, 1, -1);

		if (errno == EINTR)
			continue;

		if (pfd.revents & POLLHUP)
			break;
		else if (pfd.revents & POLLIN)
			while (events(0)) {};

		pfd.revents = 0;
	}

	ret = 0;
err:
	destroy_text(&icon_.txt);
	destroy_text(&text_.txt);
	close_font(icon_.font_id);
	close_font(text_.font_id);

	return ret;
}
