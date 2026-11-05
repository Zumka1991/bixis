#ifndef BIXIS_FONT_H
#define BIXIS_FONT_H

#include <stdint.h>

enum { STYLE_BOLD = 1, STYLE_ITALIC = 2 };

struct glyph {
	int left, top;      /* bearing relative to pen position / baseline */
	int w, h;
	uint8_t *bmp;       /* 8-bit coverage, pitch == w; NULL = nothing to draw */
};

struct font {
	int cw, ch;         /* cell size in pixels */
	int baseline;       /* from the top of the cell */
	int ul_pos, ul_thick;
	struct font_impl *impl;
};

/* size is in points; scale is the integer buffer scale */
struct font *font_open(const char *family, double size, int scale);
void font_close(struct font *f);
const struct glyph *font_glyph(struct font *f, uint32_t cp, int style);

#endif
