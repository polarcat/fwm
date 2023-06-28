#pragma once

#include <xcb/xcb.h>
#include <stdint.h>

typedef int8_t fontid_t;

#ifndef MAX_FONTS
#define MAX_FONTS 2U
#endif

enum font_constants {
	INVALID_FONT_ID = -1,
};

struct text;

struct xcb {
	xcb_connection_t *dpy;
	xcb_drawable_t win;
	xcb_gcontext_t gc;
};

static inline uint8_t invalid_font_id(fontid_t font_id)
{
	return (font_id == INVALID_FONT_ID || font_id >= MAX_FONTS);
}

void close_font(fontid_t font_id);
fontid_t open_font(const char *path, uint16_t hdpi, uint16_t vdpi);

struct text *create_text(void);
void destroy_text(struct text **);
void set_text_font(struct text *, fontid_t, float font_size);
void set_text_pos(struct text *, int16_t, int16_t);
void set_text_str(struct text *, const char *, uint16_t len);
void set_text_color(struct text *, uint32_t fg, uint32_t bg);
void get_text_size(struct text *, uint16_t *w, uint16_t *h);
void set_text_fade(struct text *, uint16_t glyph_idx);
void draw_text_xcb(struct xcb *xcb, struct text *text);
