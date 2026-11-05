/* Software renderer into a wl_shm buffer. Only rows that differ from what the
 * buffer already shows are redrawn. */
#include "render.h"
#include "util.h"

#include <string.h>

#define CUR_FOCUSED(shape) (1 + (shape))
#define CUR_UNFOCUSED      0x10

static void rect(struct canvas *cv, int x, int y, int w, int h, uint32_t c)
{
	int x0 = MAX(x, 0), y0 = MAX(y, 0);
	int x1 = MIN(x + w, cv->w), y1 = MIN(y + h, cv->h);

	c |= 0xff000000;
	for (int yy = y0; yy < y1; yy++) {
		uint32_t *p = cv->px + (size_t)yy * cv->w;
		for (int xx = x0; xx < x1; xx++)
			p[xx] = c;
	}
}

static inline uint32_t blend(uint32_t d, uint32_t s, unsigned a)
{
	unsigned ia = 255 - a;
	uint32_t rb = (((s & 0xff00ff) * a + (d & 0xff00ff) * ia) >> 8) & 0xff00ff;
	uint32_t g = (((s & 0x00ff00) * a + (d & 0x00ff00) * ia) >> 8) & 0x00ff00;
	return 0xff000000 | rb | g;
}

static void blit(struct canvas *cv, const struct glyph *g, int x, int y,
                 int clip0, int clip1, uint32_t fg)
{
	clip0 = MAX(clip0, 0);
	clip1 = MIN(clip1, cv->h);
	fg |= 0xff000000;

	for (int gy = 0; gy < g->h; gy++) {
		int py = y + gy;
		if (py < clip0 || py >= clip1)
			continue;
		uint32_t *row = cv->px + (size_t)py * cv->w;
		const uint8_t *m = g->bmp + gy * g->w;
		for (int gx = 0; gx < g->w; gx++) {
			int px = x + gx;
			unsigned a = m[gx];
			if (!a || px < 0 || px >= cv->w)
				continue;
			row[px] = a == 255 ? fg : blend(row[px], fg, a);
		}
	}
}

static void colors(const struct cell *c, const struct theme *th, uint32_t *fg, uint32_t *bg)
{
	uint32_t f = c->fg, b = c->bg;

	if (c->attr & ATTR_REVERSE) {
		uint32_t tmp = f;
		f = b;
		b = tmp;
	}
	if (c->attr & ATTR_SELECTED) {
		f = th->sel_fg;
		b = th->sel_bg;
	}
	if ((c->attr & ATTR_CURSOR) && c->_pad == CUR_FOCUSED(CURSOR_BLOCK)) {
		f = th->cursor_text;
		b = th->cursor;
	}
	if (c->attr & ATTR_DIM)
		f = blend(b, f, 160) & 0xffffff;
	if (c->attr & ATTR_INVISIBLE)
		f = b;
	*fg = f;
	*bg = b;
}

static void draw_row(struct canvas *cv, struct font *font, const struct theme *th,
                     const struct cell *row, int cols, int x0, int y0, uint32_t pad_bg)
{
	int cw = font->cw, ch = font->ch;
	int thick = MAX(2, font->ul_thick * 2);
	uint32_t fg, bg;

	/* padding first: glyphs may overhang into it */
	rect(cv, 0, y0, x0, ch, pad_bg);
	rect(cv, x0 + cols * cw, y0, cv->w, ch, pad_bg);

	for (int x = 0; x < cols; x++) {
		const struct cell *c = &row[x];
		if ((c->attr & ATTR_WDUMMY) && x > 0 && (row[x - 1].attr & ATTR_WIDE))
			continue;
		int w = (c->attr & ATTR_WIDE) && x + 1 < cols ? 2 : 1;
		colors(c, th, &fg, &bg);
		rect(cv, x0 + x * cw, y0, w * cw, ch, bg);
	}

	for (int x = 0; x < cols; x++) {
		const struct cell *c = &row[x];
		if (c->attr & ATTR_WDUMMY)
			continue;
		int px = x0 + x * cw;
		int w = (c->attr & ATTR_WIDE) && x + 1 < cols ? 2 : 1;
		colors(c, th, &fg, &bg);

		if (c->cp > ' ' && !(c->attr & ATTR_INVISIBLE)) {
			int style = (c->attr & ATTR_BOLD ? STYLE_BOLD : 0) |
			            (c->attr & ATTR_ITALIC ? STYLE_ITALIC : 0);
			const struct glyph *g = font_glyph(font, c->cp, style);
			if (g->bmp)
				blit(cv, g, px + g->left, y0 + font->baseline - g->top, y0, y0 + ch, fg);
		}
		if (c->attr & ATTR_UNDERLINE)
			rect(cv, px, y0 + font->ul_pos, w * cw, font->ul_thick, fg);
		if (c->attr & ATTR_STRIKE)
			rect(cv, px, y0 + font->baseline - font->baseline * 3 / 10, w * cw, font->ul_thick, fg);

		if (!(c->attr & ATTR_CURSOR))
			continue;
		switch (c->_pad) {
		case CUR_FOCUSED(CURSOR_UNDERLINE):
			rect(cv, px, y0 + ch - thick, w * cw, thick, th->cursor);
			break;
		case CUR_FOCUSED(CURSOR_BAR):
			rect(cv, px, y0, thick, ch, th->cursor);
			break;
		case CUR_UNFOCUSED:
			rect(cv, px, y0, w * cw, 1, th->cursor);
			rect(cv, px, y0 + ch - 1, w * cw, 1, th->cursor);
			rect(cv, px, y0, 1, ch, th->cursor);
			rect(cv, px + w * cw - 1, y0, 1, ch, th->cursor);
			break;
		}
	}
}

static void build_row(struct cell *out, const struct term *t, int r,
                      const struct frame *fr, const struct theme *th)
{
	int64_t abs = term_view_top(t) + r;
	int len = 0;
	const struct cell *src = term_abs_line(t, abs, &len);
	int n = src ? MIN(len, t->cols) : 0;
	struct cell b = { .fg = th->fg, .bg = th->bg };

	if (n)
		memcpy(out, src, n * sizeof *out);
	for (int x = n; x < t->cols; x++)
		out[x] = b;

	if (t->sel_active)
		for (int x = 0; x < t->cols; x++)
			if (term_cell_selected(t, abs, x))
				out[x].attr |= ATTR_SELECTED;

	if (t->view == 0 && t->cursor_visible && r == t->cy) {
		struct cell *c = &out[t->cx];
		if ((c->attr & ATTR_WDUMMY) && t->cx > 0)
			c--;
		c->attr |= ATTR_CURSOR;
		c->_pad = fr->focused ? CUR_FOCUSED(t->cursor_shape) : CUR_UNFOCUSED;
	}
}

void render(struct canvas *cv, struct snapshot *snap, struct font *font,
            const struct theme *th, const struct frame *fr, int *y0, int *y1)
{
	static struct cell *line;
	static int line_cap;

	struct term *t = fr->t;
	int bar = fr->tabbar != NULL;
	int cols = t->cols, rows = t->rows + bar;
	int ch = font->ch;

	*y0 = cv->h;
	*y1 = 0;

	if (!snap->valid || snap->cols != cols || snap->rows != rows) {
		free(snap->cells);
		snap->cells = xmalloc((size_t)cols * rows * sizeof *snap->cells);
		memset(snap->cells, 0xff, (size_t)cols * rows * sizeof *snap->cells);
		snap->cols = cols;
		snap->rows = rows;
		snap->valid = true;
		rect(cv, 0, 0, cv->w, cv->h, th->bg);
		if (bar)
			rect(cv, 0, 0, cv->w, ch, th->tab_bg);
		*y0 = 0;
		*y1 = cv->h;
	}
	if (line_cap < cols) {
		line_cap = cols;
		line = xrealloc(line, line_cap * sizeof *line);
	}

	int grid_y = (bar ? ch : 0) + fr->pad;
	for (int r = 0; r < rows; r++) {
		int py;
		uint32_t pad_bg = th->bg;

		if (bar && r == 0) {
			memcpy(line, fr->tabbar, cols * sizeof *line);
			py = 0;
			pad_bg = th->tab_bg;
		} else {
			build_row(line, t, r - bar, fr, th);
			py = grid_y + (r - bar) * ch;
		}

		struct cell *s = snap->cells + (size_t)r * cols;
		if (!memcmp(line, s, cols * sizeof *line))
			continue;
		draw_row(cv, font, th, line, cols, fr->pad, py, pad_bg);
		memcpy(s, line, cols * sizeof *line);
		*y0 = MIN(*y0, py);
		*y1 = MAX(*y1, py + ch);
	}
	*y0 = MAX(*y0, 0);
	*y1 = MIN(*y1, cv->h);
}
