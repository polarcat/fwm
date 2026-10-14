/* menu-lib.c: rudimentary menu dialog to be called from Lua.
 *
 * Copyright (c) 2026, Aliaksei Katovich <aliaksei.katovich at gmail.com>
 *
 * Released under the GNU General Public License, version 2
 */

#define USE_CRC32
#include "text.h"

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>

#include <math.h>

#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>

#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-x11.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#define XK_MISCELLANY
#include <X11/keysymdef.h>

#define LOG_TAG "menulib: "

#include "fwm.h"
#include "misc.h"

#ifndef UCHAR_MAX
#define UCHAR_MAX 255
#endif

#ifndef MENU_APP
#define MENU_APP "fwm-menu"
#endif

#define MENU_LOCK_PATH "/tmp/." MENU_APP ".lock"
#define COLOR_DISTANCE 10
#define PROMPT_LEN (sizeof("> ") - 1)

struct config {
	const char *name;
	int hdpi;
	int vdpi;
	int fg;
	int bg;
	int selfg;
	int selbg;
	int curbg;
	float font_scale;
	float font_size;
	const char *text_font;
	const char *icon_font;
	bool delineate; // highlight odd/even rows
	bool searchbar; // show search bar
	bool hide_input; // hide input in the search bar
	bool wait_visible; // wait until window becomes fully visible
	int page_size; // number of rows on the page
	int row_size; // expand row to this number of columns
	int selidx;
};

static struct config cfg_;

static xcb_screen_t *scr_;
static xcb_connection_t *dpy_;
static xcb_drawable_t win_;
static xcb_gcontext_t gc_;
static xcb_key_symbols_t *ksyms_;
static struct xkb_context *xkb_;
static struct xkb_keymap *keymap_;

static bool warp_pointer_;
static bool done_;
static int lock_ = -1;

struct text_info {
	fontid_t font_id;
	struct text *txt;
	char *str;
	uint8_t len;
	uint8_t x;
};

static struct text_info text_;
static struct text_info icon_;

static uint16_t text_h_;
static int16_t text_y_;
static int16_t icon_y_;

static char search_buf_[UCHAR_MAX];
static uint8_t search_idx_ = PROMPT_LEN; /* '> ' */
static int16_t found_idx_;
static uint8_t swap_col_idx_;
static uint8_t search_col_idx_;
static char *input_ptr_ = search_buf_;

static uint16_t page_w_;
static uint16_t page_h_;

static uint8_t pages_num_;
static uint8_t rows_rem_;

static uint8_t space_w_;

struct page {
	char *rowptr;
	uint16_t rowidx;
};

static struct page pages_[UCHAR_MAX + 1];

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

static char data_buf_[128];
static char *data_;
static size_t data_size_;

static uint16_t row_h_;
static struct row *selrow_;
static struct row *rows_;

static struct row **view_;

static uint16_t *cols_px_;
static uint8_t *cols_size_;
static uint8_t cols_per_row_;

static uint16_t rows_num_; /* lines in file */

static uint8_t selidx_;
static uint8_t page_idx_;
static uint8_t follow_;

static char hidden_buf_[128];
static uint8_t hidden_idx_;

static void get_space_width(fontid_t font_id)
{
	uint16_t w;
	uint16_t h;
	struct text *text = create_text();

	if (!text)
		return;

	set_text_font(text, font_id, cfg_.font_size);

	set_text_str(text, "=", sizeof("="));
	get_text_size(text, &w, &h);
	space_w_ = w;

	set_text_str(text, "|", sizeof("|"));
	get_text_size(text, &w, &h);
	text_h_ = h;

	destroy_text(&text);
}

static void fill_rect(uint32_t c, int16_t x, int16_t y, uint16_t w, uint16_t h)
{
	xcb_rectangle_t rect = { x, y, w, h, };
	xcb_change_gc(dpy_, gc_, XCB_GC_FOREGROUND, &c);
	xcb_poly_fill_rectangle(dpy_, win_, gc_, 1, &rect);
	xcb_flush(dpy_);
}

static void draw_line(uint8_t idx)
{
	int16_t y = idx * row_h_ + y_pad_ + row_h_ / 2;
	int16_t h = row_h_ / 8;
	if (h < 1) {
		h = 1;
	}
	fill_rect(cfg_.selbg, x_pad_, y, page_w_, h);
}

static int16_t draw_rect(uint8_t idx, bool focus)
{
	int16_t y = idx * row_h_ + y_pad_;
	uint32_t bg;

	focus ? (bg = cfg_.selbg) : (bg = cfg_.bg);

	if (cfg_.delineate && !focus && idx % 2) {
		bg = ((((bg >> 16) & 0xff) + COLOR_DISTANCE) << 16)
		     | ((((bg >> 8) & 0xff) + COLOR_DISTANCE) << 8)
		     | ((bg & 0xff) + COLOR_DISTANCE);
	}

	fill_rect(bg, x_pad_, y, page_w_, row_h_);
	cfg_.curbg = bg;
	return y;
}

static int16_t draw_col(struct column *col, uint8_t i, int16_t x, int16_t y,
 bool focus)
{
	uint16_t w;
	uint16_t h;
	uint32_t fg;
	uint32_t bg;
	uint8_t len;
	struct xcb xcb = { dpy_, win_, gc_ };

	if (!col->str || !col->len) {
		return 0;
	} else if (*col->str == '\a') {
		col->str++;
		col->len--;
	}

	if (focus) {
		fg = cfg_.selfg;
		bg = cfg_.selbg;
	} else {
		fg = cfg_.fg;
		bg = cfg_.bg;
	}

	set_text_str(col->text, col->str, col->len);
	get_text_size(col->text, &w, &h);

	if (w > cols_px_[i]) {
		len = cols_size_[i];
	} else {
		len = col->len;
	}

	set_text_str(col->text, col->str, len);
	set_text_pos(col->text, x, y);
	set_text_color(col->text, fg, bg);
	draw_text_xcb(&xcb, col->text);

	return x + w + x_pad_;
}

static void draw_search_bar(void)
{
	struct xcb xcb = { dpy_, win_, gc_ };
	int16_t x = x_pad_ * 2;
	int16_t y = draw_rect(cfg_.page_size, 0) + text_y_;
	uint8_t len;
	char *str = search_buf_;

	if (cfg_.hide_input) {
		len = hidden_idx_ + 1;
		y += 3 * (space_w_ / 3);
	} else {
		str[0] = '>';
		str[1] = ' ';
		str[search_idx_] = '_';
		len = search_idx_ + 1;
	}

	set_text_str(text_.txt, str, len);
	set_text_pos(text_.txt, x, y);
	set_text_color(text_.txt, cfg_.fg, cfg_.curbg);
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

	len = cols_size_[dst];

	if (cols_size_[dst] < cols_size_[src]) {
		cols_size_[dst] = cols_size_[src];
	}

	cols_size_[src] = len;
	px = cols_px_[dst];

	if (cols_px_[dst] < cols_px_[src]) {
		cols_px_[dst] = cols_px_[src];
	}

	cols_px_[src] = px;
}

static void warp_pointer(int16_t x, int16_t y)
{
	if (warp_pointer_) {
		xcb_warp_pointer(dpy_, XCB_NONE, win_, 0, 0, 0, 0, x, y);
		xcb_flush(dpy_);
		warp_pointer_ = false;
	}
}

static void draw_row(struct row *row, uint8_t idx, bool focus)
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

	if (*col->str == '\n') { // Separator line.
		draw_line(idx);
		return;
	}

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

	if (ptr == end) {
		col->len = ptr - start;
	}

	y = draw_rect(idx, focus) + text_y_;
	x = x_pad_ * 2;

	if (swap_col_idx_) {
		swap_cols(row, 0, swap_col_idx_);
	}

	for (i = 0; i < cols_per_row_; i++) {
		int16_t start_y = y;

		if (row->cols[i].text == icon_.txt) {
			start_y--; // HACK: icon is better aligned ths way.
		}

		draw_col(&row->cols[i], i, x, start_y, focus);
		x += cols_px_[i];
	}

	if (focus) {
		warp_pointer(page_w_ + x_pad_, y);
	}
}

static void draw_menu(void)
{
	char *start = pages_[page_idx_].rowptr;
	char *end = data_ + data_size_;
	char *ptr = start;
	bool focus = false;
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
				focus = true;
			} else if (!selrow_ && !selidx_) {
				selrow_ = row;
				focus = true;
			} else if (selrow_ == row) {
				focus = true;
			} else {
				focus = false;
			}

			draw_row(row, i, focus);
			view_[i] = row;
			i++;
			start = ptr + 1;

			if (rowidx >= rows_num_) { /* fill dummy rows ... */
				for (; i < cfg_.page_size; i++) {
					int16_t y = i * row_h_ + y_pad_;
					fill_rect(cfg_.bg, x_pad_, y, page_w_,
						row_h_);
				}

				break; /* ... and return */
			}

			if (i >= cfg_.page_size) {
				break; /* page done */
			}
		}

		ptr++;
	}

	if (cfg_.searchbar) {
		draw_search_bar();
	}

	warp_pointer(page_w_ + x_pad_, row_h_ - y_pad_);
}

static bool match_col(const char *col, uint8_t len)
{
	uint8_t search_idx = search_idx_ - PROMPT_LEN;

	if (len < search_idx) {
		return false;
	}

	if (memcmp(col, search_buf_ + PROMPT_LEN, search_idx) == 0) {
		return true;
	}

	return false;
}

static bool find_col(const char *rowstr, uint8_t idx)
{
	const char *ptr = rowstr;
	const char *col_start = rowstr;
	uint8_t col_idx = 0;

	if (*ptr == '\a' && idx < cols_per_row_) { /* if icon column */
		idx++;
	}

	while (*ptr != '\n') {
		if (*ptr == '\t') {
			if (col_idx == idx) {
				if (match_col(col_start, ptr - col_start)) {
					return true;
				}
			}

			col_idx++;
			col_start = ptr + 1;
		}

		ptr++;
	}

	if (*ptr == '\n' && idx == cols_per_row_ - 1) {
		if (match_col(col_start, ptr - col_start)) {
			return true;
		}
	}

	return false;
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
		logd("ignore sym 0x%x", sym);
		return;
	} else if (search_idx_ < sizeof(search_buf_)) {
		search_buf_[search_idx_++] = sym;
	}

	if (cfg_.searchbar) {
		draw_search_bar();
	}

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

			if (selidx++ >= cfg_.page_size - 1) {
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
	selidx_ = cfg_.page_size - 1;
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
	warp_pointer_ = true;
	draw_row(selrow_, selidx_, 1);
}

static void line_down(void)
{
	uint8_t rem = (selidx_ + rows_rem_) == cfg_.page_size - 1;

	if (page_idx_ == pages_num_ - 1 && rem) {
		return; /* last not full page */
	} else if (selidx_ == cfg_.page_size && page_idx_ == pages_num_) {
		return; /* last page */
	} else if (selidx_ == cfg_.page_size && !pages_num_) {
		return; /* single page */
	} else if (selidx_ == cfg_.page_size - 1) {
		page_down();
		return;
	}

	draw_row(selrow_, selidx_, 0);
	selidx_++;
	selrow_ = view_[selidx_];
	warp_pointer_ = true;
	draw_row(selrow_, selidx_, 1);
}

static xcb_keysym_t get_keysyms(xcb_keycode_t code)
{
	const xkb_keysym_t *syms;
	int len;

	if (keymap_ == NULL) {
		return (xcb_keysym_t) 0;
	}

	len = xkb_keymap_key_get_syms_by_level(keymap_, code, 0, level_, &syms);

#ifdef DEBUG
	int i;
	for (i = 0; i < len; i++) {
		char name[64];
		const xkb_keysym_t sym = syms[i];
		xkb_keysym_get_name(sym, name, sizeof(name) - 1);
		logd("[%d] sym 0x%x name '%s'", i, sym, name);
	}
#endif

	if (len) {
		return (xcb_keysym_t) syms[0];
	}

	return (xcb_keysym_t) 0;
}

static void key_release(xcb_key_press_event_t *e)
{
	xcb_keysym_t sym;

	if (ksyms_ == NULL) {
		logw("key symbols not available");
		return;
	}

	sym = xcb_key_release_lookup_keysym(ksyms_, e, 0);
	if (sym == XK_Shift_L || sym == XK_Shift_R) {
		level_ = 0; /* expect lower-case symbols */
	} else if (sym == XKB_KEY_ISO_Level3_Shift) {
		level_ = 0;
	} else if (sym == XK_Control_L || sym == XK_Control_R) {
		control_ = 0; /* cancel command */
	}
}

static void reset_pointer(void **ptr)
{
	if (*ptr != NULL) {
		free(*ptr);
		*ptr = NULL;
	}
}

static void release_singleton_lock(int fd)
{
	if (fd < 0) {
		return;
	}

	struct flock fl = {
		.l_type = F_UNLCK, // Set lock type to unlock
		.l_whence = SEEK_SET, // Start from beginning of file
		.l_start = 0, // Offset 0
		.l_len = 0 // 0 means until EOF (unlocks the whole file)
	};

	if (fcntl(fd, F_SETLK, &fl) == -1) {
		loge("Failed to unlock '" MENU_LOCK_PATH "'");
	}
}

static bool acquire_singleton_lock(void)
{
	lock_ = open(MENU_LOCK_PATH, O_CREAT | O_RDWR, 0600);
	if (lock_ < 0) {
		loge("Failed to create lock '" MENU_LOCK_PATH "'");
		return false;
	}

	struct flock fl = {
		.l_type = F_WRLCK,
		.l_whence = SEEK_SET,
		.l_start = 0,
		.l_len = 0
	};

	if (fcntl(lock_, F_SETLK, &fl) != -1) {
		return true;
	} else if (fcntl(lock_, F_GETLK, &fl) == -1) {
		loge("Failed to lock '" MENU_LOCK_PATH "'");
		return false;
	}

	if (fl.l_type != F_UNLCK) {
		logw("Stop %d behind the lock", fl.l_pid);
		kill(fl.l_pid, SIGKILL);
	}

	for (int i = 0; i < 20; i++) {
		if (fcntl(lock_, F_SETLK, &fl) != -1) {
			return true;
		}
		sleep_ms(100);
	}

	loge("Failed to lock '" MENU_LOCK_PATH "'");
	return false;
}

static void cleanup()
{
	destroy_text(&icon_.txt);
	destroy_text(&text_.txt);

	if (data_ != NULL) {
		free(data_);
		data_ = NULL;
	}

	reset_pointer((void**)&cols_size_);
	reset_pointer((void**)&cols_px_);
	reset_pointer((void**)&view_);

	for (int i = 0; i < rows_num_; i++) {
		struct row *row = &rows_[i];
		reset_pointer((void**)&row->cols);
	}
	reset_pointer((void**)&rows_);

	if (ksyms_ != NULL) {
		xcb_key_symbols_free(ksyms_);
		ksyms_ = NULL;
	}

	if (lock_ >= 0) {
		release_singleton_lock(lock_);
		close(lock_);
		lock_ = -1;
	}

	if (dpy_ != NULL) {
		if (win_ != XCB_NONE) {
			xcb_destroy_window(dpy_, win_);
			win_ = XCB_NONE;
		}
		if (gc_ != XCB_NONE) {
			xcb_free_gc(dpy_, gc_);
			gc_ = XCB_NONE;
		}
		xcb_flush(dpy_);
		xcb_disconnect(dpy_);
		dpy_ = NULL;
	}

	cols_per_row_ = 0;
	pages_num_ = 0;
	page_idx_ = 0;
	rows_num_ = 0;
	rows_rem_ = 0;
	selrow_ = NULL;

	search_idx_ = PROMPT_LEN; /* '> ' */
	memset(search_buf_, 0, sizeof(search_buf_));
}

static void done()
{
	if (cfg_.hide_input) {
		hidden_buf_[hidden_idx_] = '\0';
		logd("'%s'", hidden_buf_);
	} else {
		if (search_idx_ < PROMPT_LEN) {
			search_idx_ = PROMPT_LEN;
		}
		input_ptr_ = &search_buf_[PROMPT_LEN];
		*(input_ptr_ + search_idx_ - 2) = '\0';
		*(selrow_->str + selrow_->len) = '\0';
		logd("'%s'", selrow_->str);
	}
	done_ = true;
}

static void key_press(xcb_key_press_event_t *e)
{
	xcb_keysym_t sym;

#ifdef DEBUG
	get_keysyms(e->detail);
#endif

	if (ksyms_ == NULL) {
		logw("key symbols not available");
		return;
	}

	sym = xcb_key_press_lookup_keysym(ksyms_, e, 0);

	if (cfg_.hide_input) {
		if (sym == XK_BackSpace && hidden_idx_ == 0) {
			search_buf_[0] = '\0';
			hidden_buf_[0] = '\0';
		} else if (sym == XK_BackSpace && hidden_idx_ > 0) {
			hidden_idx_--;
			search_buf_[hidden_idx_] = '\0';
			hidden_buf_[hidden_idx_] = '\0';
		} else if (sym == XK_Return) {
			done();
		} else if (sym == XK_Shift_L || sym == XK_Shift_R) {
			level_ = 1; // expect upper-case symbols
			return;
		} else if (sym < 0x21 || sym > 0x7e) {
			return;
		} else if ((sym = get_keysyms(e->detail))) {
			if (hidden_idx_ < sizeof(hidden_buf_)) {
				search_buf_[hidden_idx_] = '*';
				hidden_buf_[hidden_idx_] = sym;
				hidden_idx_++;
			}
		}

		draw_search_bar();
		return;
	}

	if (sym == XK_Shift_L || sym == XK_Shift_R) {
		level_ = 1; // expect upper-case symbols
		return;
	} else if (sym == XKB_KEY_ISO_Level3_Shift) {
		// FIXME: AltGr does not correspond level 3 on some systems.
		level_ = 2; // expect AltGr symbols
		return;
	} else if (sym == XK_Control_L || sym == XK_Control_R) {
		control_ = 1; // expect command
		return;
	} else if (sym == XK_Escape) { // cancel
		selrow_ = NULL;
		done_ = true;
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
		if (cfg_.searchbar) {
			draw_search_bar();
		}
	} else if (sym == XK_Return) {
		done();
	} else if ((sym = get_keysyms(e->detail))) {
		warp_pointer_ = true;
		find_row(sym);
	}

	follow_ = 0;
}

static void follow_pointer(xcb_motion_notify_event_t *e)
{
	uint8_t i;
	uint8_t rows_num;

	if (pages_num_ && page_idx_ == pages_num_ - 1) {
		rows_num = cfg_.page_size - rows_rem_;
	} else {
		rows_num = cfg_.page_size;
	}

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

	if (!pages_num_) {
		return;
	}

	if (e->event_y >= page_h_ + y_pad_) { /* follow down */
		if (page_idx_ >= pages_num_ - 1) {
			return;
		}

		warp_pointer(page_w_ + x_pad_, y_pad_);
		page_down();
	} else if (e->event_y < y_pad_) { /* follow up */
		if (!page_idx_) {
			return;
		}

		warp_pointer(page_w_ + x_pad_, page_h_ - y_pad_);
		page_up();
	}
}

static void grab_pointer(void)
{
	xcb_grab_pointer(dpy_, XCB_NONE, win_,
			 XCB_EVENT_MASK_POINTER_MOTION
			 | XCB_EVENT_MASK_BUTTON_PRESS
			 | XCB_EVENT_MASK_BUTTON_RELEASE,
			 XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
			 win_, XCB_NONE, XCB_CURRENT_TIME);
	xcb_flush(dpy_);
	return;
}

struct newline_info {
	// in
	uint16_t w;
	char *ptr;
	const char *col_start;
	// out
	uint16_t col;
	char *row_start;
	uint8_t row_max_len;
	char *longest_row;
};

static bool handle_newline(struct newline_info *info)
{
	rows_num_++;

	if (info->col == 0 && *info->ptr == '\n') {
		return true;
	}

	if (info->col + 1 < cols_per_row_) {
		loge("expected %u columns in row %u", cols_per_row_, rows_num_);
		return false;
	} else if (info->col == cols_per_row_ - 1) {
		uint8_t len = info->ptr - info->col_start;

		if (info->w > cols_px_[info->col]) {
			cols_px_[info->col] = info->w;
		}

		len = info->ptr - info->row_start;

		if (len > info->row_max_len) {
			info->row_max_len = len;
			info->longest_row = info->row_start;
		}

		info->row_start = info->ptr + 1;
		info->col = 0; /* reset lengths counter */

		uint8_t page = rows_num_ / cfg_.page_size;

		if (pages_[page].rowptr == NULL) {
			pages_[page].rowptr = info->row_start;
			pages_[page].rowidx = rows_num_;
		}
	}

	return true;
}

static bool init_rows(void)
{
	uint16_t col;
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

	x_pad_ = space_w_;
	y_pad_ = x_pad_;
	row_h_ = text_h_ + 2 * (space_w_ / 3);
	text_y_ = (row_h_ - text_h_) / 2;
	icon_y_ = text_y_;

	if (!(text_y_ % 2)) {
		text_y_++;
	}

	ptr = data_;
	while (*ptr == '\n' && ptr < end) {
		ptr++; // skip leading separators
	}

	if (ptr >= end) {
		loge("menu with separators only");
		return false;
	}

	while (ptr < end) { // calc number of items in row
		if (*ptr == '\t') {
			cols_per_row_++;
		} else if (*ptr == '\n') {
			cols_per_row_++;
			break;
		}

		ptr++;
	}

	if (cols_per_row_ < 1) {
		loge("zero number of columns");
		return false;
	}

	if (*ptr != '\n') {
		errno = 0;
		loge("end of line not found");
		return false;
	}

	if (!cols_per_row_) {
		loge("failed to find number of items in row");
		return false;
	}

	cols_size_ = calloc(sizeof(*cols_size_), cols_per_row_);

	if (!cols_size_) {
		loge("calloc(%lu) failed",
			sizeof(*cols_size_) * cols_per_row_);
		return false;
	}

	cols_px_ = calloc(sizeof(*cols_px_), cols_per_row_);

	if (!cols_px_) {
		loge("calloc(%lu) failed",
			sizeof(*cols_px_) * cols_per_row_);
		return false;
	}

	if (search_col_idx_ >= cols_per_row_) {
		search_col_idx_ = 0;
		swap_col_idx_ = 0;
	}

#ifdef DEBUG
	uint8_t col_max_len = cfg_.row_size / cols_per_row_;
	logd("max col len %u", col_max_len);
#endif
	ptr = data_;
	col_start = ptr;
	row_start = ptr;
	longest_row = data_;
	col = 0;
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

			cols_size_[col] = ptr - col_start + 1;
			set_text_str(text, col_start, cols_size_[col]);
			get_text_size(text, &w, &h);

			if (w > cols_px_[col]) {
				cols_px_[col] = w;
			}

			if (*ptr == '\t') {
				if (++col >= cols_per_row_) {
					loge("expected %u columns in row %u",
						cols_per_row_, rows_num_ + 1);
					return false;
				}
			} else if (*ptr == '\n') {
				struct newline_info info = {
					.w = w,
					.col = col,
					.ptr = ptr,
					.row_start = row_start,
					.col_start = col_start,
					.row_max_len = row_max_len,
					.longest_row = longest_row,
				};
				if (!handle_newline(&info)) {
					return false;
				}
				col = info.col;
				row_start = info.row_start;
				row_max_len = info.row_max_len;
				longest_row = info.longest_row;
			}

			col_start = ptr + 1;
		}

		ptr++;
	}

	if (longest_row == NULL) {
		errno = 0;
		loge("file parse error, longest row not found");
		return false;
	}

	pages_[UCHAR_MAX].rowptr = end;
	pages_[UCHAR_MAX].rowidx = 0;

#ifdef DEBUG
	char tmp[256] = {0};
	snprintf(tmp, sizeof(tmp), "%s", longest_row);
	logd("longstr: '%s'", tmp);
#endif

	logd("x pad: %u row len: %u str: '%s'", x_pad_, cfg_.row_size,
		longest_row);

	if (rows_num_ == 0) {
		if (cols_per_row_) {
			rows_num_ = 1;
		} else {
			errno = 0;
			loge("failed to detect number of rows");
			return false;
		}
	}

	if (cfg_.page_size > rows_num_) {
		cfg_.page_size = rows_num_;
	}

	logd("items per row %u, row cols %u, page rows %u/%u",
		cols_per_row_, cfg_.row_size, cfg_.page_size, rows_num_);

	rows_ = calloc(sizeof(*rows_), rows_num_);
	if (rows_ == NULL) {
		loge("calloc(%lu) failed", sizeof(*rows_) * rows_num_);
		return false;
	}

	view_ = calloc(sizeof(*view_), cfg_.page_size);
	if (view_ == NULL) {
		loge("calloc(%lu) failed", sizeof(*view_) * cfg_.page_size);
		return false;
	}

	int i;
	for (i = 0; i < rows_num_; i++) {
		struct row *row = &rows_[i];

		row->cols = calloc(sizeof(*row->cols), cols_per_row_);
		if (row->cols == NULL) {
			loge("calloc(%lu) failed",
				sizeof(*row->cols) * cols_per_row_);
			return false;
		}
	}

	pages_num_ = rows_num_ / cfg_.page_size;
	if (rows_num_ % cfg_.page_size) {
		pages_num_++;
	}

	rows_rem_ = pages_num_ * cfg_.page_size - rows_num_;
	page_w_ = 0;

	for (i = 0; i < cols_per_row_; i++) {
		cols_px_[i] += x_pad_;
		page_w_ += cols_px_[i];
	}

	if (cfg_.row_size > row_max_len) {
		uint16_t diff = page_w_ - cols_px_[i - 1];
		page_w_ += (cfg_.row_size - row_max_len) * space_w_;
		cols_px_[i - 1] = page_w_ - diff;
	}

	logd("text y: %u row width %u pages num %u", text_y_, page_w_,
		pages_num_);

	return true;
}

static bool init_xkb(void)
{
	int32_t dev;

	if ((xkb_ = xkb_context_new(XKB_CONTEXT_NO_FLAGS)) == NULL) {
		loge("xkb_context_new() failed");
		return false;
	}

	if ((dev = xkb_x11_get_core_keyboard_device_id(dpy_)) < 0) {
		loge("xkb_x11_get_core_keyboard_device_id() failed");
		return false;
	}

	keymap_ = xkb_x11_keymap_new_from_device(xkb_, dpy_, dev,
		XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (keymap_ == NULL) {
		loge("xkb_x11_keymap_new_from_device() failed");
		return false;
	}

#ifdef DEBUG
	logd("DEBUG %s:%d", __func__, __LINE__);
	state_ = xkb_x11_state_new_from_device(keymap_, dpy_, dev);
	if (state_ == NULL) {
		loge("xkb_x11_state_new_from_device() failed");
		return false;
	}
#endif

	return true;
}

static int get_integer(lua_State *L, const char *name, int defval)
{
	int ret;

	lua_getfield(L, 1, name);
	if (lua_isnil(L, -1)) {
		ret = defval;
	} else {
		ret = (int)luaL_checkinteger(L, -1);
	}

	lua_pop(L, 1);
	return ret;
}

static float get_float(lua_State *L, const char *name, float defval)
{
	float ret;

	lua_getfield(L, 1, name);
	if (lua_isnil(L, -1)) {
		ret = defval;
	} else {
		ret = (float)luaL_checknumber(L, -1);
	}

	lua_pop(L, 1);
	return ret;
}

static const char *get_string(lua_State *L, const char *name, const char *defval)
{
	const char *ret;

	lua_getfield(L, 1, name);
	if (lua_isnil(L, -1)) {
		ret = defval;
	} else {
		ret = strdup(luaL_checkstring(L, -1));
	}

	lua_pop(L, 1);
	return ret;
}

static bool get_boolean(lua_State *L, const char *name, bool defval)
{
	bool ret;

	lua_getfield(L, 1, name);
	if (lua_isnil(L, -1)) {
		ret = defval;
	} else {
		luaL_checktype(L, -1, LUA_TBOOLEAN);
		ret = lua_toboolean(L, -1);
	}

	lua_pop(L, 1);
	return ret;
}

static bool init_menu(lua_State *lua)
{
	int i;
	int n;
	int str_len;
	char *ptr;

	luaL_checktype(lua, 1, LUA_TTABLE);

	data_ = NULL;
	data_size_ = 0;
	n = luaL_len(lua, -1);
	ptr = data_;

	for (i = 1; i <= n; i++) {
		lua_geti(lua, -1, i);

		const char *str = luaL_checkstring(lua, -1);
		if (str == NULL) {
			loge("malformed menu item");
			lua_pop(lua, 1);
			return false;
		}

		str_len = strlen(str);
		if (str_len < 1) {
			loge("zero length menu item");
			lua_pop(lua, 1);
			return false;
		} else if (i == 1) {
			data_ = malloc(str_len);
			ptr = data_;
		} else {
			data_ = realloc(data_, data_size_ + str_len);
			ptr = data_ + data_size_;
		}

		if (data_ == NULL) {
			loge("failed to allocate %zu bytes", data_size_);
			lua_pop(lua, 1);
			return false;
		}

		memcpy(ptr, str, str_len);
		data_size_ += str_len;

		lua_pop(lua, 1);
	}

	if (data_ == NULL) {
		loge("data buffer is empty");
		return false;
	}

#ifdef DEBUG
	FILE *f = fopen("/tmp/data.bin", "w");
	if (f) {
		fwrite(data_, data_size_, 1, f);
		fclose(f);
	}
#endif

	return init_rows();
}

static uint8_t init_text(struct text_info *text)
{
	if (!(text->txt = create_text())) {
		return 0;
	}

	set_text_font(text->txt, text->font_id, cfg_.font_size);
	return 1;
}

static xcb_atom_t get_atom(const char *str, uint8_t len)
{
	xcb_intern_atom_cookie_t c;
	xcb_intern_atom_reply_t *r;
	xcb_atom_t a;

	c = xcb_intern_atom(dpy_, 0, len, str);
	r = xcb_intern_atom_reply(dpy_, c, NULL);
	if (!r) {
		loge("xcb_intern_atom(%s) failed", str);
		return (XCB_ATOM_NONE);
	}

	a = r->atom;
	free(r);
	return a;
}

static void focus_window()
{
	uint32_t val[1] = { XCB_STACK_MODE_ABOVE, };
	uint16_t mask = XCB_CONFIG_WINDOW_STACK_MODE;

	xcb_configure_window_checked(dpy_, win_, mask, val);

	xcb_change_property_checked(dpy_, XCB_PROP_MODE_REPLACE, scr_->root,
		win_, XCB_ATOM_WINDOW, 32, 1, &win_);
	xcb_set_input_focus_checked(dpy_, XCB_NONE, win_, XCB_CURRENT_TIME);
	xcb_flush(dpy_);
}

static void wait_visible(void)
{
	int state;
	xcb_generic_event_t *e;

	while (1) {
		e = xcb_wait_for_event(dpy_);
		if (!e) {
			continue;
		}

		state = ((xcb_visibility_notify_event_t *) e)->state;
		free(e);
		if (state == XCB_VISIBILITY_UNOBSCURED) {
			break;
		} else if (state == XCB_VISIBILITY_PARTIALLY_OBSCURED) {
			break;
		}
	}
}

static void button_press(xcb_button_press_event_t *e)
{
	switch (e->detail) {
	case MOUSE_BTN_LEFT: // fall through
	case MOUSE_BTN_MID: // fall through
	case MOUSE_BTN_RIGHT:
		done();
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

static bool events(bool wait)
{
	xcb_generic_event_t *e;

	if (wait) {
		e = xcb_wait_for_event(dpy_);
	} else {
		e = xcb_poll_for_event(dpy_);
	}

	if (e == NULL) {
		return false;
	} else if (e->response_type == 0) {
		return false;
	}

	switch (e->response_type & ~0x80) {
	case XCB_VISIBILITY_NOTIFY:
		switch (((xcb_visibility_notify_event_t *) e)->state) {
		case XCB_VISIBILITY_PARTIALLY_OBSCURED: // fall through
			logd("XCB_VISIBILITY_PARTIALLY_OBSCURED");
			if (cfg_.hide_input) {
				break;
			}
		case XCB_VISIBILITY_FULLY_OBSCURED:
			logd("XCB_VISIBILITY_FULLY_OBSCURED");
			if (cfg_.hide_input) {
				focus_window();
			} else {
				if (cfg_.wait_visible) {
					wait_visible();
				} else {
					done(); // By default it is expected
						// that window is fully
						// visible. Otherwise close
						// window and ungrab pointer.
				}
			}
			break;
		case XCB_VISIBILITY_UNOBSCURED:
			logd("XCB_VISIBILITY_UNOBSCURED");
			break;
		}
		break;
	case XCB_UNMAP_NOTIFY:
		logd("XCB_UNMAP_NOTIFY");
		xcb_ungrab_pointer(dpy_, XCB_CURRENT_TIME);
		done();
		break;
	case XCB_DESTROY_NOTIFY:
		logd("XCB_DESTROY_NOTIFY");
		xcb_ungrab_pointer(dpy_, XCB_CURRENT_TIME);
		done();
		break;
	case XCB_ENTER_NOTIFY:
		logd("XCB_ENTER_NOTIFY");
		grab_pointer();
		break;
	case XCB_LEAVE_NOTIFY:
		logd("XCB_LEAVE_NOTIFY");
		xcb_ungrab_pointer(dpy_, XCB_CURRENT_TIME);
		done();
		break;
	case XCB_EXPOSE:
		logd("XCB_EXPOSE");
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
		logd("XCB_BUTTON_PRESS");
		button_press((xcb_button_press_event_t *) e);
		break;
	case XCB_MOTION_NOTIFY:
		logd("XCB_MOTION_NOTIFY");
		follow_pointer((xcb_motion_notify_event_t *) e);
		break;
	default:
		logi("event %d (%d)", e->response_type & ~0x80,
			e->response_type);
	}

	free(e);
	return true;
}

static bool init(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);

	cfg_.name = get_string(L, "name", MENU_APP);
	cfg_.hdpi = get_integer(L, "hdpi", 96);
	cfg_.vdpi = get_integer(L, "vdpi", 96);
	cfg_.fg = get_integer(L, "fg", 0xa0a0a0);
	cfg_.bg = get_integer(L, "bg", 0x0f0f0f);
	cfg_.selfg = get_integer(L, "selfg", 0xe0e0e0);
	cfg_.selbg = get_integer(L, "selbg", 0x303030);
	selidx_ = get_integer(L, "selidx", 0);
	cfg_.font_scale = get_float(L, "font_scale", 1.0f);
	cfg_.font_size = get_integer(L, "font_size", 10.5f);
	cfg_.text_font = get_string(L, "text_font", NULL);
	cfg_.icon_font = get_string(L, "icon_font", NULL);
	cfg_.delineate = get_boolean(L, "delineate", true);
	cfg_.searchbar = get_boolean(L, "searchbar", true);
	cfg_.hide_input = get_boolean(L, "hide_input", false);
	cfg_.wait_visible = get_boolean(L, "wait_visible", true);
	cfg_.page_size = get_integer(L, "page_size", 25);
	cfg_.row_size = get_integer(L, "row_size", 0);

	if (cfg_.text_font == NULL) {
		loge("text font is not provided");
		return false;
	} else if (cfg_.icon_font == NULL) {
		loge("icons font is not provided");
		return false;
	}

	warp_pointer_ = true;
	done_ = false;
	search_col_idx_ = 0;

	icon_.font_id = open_font(cfg_.icon_font, cfg_.hdpi, cfg_.vdpi);
	if (invalid_font_id(icon_.font_id)) {
		loge("invalid icons font");
		return false;
	} else if (!init_text(&icon_)) {
		loge("failed to init icons");
		return false;
	}

	get_space_width(icon_.font_id);

	text_.font_id = open_font(cfg_.text_font, cfg_.hdpi, cfg_.vdpi);
	if (invalid_font_id(text_.font_id)) {
		loge("invalid text font");
		return false;
	} else if (!init_text(&text_)) {
		loge("failed to init text");
		return false;
	}

	get_space_width(text_.font_id);

	return true;
}

//------------------------------------------------------------------------------
//
// Lua API
//
static int run(lua_State *L)
{
	struct pollfd pfd;
	uint32_t mask;
	uint32_t val[2];

	if (!init(L)) {
		return 0;
	} else if (!acquire_singleton_lock()) {
		return 0;
	} else if (!(dpy_ = xcb_connect(NULL, NULL))) {
		loge("xcb_connect() failed");
		return 0;
	}

	scr_ = xcb_setup_roots_iterator(xcb_get_setup(dpy_)).data;
	if (scr_ == NULL) {
		loge("failed to get screen");
		return 0;
	}

	xkb_x11_setup_xkb_extension(dpy_,
		XKB_X11_MIN_MAJOR_XKB_VERSION, XKB_X11_MIN_MINOR_XKB_VERSION,
		XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS, NULL, NULL, NULL, NULL);

	win_ = xcb_generate_id(dpy_);
	gc_ = xcb_generate_id(dpy_);

	mask = XCB_GC_FOREGROUND;
	val[0] = cfg_.fg;
	mask |= XCB_GC_GRAPHICS_EXPOSURES;
	val[1] = 0;
	xcb_create_gc(dpy_, gc_, scr_->root, mask, val);

	if (!cfg_.hide_input) {
		if (!init_menu(L)) {
			goto err;
		}
	} else {
		uint32_t sz1 = strlen(cfg_.name);
		uint32_t sz2 = sizeof(data_buf_) - 3;

		if (sz1 > sz2) {
			sz1 = sz2;
		}

		memcpy(data_buf_, cfg_.name, sz1);
		data_buf_[sz1] = '\n';

		data_ = data_buf_;
		data_size_ = sz1 + 1;

		if (init_rows() != 0) {
			goto err;
		}
	}

	page_h_ = row_h_ * cfg_.page_size;

	logd("menu %ux%u cols %u rows %u/%u glyphs per row %u row height %u",
		page_w_, page_h_, cfg_.row_size, cfg_.page_size, rows_num_,
		page_w_ / x_pad_, row_h_);

	mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
	val[0] = cfg_.bg;
	val[1] = XCB_EVENT_MASK_EXPOSURE;
	val[1] |= XCB_EVENT_MASK_VISIBILITY_CHANGE;
	val[1] |= XCB_EVENT_MASK_KEY_PRESS;
	val[1] |= XCB_EVENT_MASK_KEY_RELEASE;
	val[1] |= XCB_EVENT_MASK_BUTTON_PRESS;
	val[1] |= XCB_EVENT_MASK_POINTER_MOTION;
	val[1] |= XCB_EVENT_MASK_LEAVE_WINDOW;
	val[1] |= XCB_EVENT_MASK_ENTER_WINDOW;

	uint16_t tmp;
	cfg_.searchbar ? (tmp = row_h_) : (tmp = 0);

	xcb_create_window(dpy_, XCB_COPY_FROM_PARENT, win_, scr_->root, 0, 0,
		page_w_ + 2 * x_pad_, page_h_ + 2 * y_pad_ + tmp, 0,
		XCB_WINDOW_CLASS_INPUT_OUTPUT, scr_->root_visual, mask, val);

        xcb_change_property(dpy_, XCB_PROP_MODE_REPLACE, win_, XCB_ATOM_WM_NAME,
		XCB_ATOM_STRING, 8, strlen(cfg_.name), cfg_.name);
#if 0
	// Set WM_CLASS with two null-terminated strings: instance and class.
	// This allows XWayland to expose a proper app_id to the Wayland
	// compositor (e.g., for surface:get_app_id() in Weston's Lua shell).
	// We intern the WM_CLASS atom explicitly for robustness.
	//
	xcb_atom_t wm_class_atom = get_atom("WM_CLASS", sizeof("WM_CLASS") - 1);
	if (wm_class_atom != XCB_ATOM_NONE) {
		char wm_class[256];
		size_t len = strlen(cfg_.name);
		if (len >= sizeof(wm_class) / 2) {
			len = sizeof(wm_class) / 2 - 1;
		}

		memcpy(wm_class, cfg_.name, len);
		wm_class[len] = '\0';
		memcpy(wm_class + len + 1, cfg_.name, len);
		wm_class[len * 2 + 1] = '\0';
		xcb_change_property(dpy_, XCB_PROP_MODE_REPLACE, win_,
			wm_class_atom, XCB_ATOM_STRING, 8, len * 2 + 2,
			wm_class);
		/* Flush to ensure WM_CLASS is set before window is mapped */
		xcb_flush(dpy_);
	}

	// Set _NET_WM_WINDOW_TYPE to DIALOG so XWayland/Weston treats this
	// as a transient surface (menu) and assigns proper layer/role.
	//
	xcb_atom_t net_wm_window_type = get_atom("_NET_WM_WINDOW_TYPE",
		sizeof("_NET_WM_WINDOW_TYPE") - 1);
	xcb_atom_t net_wm_window_type_dialog = get_atom(
		"_NET_WM_WINDOW_TYPE_DIALOG",
		sizeof("_NET_WM_WINDOW_TYPE_DIALOG") - 1);
	if (net_wm_window_type != XCB_ATOM_NONE
		&& net_wm_window_type_dialog != XCB_ATOM_NONE) {
		xcb_change_property(dpy_, XCB_PROP_MODE_REPLACE, win_,
			net_wm_window_type, XCB_ATOM_ATOM, 32, 1,
			&net_wm_window_type_dialog);
	}
#endif

	xcb_atom_t atom = get_atom("_NET_WM_PID", sizeof("_NET_WM_PID") - 1);
	uint32_t data = getpid();
	xcb_change_property(dpy_, XCB_PROP_MODE_REPLACE, win_, atom,
		XCB_ATOM_CARDINAL, 32, 1, &data);

	xcb_map_window(dpy_, win_);
	atom = get_atom("_NET_ACTIVE_WINDOW", sizeof("_NET_ACTIVE_WINDOW") - 1);
	xcb_change_property(dpy_, XCB_PROP_MODE_REPLACE, scr_->root, atom,
		XCB_ATOM_WINDOW, 32, 1, &win_);
	xcb_set_input_focus_checked(dpy_, XCB_NONE, win_, XCB_CURRENT_TIME);
	xcb_flush(dpy_);

	if ((ksyms_ = xcb_key_symbols_alloc(dpy_)) == NULL) {
		logw("xcb_key_symbols_alloc() failed");
	}

	if (!init_xkb()) {
		goto err;
	}

	while (!done_) {
		events(true);
	}

	// DUNNO: this trick is needed to deliver events in case another menu
	// window grabs pointer.
	//
	pfd.fd = xcb_get_file_descriptor(dpy_);
	pfd.events = POLLIN;
	pfd.revents = 0;

	while (!done_) {
		errno = 0;
		poll(&pfd, 1, -1);

		if (errno == EINTR)
			continue;

		if (pfd.revents & POLLHUP)
			break;
		else if (pfd.revents & POLLIN)
			while (events(false)) {};

		pfd.revents = 0;
	}

err:
	if (cfg_.hide_input) {
		lua_pushstring(L, hidden_buf_);
	} else if (selrow_ != NULL) {
		lua_pushstring(L, strdup(selrow_->str));
	} else {
		lua_pushnil(L);
	}

	if (*input_ptr_ == '\0') {
		lua_pushnil(L);
	} else {
		lua_pushstring(L, input_ptr_);
	}

	cleanup();
	return 2;
}

static int sentinel_gc(lua_State *L)
{
	(void)L;
	cleanup();

	// Since fonts were cached only close them upon library closure.
	close_font(icon_.font_id);
	close_font(text_.font_id);

	return 0;
}

int luaopen_lua(lua_State *L)
{
	lua_newuserdatauv(L, 0, 0);
	if (luaL_newmetatable(L, "menulib.sentinel")) {
		lua_pushcfunction(L, sentinel_gc);
		lua_setfield(L, -2, "__gc");
	}
	lua_setmetatable(L, -2);
	lua_setfield(L, LUA_REGISTRYINDEX, "menulib.sentinel");

	luaL_Reg funcs[] = {
		{"run", run},
		{NULL, NULL}
	};
	luaL_newlib(L, funcs);
	return 1;
}
//
// Lua API
//
//------------------------------------------------------------------------------
