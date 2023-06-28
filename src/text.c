#include "text.h"
#include "misc.h"

#include <stdio.h>
#include <math.h>
#include <freetype/ftadvanc.h>
#include <freetype/ftsnames.h>
#include <freetype/tttables.h>
#include <freetype/ftglyph.h>
#include <xcb/xcb_image.h>

#ifndef MAX_PATH
#define MAX_PATH 128U
#endif

#define MAX_FONT_SIZE UINT8_MAX
#define MAX_FONT_SIZES (MAX_FONT_SIZE + 1)

#if 0
#define FT_LOAD_DEFAULT                      0x0
#define FT_LOAD_NO_SCALE                     ( 1L << 0 )
#define FT_LOAD_NO_HINTING                   ( 1L << 1 )
#define FT_LOAD_RENDER                       ( 1L << 2 )
#define FT_LOAD_NO_BITMAP                    ( 1L << 3 )
#define FT_LOAD_VERTICAL_LAYOUT              ( 1L << 4 )
#define FT_LOAD_FORCE_AUTOHINT               ( 1L << 5 )
#define FT_LOAD_CROP_BITMAP                  ( 1L << 6 )
#define FT_LOAD_PEDANTIC                     ( 1L << 7 )
#define FT_LOAD_IGNORE_GLOBAL_ADVANCE_WIDTH  ( 1L << 9 )
#define FT_LOAD_NO_RECURSE                   ( 1L << 10 )
#define FT_LOAD_IGNORE_TRANSFORM             ( 1L << 11 )
#define FT_LOAD_MONOCHROME                   ( 1L << 12 )
#define FT_LOAD_LINEAR_DESIGN                ( 1L << 13 )
#define FT_LOAD_NO_AUTOHINT                  ( 1L << 15 )
	/* Bits 16-19 are used by `FT_LOAD_TARGET_` */
#define FT_LOAD_COLOR                        ( 1L << 20 )
#define FT_LOAD_COMPUTE_METRICS              ( 1L << 21 )
#define FT_LOAD_BITMAP_METRICS_ONLY          ( 1L << 22 )
#endif

union rgb {
	uint32_t data;
	struct {
		uint8_t b;
		uint8_t g;
		uint8_t r;
	};
};

struct bmp {
	uint16_t w;
	uint16_t h;
	uint8_t *data;
};

struct text {
	/* requested values */
	fontid_t font_id;
	float font_size;
	const char *str;
	uint16_t len;
	int16_t x; /* relative to window's left edge */
	int16_t y; /* relative to window's top edge */
	union rgb fg;
	union rgb bg;
	uint8_t bg_brighter:1;

	/* returned values */
	uint16_t w;
	uint16_t h;
	uint16_t y_max;
	uint16_t y_max_max; /* store max known y_max */
	uint16_t fade_idx; /* start from a glyph at given index */
	uint8_t fade_step;
	uint8_t fade;
};

struct glyph {
	FT_BitmapGlyph bmp;
	int16_t y_min;
	int16_t y_max;
	uint8_t x_adv;
};

struct glyphs_cache {
	uint32_t glyphs_num;
	struct glyph *glyphs;
	float font_size;
};

struct font {
	uint32_t font_hash;
	FT_Face face;
	uint8_t glyphs_num; /* as many as font sizes in use */
	struct glyphs_cache *glyphs_cache;
	uint8_t kerning:1;
	const FT_Byte *font_data;
	uint16_t hdpi;
	uint16_t vdpi;
	struct bmp bmp; /* pre-allocated storage for rasterized glyph */
};

struct draw {
	struct xcb *xcb;
	uint32_t depth;
	struct glyph *glyph;
	int16_t x;
	struct text *text;
	struct font *font;
#ifdef DRAW_BY_PIXEL
	uint32_t px;
	xcb_point_t pt;
#endif
};

struct font_buf {
	uint8_t *data;
	uint32_t size;
};

static FT_Library fontlib_;
static struct font fontcache_[MAX_FONTS];

static inline uint32_t hash32(char const *s, size_t n)
{
	return n ? (hash32(s, n - 1) ^ s[n - 1]) * 16777619U : 2166136261U;
}

static inline int32_t fp26(int32_t val)
{
	return val >> 6;
}

static inline uint32_t getc_utf8(const char *str, uint16_t len, uint16_t at)
{
	uint16_t pos = 0;
	uint32_t ret = 0;
	uint8_t bytes = 0;
	uint32_t mask = 0xff;

	if (!len || at > len)
		return 0;

	do {
		uint8_t c = (uint8_t) *str;

		if ((c & 0x80) == 0) {
			if (c == 0)
				return 0;
			str++;
			bytes = 1;
			mask = 0x7f;
		} else if ((c & 0x20) == 0) {
			str += 2;
			bytes = 2;
			mask = 0x07ff;
		} else if ((c & 0x10) == 0) {
			str += 3;
			bytes = 3;
			mask = 0xffff;
		} else {
			str += 4;
			bytes = 4;
			mask = 0x1fffff;
		}

		if (pos == at) {
			if (bytes == 0 || bytes == 1)
				return c & mask;

			str -= bytes;

			for (; bytes > 0; --bytes)
				ret = (ret << 6) | (*str++ & 0x3f);

			return ret & mask;
		}

		pos++;
	} while (pos < len);

	return 0;
}

static int8_t calc_kerning(struct font *font, uint32_t prev_glyph_idx,
 uint32_t glyph_idx)
{
	FT_Vector delta;

	FT_Get_Kerning(font->face, prev_glyph_idx, glyph_idx,
	 FT_KERNING_UNFITTED, &delta);

	return fp26(delta.x);
}

static uint8_t resize_glyphs_cache(struct font *font, float font_size)
{
	size_t mem_size;
	size_t cur_size;
	uint8_t *ptr;
	void *new_glyphs_cache;

	if (font->glyphs_num >= MAX_FONT_SIZES) {
		ee("too many font sizes %u\n", font->glyphs_num);
		return 0;
	}

	cur_size = font->glyphs_num * sizeof(*font->glyphs_cache);
	mem_size = ++font->glyphs_num + sizeof(struct glyphs_cache);
	new_glyphs_cache = realloc(font->glyphs_cache, mem_size);
	if (!new_glyphs_cache) {
		ee("failed to allocate %zu bytes\n", mem_size);
		return 0;
	}

	font->glyphs_cache = new_glyphs_cache;
	ptr = (uint8_t *) font->glyphs_cache + cur_size;
	dd("memset %p sizeof %zu bytes (%zu)\n", ptr, mem_size - cur_size, cur_size);
	memset(ptr, 0, mem_size - cur_size);
	font->glyphs_cache[font->glyphs_num - 1].font_size = font_size;
	return 1;
}

static uint8_t resize_glyphs_array(struct glyphs_cache *glyphs_cache,
 uint32_t idx)
{
	size_t mem_size;
	size_t cur_size;
	uint8_t *ptr;
	void *new_glyphs;

	mem_size = ++idx * sizeof(*glyphs_cache->glyphs);
	new_glyphs = realloc(glyphs_cache->glyphs, mem_size);
	if (!new_glyphs) {
		ee("failed to allocate %zu bytes\n", mem_size);
		return 0;
	}

	glyphs_cache->glyphs = new_glyphs;
	cur_size = glyphs_cache->glyphs_num * sizeof(*glyphs_cache->glyphs);
	ptr = (uint8_t *) glyphs_cache->glyphs + cur_size;
	dd("memset %p sizeof %zu bytes (%zu)\n", ptr, mem_size - cur_size, cur_size);
	memset(ptr, 0, mem_size - cur_size);
	glyphs_cache->glyphs_num = idx;
	return 1;
}

static uint8_t make_glyph(struct font *font, struct glyphs_cache *glyphs_cache,
 uint32_t idx)
{
	struct glyph *glyph;
	FT_BBox bbox;
	FT_Glyph bmp;
	uint32_t flags;
	FT_Face face = font->face;
	float font_size = glyphs_cache->font_size;
	uint8_t realloc_bmp;

	dd("make glyph %u cache size %u font size %.02f\n", idx,
	 glyphs_cache->glyphs_num, glyphs_cache->font_size);

	if (FT_Set_Char_Size(face, 0, font_size * 64, font->hdpi, font->vdpi)) {
		ee("failed set char size for font hash %x\n", font->font_hash);
		return 0;
	}

	flags = FT_LOAD_RENDER | FT_LOAD_FORCE_AUTOHINT;
	if (FT_Load_Glyph(face, idx, flags))
		return 0;

	if (FT_Get_Glyph(face->glyph, &bmp))
		return 0;

	/* sanity check */
	if (bmp->format != FT_GLYPH_FORMAT_BITMAP) {
		FT_Done_Glyph(bmp);
		return 0;
	}

	FT_Glyph_Get_CBox(bmp, FT_GLYPH_BBOX_UNSCALED, &bbox);
	glyph = &glyphs_cache->glyphs[idx];
	glyph->y_min = fp26(bbox.yMin);
	glyph->y_max = fp26(bbox.yMax);
	glyph->bmp = (FT_BitmapGlyph) bmp;

	/* always advance with bigger value */
	if (fp26(face->glyph->advance.x) < (int32_t) glyph->bmp->bitmap.width)
		glyph->x_adv = glyph->bmp->bitmap.width;
	else
		glyph->x_adv = fp26(face->glyph->advance.x);

	realloc_bmp = 0;
	if (font->bmp.w < glyph->bmp->bitmap.width) {
		font->bmp.w = glyph->bmp->bitmap.width;
		realloc_bmp = 1;
	}

	if (font->bmp.h < glyph->bmp->bitmap.rows) {
		font->bmp.h = glyph->bmp->bitmap.rows;
		realloc_bmp = 1;
	}

	if (realloc_bmp) {
		uint16_t size = font->bmp.w * font->bmp.h * 4;
		if (!(font->bmp.data = realloc(font->bmp.data, size))) {
			ee("failed to allocate %u bytes\n", size);
			return 0;
		}
	}

	return 1;
}

static struct glyphs_cache *find_glyphs_cache(struct font *font,
 float font_size)
{
	for (uint8_t i = 0; i < font->glyphs_num; ++i) {
		if (font->glyphs_cache[i].font_size == font_size)
			return &font->glyphs_cache[i];
	}

	return NULL;
}

static uint8_t cache_glyph(struct font *font, float font_size, uint32_t idx)
{
	struct glyphs_cache *glyphs_cache;

	/* init cache line with given font size */
	if (!font->glyphs_num && !resize_glyphs_cache(font, font_size)) {
		return 0;
	}

	/* find cache entry with given font size ... */
	glyphs_cache = find_glyphs_cache(font, font_size);

	/* ... if not found create new cache entry with given font size */
	if (!glyphs_cache && !resize_glyphs_cache(font, font_size)) {
		return 0;
	}

	glyphs_cache = &font->glyphs_cache[font->glyphs_num - 1];
	dd("cache glyph %u cache size %u font size %.02f\n", idx,
	 glyphs_cache->glyphs_num, glyphs_cache->font_size);
	if (glyphs_cache->glyphs_num <= idx) {
		if (!resize_glyphs_array(glyphs_cache, idx)) {
			return 0;
		}
	}

	return make_glyph(font, glyphs_cache, idx);
}

static void measure_text(struct text *text)
{
	uint16_t len = 0;
	uint16_t prev_w_inc = 0;
	uint16_t prev_bmp_w = 0;
	uint32_t c;
	int16_t y_min = INT16_MAX;
	uint32_t prev_idx = 0;
	struct font *font = &fontcache_[text->font_id];
	struct glyphs_cache *glyphs_cache;

	text->w = 0;
	text->y_max = 0;

	/* NOTE: risky assumption here is that FT_Get_Char_Index always returns
	 * values that are within face->num_glyphs range */
	for (uint16_t i = 0; (c = getc_utf8(text->str, text->len, i)); ++i) {
		int8_t kerning;
		struct glyph *glyph;
		uint32_t idx = FT_Get_Char_Index(font->face, c);

		dd("glyph U+%04X idx %u\n", c, idx);

		glyphs_cache = find_glyphs_cache(font, text->font_size);
		if (!glyphs_cache) {
			if (!cache_glyph(font, text->font_size, idx)) {
				ee("failed to cache glyph U+%04X idx %u\n", c, idx);
				continue;
			}
		}

		/* search again */
		glyphs_cache = find_glyphs_cache(font, text->font_size);
		if (!glyphs_cache) {
			if (!cache_glyph(font, text->font_size, idx)) {
				ee("failed to cache glyph U+%04X idx %u\n", c, idx);
				continue;
			}
		} else if (glyphs_cache->glyphs_num <= idx) {
			if (!cache_glyph(font, text->font_size, idx)) {
				ee("failed to cache glyph U+%04X idx %u\n", c, idx);
				continue;
			}
		}

		glyph = &glyphs_cache->glyphs[idx];
		if (!glyph->bmp) {
			if (!cache_glyph(font, text->font_size, idx)) {
				ee("failed to cache glyph U+%04X idx %u\n", c, idx);
				continue;
			}
		}

		dd("glyph %u bmp %p yyx (%d %d %u) glyphs cache size %u font size %.02f\n",
		  idx, glyph->bmp, glyph->y_min, glyph->y_max, glyph->x_adv,
		  glyphs_cache->glyphs_num, glyphs_cache->font_size);

		if (font->kerning && prev_idx)
			kerning = calc_kerning(font, prev_idx, idx);
		else
			kerning = 0;

		if (text->y_max < glyph->y_max)
			text->y_max = glyph->y_max;

		if (y_min > glyph->y_min)
			y_min = glyph->y_min;

		if (text->y_max_max < text->y_max)
			text->y_max_max = text->y_max;

		prev_bmp_w = glyph->bmp->bitmap.width;
		prev_w_inc = glyph->x_adv + kerning;
		text->w += prev_w_inc;
		len++;
		prev_idx = idx;
	}

	if (text->w >= prev_w_inc) {
		text->w -= prev_w_inc;
		text->w += prev_bmp_w;
	}

	text->h = text->y_max + abs(y_min);
}

static uint8_t read_font_file(const char *path, struct font_buf *buf)
{
	FILE *f = fopen(path, "r");

	if (!f) {
		ee("failed to open file '%s'\n", path);
		return 0;
	}

	buf->data = NULL;
	fseek(f, 0L, SEEK_END);
	if ((buf->size = ftell(f)) <= 0) {
		ee("bad file size %u\n", buf->size);
		goto err;
	}
	fseek(f, 0L, SEEK_SET);

	if (!(buf->data = (uint8_t *) malloc(buf->size))) {
		ee("failed to allocate %u bytes\n", buf->size);
		goto err;
	}

	if (fread(buf->data, 1, buf->size, f) != buf->size) {
		ee("failed to read %u bytes\n", buf->size);
		goto err;
	}

	fclose(f);
	return 1;
err:
	fclose(f);
	free(buf->data);
	buf->data = NULL;
	return 0;
}

#ifdef DRAW_BY_PIXEL
static inline void draw_pixel(struct draw *d)
{
	xcb_change_gc(d->xcb->dpy, d->xcb->gc, XCB_GC_FOREGROUND, &d->px);
	xcb_poly_point(d->xcb->dpy, XCB_COORD_MODE_ORIGIN, d->xcb->win,
	 d->xcb->gc, 1, &d->pt);
}
#else
static inline void draw_bmp(struct draw *d, struct bmp *bmp, uint16_t y)
{
	xcb_image_t *img;
	xcb_pixmap_t pix = xcb_generate_id(d->xcb->dpy);

	if (!pix) {
		ee("failed to create pixmap from bmp %p wh (%u %u)\n",
		 bmp->data, bmp->w, bmp->h);
		return;
	}

	xcb_create_pixmap(d->xcb->dpy, d->depth, pix, d->xcb->win, bmp->w,
	 bmp->h);
	img = xcb_image_create_native(d->xcb->dpy, bmp->w, bmp->h,
	 XCB_IMAGE_FORMAT_Z_PIXMAP, d->depth, bmp->data, bmp->w * bmp->h * 4,
	 bmp->data);

	if (!img) {
		ee("failed to create image from bmp %p wh (%u %u)\n",
		 bmp->data, bmp->w, bmp->h);
	} else {
		xcb_image_put(d->xcb->dpy, pix, d->xcb->gc, img, 0, 0, 0);
		xcb_copy_area(d->xcb->dpy, pix, d->xcb->win, d->xcb->gc, 0, 0,
		 d->x, y, bmp->w, bmp->h);
		img->base = NULL; /* allow to reuse bmp memory */
		xcb_image_destroy(img);
	}

	xcb_free_pixmap(d->xcb->dpy, pix);
}
#endif

static inline uint8_t is_color_brighter(union rgb *c1, union rgb *c2)
{
	uint8_t y1 = 0;
	uint8_t y2 = 0;

	if (c1->r != c2->r)
		(c1->r > c2->r) ? (y1++) : (y2++);

	if (c1->g != c2->g)
		(c1->g > c2->g) ? (y1++) : (y2++);

	if (c1->b != c2->b)
		(c1->b > c2->b) ? (y1++) : (y2++);

	return y1 > y2;
}

static inline void draw_glyph(struct draw *d)
{
	int16_t y_offs = d->text->y + d->text->y_max - d->glyph->y_max;
	FT_Bitmap *bmp = &d->glyph->bmp->bitmap;
#ifndef SLOW_DRAW
	uint8_t *rgba_ptr = d->font->bmp.data;
	struct bmp buf;

	buf.w = bmp->width;
	buf.h = bmp->rows;
	buf.data = rgba_ptr;
#endif

	if (d->text->y_max_max > d->text->y_max)
		y_offs = d->text->y + d->text->y_max_max - d->glyph->y_max;

	for (uint16_t row = 0; row < bmp->rows; ++row) {
		for(uint16_t col = 0; col < bmp->width; ++col) {
			uint32_t offset = row * bmp->width + col;
			uint8_t c = *((uint8_t *) bmp->buffer + offset);
			union rgb rgb;

			if (d->text->fade) {
				int16_t cc = (int16_t) c - d->text->fade;
				(cc < 0) ? (c = 0) : (c = cc);
			}

			if (c == 0) {
#ifdef DRAW_BY_PIXEL
				d->px = d->text->bg.data;
#endif
				rgb.r = d->text->bg.r;
				rgb.g = d->text->bg.g;
				rgb.b = d->text->bg.b;
			} else {
				if (!d->text->bg_brighter) {
					rgb.r = c * d->text->fg.r / 255;
					rgb.g = c * d->text->fg.g / 255;
					rgb.b = c * d->text->fg.b / 255;

					if (rgb.r < d->text->bg.r)
						rgb.r = d->text->bg.r;
					if (rgb.g < d->text->bg.g)
						rgb.g = d->text->bg.g;
					if (rgb.b < d->text->bg.b)
						rgb.b = d->text->bg.b;
				} else {
					rgb.r = (255 - c) * d->text->bg.r / 255;
					rgb.g = (255 - c) * d->text->bg.g / 255;
					rgb.b = (255 - c) * d->text->bg.b / 255;

					if (rgb.r > d->text->bg.r) {
						rgb.r = d->text->bg.r;
						rgb.g = d->text->bg.r;
						rgb.b = d->text->bg.r;
					} else if (rgb.g > d->text->bg.g) {
						rgb.r = d->text->bg.g;
						rgb.g = d->text->bg.g;
						rgb.b = d->text->bg.g;
					} else if (rgb.b > d->text->bg.b) {
						rgb.r = d->text->bg.b;
						rgb.g = d->text->bg.b;
						rgb.b = d->text->bg.b;
					}
				}
#ifdef DRAW_BY_PIXEL
				d->px = rgb.r << 16 | rgb.g << 8 | rgb.b;
#endif
			}

//#define PRINT_GLYPHS
#ifdef PRINT_GLYPHS
			if (c != 0) {
				fprintf(stderr, "%02x", c);
			} else {
				fprintf(stderr, "..");
			}
#endif

#ifdef DRAW_BY_PIXEL
			d->pt.x = col + d->x;
			d->pt.y = row + y_offs;
			draw_pixel(d);
#else
			if (buf.data) {
				*rgba_ptr++ = rgb.b;
				*rgba_ptr++ = rgb.g;
				*rgba_ptr++ = rgb.r;
				*rgba_ptr++ = 0;
			}
#endif
		}
#ifdef PRINT_GLYPHS
		fprintf(stderr, "| bmp_wh (%u %u) y_minmax (%d %d) x_adv=%u text_h=%u\n",
		 bmp->width, bmp->rows, d->glyph->y_min, d->glyph->y_max,
		 d->glyph->x_adv, d->text->h);
#endif
	}
#ifndef DRAW_BY_PIXEL
	draw_bmp(d, &buf, y_offs);
#endif
}

void draw_text_xcb(struct xcb *xcb, struct text *text)
{
	xcb_screen_t *scr;
	uint32_t c;
	struct font *font;
	struct glyphs_cache *glyphs_cache;
	struct draw draw;
	uint32_t prev_idx = 0;

	/* sanity checks */
	if (!text || !text->str) {
		return;
	} else if (invalid_font_id(text->font_id)) {
		return;
	} else if (text->font_size > MAX_FONT_SIZE) {
		return;
	} else if (!text->y_max) {
		measure_text(text);
	}

	font = &fontcache_[text->font_id];
	if (!(glyphs_cache = find_glyphs_cache(font, text->font_size)))
		return;

	draw.x = text->x;
	draw.text = text;
	draw.xcb = xcb;
	scr = xcb_setup_roots_iterator(xcb_get_setup(xcb->dpy)).data;
	if (!scr) {
		ee("failed to get screen\n");
		return;
	}
	draw.depth = scr->root_depth;
	draw.font = font;

	for (uint16_t i = 0; (c = getc_utf8(text->str, text->len, i)); ++i) {
		int8_t kerning;
		uint32_t idx = FT_Get_Char_Index(font->face, c);

		dd("'%c' glyph U+%04X idx %u\n", c & 0xff, c, idx);
		if (idx == 0)
			continue;

		draw.glyph = &glyphs_cache->glyphs[idx];
		if (!draw.glyph->bmp && !cache_glyph(font, text->font_size, idx))
			continue;

		if (font->kerning && prev_idx)
			kerning = calc_kerning(font, prev_idx, idx);
		else
			kerning = 0;

		dd("[%u] glyph %u '%c' bmp %p yyx (%d %d %u) glyphs cache size %u font size %.02f\n",
		  i, idx, c & 0xff,
		  draw.glyph->bmp, draw.glyph->y_min, draw.glyph->y_max,
		  draw.glyph->x_adv,
		  glyphs_cache->glyphs_num, glyphs_cache->font_size);

		if (text->fade_idx && i > text->fade_idx)
			text->fade += text->fade_step;

		draw_glyph(&draw);
		draw.x += draw.glyph->x_adv + kerning;
		prev_idx = idx;
	}

	text->fade = 0;
	xcb_flush(xcb->dpy);
}

void set_text_fade(struct text *text, uint16_t glyph_idx)
{
	text->fade_idx = glyph_idx;

	if (text->len && text->len > glyph_idx)
		text->fade_step = 255 / (text->len - glyph_idx);
}

void set_text_font(struct text *text, fontid_t font_id, float font_size)
{
	if (text) {
		text->font_id = font_id;
		text->font_size = font_size;
		text->y_max = 0; /* reset measurements */
		text->y_max_max = 0;
	}
}

void set_text_str(struct text *text, const char *str, uint16_t len)
{
	if (text) {
		text->str = str;
		text->len = len;
		text->y_max = 0; /* reset measurements */

		if (text->fade_idx && text->len > text->fade_idx)
			text->fade_step = 255 / (text->len - text->fade_idx);
	}
}

void set_text_pos(struct text *text, int16_t x, int16_t y)
{
	if (text) {
		text->x = x;
		text->y = y;
	}
}

void set_text_color(struct text *text, uint32_t fg, uint32_t bg)
{
	if (text) {
		text->fg.data = fg;
		text->bg.data = bg;
		text->bg_brighter = is_color_brighter(&text->bg, &text->fg);
	}
}

void get_text_size(struct text *text, uint16_t *w, uint16_t *h)
{
	if (text && w && h) {
		if (!text->str) {
			return;
		} else if (invalid_font_id(text->font_id)) {
			ee("invalid font id (%d)\n", text->font_id);
		} else if (text->font_size > MAX_FONT_SIZE) {
			ee("font %d size %.02f is out of %u range\n",
			 text->font_id, text->font_size, MAX_FONT_SIZE);
		} else if (text->str) {
			measure_text(text);
			*w = text->w;
			*h = text->h;
		}
	}
}

void destroy_text(struct text **text)
{
	if (text) {
		free(*text);
		*text = NULL;
	}
}

struct text *create_text(void)
{
	struct text *text = calloc(1, sizeof(struct text));

	if (!text)
		return NULL;

	text->font_id = INVALID_FONT_ID;
	text->fade = 0;
	text->fade_idx = 0;
	return text;
}

void close_font(fontid_t font_id)
{
	struct font *font;

	if (invalid_font_id(font_id))
		return;

	font = &fontcache_[font_id];
	for (uint8_t i = 0; i < font->glyphs_num; ++i) {
		struct glyphs_cache *glyphs_cache = &font->glyphs_cache[i];

		for (uint32_t n = 0; n < glyphs_cache->glyphs_num; ++n) {
			struct glyph *glyph = &glyphs_cache->glyphs[n];

			if (glyph->bmp) {
				FT_Done_Glyph((FT_Glyph) glyph->bmp);
				glyph->bmp = NULL;
			}
		}

		free(glyphs_cache->glyphs);
	}

	free(font->bmp.data);
	font->bmp.data = NULL;
	free(font->glyphs_cache);
	font->glyphs_cache = NULL;
	font->glyphs_num = 0;
	font->font_hash = 0;
}

fontid_t open_font(const char *path, uint16_t hdpi, uint16_t vdpi)
{
	uint32_t font_hash;
	fontid_t font_id;
	size_t path_len;
	struct font *font;
	FT_Error ft_err;
	struct font_buf font_buf;

	if (!path) {
		ee("font file path is not set\n");
		return INVALID_FONT_ID;
	} else if (!fontlib_) {
		if (FT_Init_FreeType(&fontlib_)) {
			ee("failed to init freetype library\n");
			return INVALID_FONT_ID;
		}
	}

	path_len = strnlen(path, MAX_PATH);
	if (path_len == MAX_PATH) {
		ee("path '%s' is too long, max %u\n", path, MAX_PATH);
		return INVALID_FONT_ID;
	}

	font_hash = hash32(path, path_len);
	font_id = INVALID_FONT_ID;
	for (uint8_t i = 0; i < MAX_FONTS; ++i) {
		if (!fontcache_[i].face) {
			font_id = i;
		} else if (fontcache_[i].font_hash == font_hash) {
			ww("font '%s' already registered\n", path);
			return i;
		}
	}

	if (font_id == INVALID_FONT_ID) {
		ww("only %u fonts can be registered\n", MAX_FONTS);
		return INVALID_FONT_ID;
	} else if (!read_font_file(path, &font_buf)) {
		return INVALID_FONT_ID;
	}

	font = &fontcache_[font_id];
	ft_err = FT_New_Memory_Face(fontlib_, (const FT_Byte*) font_buf.data,
	 (FT_Long) font_buf.size, 0, &font->face);

	if (ft_err == FT_Err_Unknown_File_Format) {
		ee("failed to open '%s', unknown file format\n", path);
		return INVALID_FONT_ID;
	} else if (ft_err) {
		ee("failed to use '%s'\n", path);
		return INVALID_FONT_ID;
	} else if (font->face->num_glyphs <= 0) {
		ee("font '%s' has %ld glyphs\n", path, font->face->num_glyphs);
		return INVALID_FONT_ID;
	}

	font->hdpi = hdpi;
	font->vdpi = vdpi;
	font->font_hash = font_hash;
	font->font_data = font_buf.data;
	font->kerning = FT_HAS_KERNING(font->face);

	dd("registered font %d '%s' hash %x with %ld glyphs, kerning %u\n",
	 font_id, path, font->font_hash, font->face->num_glyphs, font->kerning);

	return font_id;
}

