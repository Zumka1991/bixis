/* Software renderer into wl_shm: repaint cell spans, track surface damage
 * independently from the selected buffer's age. */
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
                 int clip0, int clip1, int clipx0, int clipx1, uint32_t fg)
{
	clip0 = MAX(clip0, 0);
	clip1 = MIN(clip1, cv->h);
	int gx0 = MAX(0, MAX(clipx0, 0) - x);
	int gx1 = MIN(g->w, MIN(clipx1, cv->w) - x);
	int gy0 = MAX(0, clip0 - y), gy1 = MIN(g->h, clip1 - y);
	if (gx0 >= gx1 || gy0 >= gy1)
		return;
	fg |= 0xff000000;

	for (int gy = gy0; gy < gy1; gy++) {
		uint32_t *row = cv->px + (size_t)(y + gy) * cv->w;
		const uint8_t *m = g->bmp + gy * g->w;
		for (int gx = gx0; gx < gx1; gx++) {
			unsigned a = m[gx];
			if (!a)
				continue;
			row[x + gx] = a == 255 ? fg : blend(row[x + gx], fg, a);
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

static void row_rect(struct canvas *cv, int x, int y, int w, int h,
                     int clip0, int clip1, uint32_t color)
{
	int left = MAX(x, clip0), right = MIN(x + w, clip1);
	if (left < right)
		rect(cv, left, y, right - left, h, color);
}

static void draw_row(struct canvas *cv, struct font *font, const struct theme *th,
                     const struct cell *row, int cols, int x0, int y0, uint32_t pad_bg,
                     int clip0, int clip1)
{
	int cw = font->cw, ch = font->ch;
	int thick = MAX(2, font->ul_thick * 2);
	uint32_t fg, bg;

	/* padding first: glyphs may overhang into it */
	row_rect(cv, 0, y0, x0, ch, clip0, clip1, pad_bg);
	row_rect(cv, x0 + cols * cw, y0, cv->w, ch, clip0, clip1, pad_bg);

	/* Fill runs of equal background in scanline order, instead of narrow cells. */
	int run = 0;
	uint32_t run_bg = 0;
	for (int x = 0; x < cols; x++) {
		const struct cell *c = &row[x];
		if ((c->attr & ATTR_WDUMMY) && x > 0 && (row[x - 1].attr & ATTR_WIDE))
			continue;
		colors(c, th, &fg, &bg);
		if (x > 0 && bg != run_bg) {
			row_rect(cv, x0 + run * cw, y0, (x - run) * cw, ch, clip0, clip1, run_bg);
			run = x;
		}
		run_bg = bg;
	}
	row_rect(cv, x0 + run * cw, y0, (cols - run) * cw, ch, clip0, clip1, run_bg);

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
				blit(cv, g, px + g->left, y0 + font->baseline - g->top,
				     y0, y0 + ch, clip0, clip1, fg);
		}
		if (c->attr & ATTR_UNDERLINE)
			row_rect(cv, px, y0 + font->ul_pos, w * cw, font->ul_thick, clip0, clip1, fg);
		if (c->attr & ATTR_STRIKE)
			row_rect(cv, px, y0 + font->baseline - font->baseline * 3 / 10,
			         w * cw, font->ul_thick, clip0, clip1, fg);

		if (!(c->attr & ATTR_CURSOR))
			continue;
		switch (c->_pad) {
		case CUR_FOCUSED(CURSOR_UNDERLINE):
			row_rect(cv, px, y0 + ch - thick, w * cw, thick, clip0, clip1, th->cursor);
			break;
		case CUR_FOCUSED(CURSOR_BAR):
			row_rect(cv, px, y0, thick, ch, clip0, clip1, th->cursor);
			break;
		case CUR_UNFOCUSED:
			row_rect(cv, px, y0, w * cw, 1, clip0, clip1, th->cursor);
			row_rect(cv, px, y0 + ch - 1, w * cw, 1, clip0, clip1, th->cursor);
			row_rect(cv, px, y0, 1, ch, clip0, clip1, th->cursor);
			row_rect(cv, px + w * cw - 1, y0, 1, ch, clip0, clip1, th->cursor);
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

/* Include both old and new overhangs, including the leading half of wide text.
 * Unchanged neighbours are drawn too, clipped to this span, so overlapping
 * italic/fallback glyphs are restored exactly as in a complete row redraw. */
static bool changed_span(const struct cell *old, const struct cell *line, int cols,
                         struct font *font, int pad, int width, int *x0, int *x1)
{
	if (!memcmp(old, line, cols * sizeof *line))
		return false;
	int first = 0, end = cols;
	while (first < end && !memcmp(&old[first], &line[first], sizeof *line))
		first++;
	if (first == end)
		return false;
	while (end > first && !memcmp(&old[end - 1], &line[end - 1], sizeof *line))
		end--;
	*x0 = pad + first * font->cw;
	*x1 = pad + end * font->cw;
	for (int v = 0; v < 2; v++) {
		const struct cell *row = v ? line : old;
		for (int x = first; x < end; x++) {
			int lead = (row[x].attr & ATTR_WDUMMY) && x > 0 ? x - 1 : x;
			const struct cell *c = &row[lead];
			int px = pad + lead * font->cw;
			int w = (c->attr & ATTR_WIDE) && lead + 1 < cols ? 2 : 1;
			*x0 = MIN(*x0, px);
			*x1 = MAX(*x1, px + w * font->cw);
			if (c->cp <= ' ' || (c->attr & (ATTR_INVISIBLE | ATTR_WDUMMY)))
				continue;
			int style = (c->attr & ATTR_BOLD ? STYLE_BOLD : 0) |
			            (c->attr & ATTR_ITALIC ? STYLE_ITALIC : 0);
			const struct glyph *g = font_glyph(font, c->cp, style);
			*x0 = MIN(*x0, px + g->left);
			*x1 = MAX(*x1, px + g->left + g->w);
		}
	}
	*x0 = CLAMP(*x0, 0, width);
	*x1 = CLAMP(*x1, 0, width);
	return *x0 < *x1;
}

static void damage_add(struct damage *damage, int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0)
		return;
	if (damage->len) {
		struct damage_rect *last = &damage->rects[damage->len - 1];
		if (last->x == x && last->w == w && last->y + last->h == y) {
			last->h += h;
			return;
		}
	}
	if (damage->len == damage->cap) {
		damage->cap = damage->cap ? damage->cap * 2 : 16;
		damage->rects = xrealloc(damage->rects, damage->cap * sizeof *damage->rects);
	}
	damage->rects[damage->len++] = (struct damage_rect){ x, y, w, h };
}

static bool snapshot_matches(const struct snapshot *s, const struct canvas *cv,
                              const struct font *font, const struct frame *fr)
{
	bool bar = fr->tabbar != NULL;
	return s && s->valid && s->cols == fr->t->cols && s->rows == fr->t->rows + bar &&
	       s->bar == bar && s->w == cv->w && s->h == cv->h && s->pad == fr->pad &&
	       s->cw == font->cw && s->ch == font->ch;
}

void render(struct canvas *cv, struct snapshot *snap, struct font *font,
            const struct theme *th, const struct frame *fr,
            const struct snapshot *presented, struct damage *damage)
{
	static struct cell *line;
	static int line_cap;

	struct term *t = fr->t;
	int bar = fr->tabbar != NULL;
	int cols = t->cols, rows = t->rows + bar;
	int ch = font->ch;

	bool repaint = !snapshot_matches(snap, cv, font, fr);
	bool full_damage = !snapshot_matches(presented, cv, font, fr);
	damage->len = 0;
	if (full_damage)
		damage_add(damage, 0, 0, cv->w, cv->h);
	if (repaint) {
		snap->cells = xrealloc(snap->cells, (size_t)cols * rows * sizeof *snap->cells);
		snap->cols = cols;
		snap->rows = rows;
		snap->bar = bar;
		snap->w = cv->w;
		snap->h = cv->h;
		snap->cw = font->cw;
		snap->ch = ch;
		snap->pad = fr->pad;
		snap->valid = true;
		rect(cv, 0, 0, cv->w, cv->h, th->bg);
		if (bar)
			rect(cv, 0, 0, cv->w, ch, th->tab_bg);
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
		int x0, x1;
		bool changed = false;
		/* Must precede writes to s: presented and snap can be the same object. */
		if (!full_damage)
			changed = changed_span(presented->cells + (size_t)r * cols, line,
			                       cols, font, fr->pad, cv->w, &x0, &x1);
		if (changed) {
			int y0 = MAX(py, 0), y1 = MIN(py + ch, cv->h);
			damage_add(damage, x0, y0, x1 - x0, y1 - y0);
		}
		if (repaint) {
			x0 = 0;
			x1 = cv->w;
		} else if (presented == snap) {
			if (!changed)
				continue;
		} else if (!changed_span(s, line, cols, font, fr->pad, cv->w, &x0, &x1)) {
			continue;
		}
		draw_row(cv, font, th, line, cols, fr->pad, py, pad_bg, x0, x1);
		memcpy(s, line, cols * sizeof *line);
	}
}
