/* Incremental pixels and damage must agree with a complete repaint. No display. */
#include "render.h"
#include "util.h"

#include <string.h>

static int fails;
#define CHECK(c) do { \
	if (!(c)) { \
		fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #c); \
		fails++; \
	} \
} while (0)

/* Deliberately overhang several neighbouring cells; include alpha blending. */
const struct glyph *font_glyph(struct font *font, uint32_t cp, int style)
{
	(void)font;
	static uint8_t bitmap[32 * 12];
	static struct glyph glyph;
	for (size_t i = 0; i < sizeof bitmap; i++)
		bitmap[i] = i % 3 == 0 ? 0 : i % 3 == 1 ? 128 : 255;
	glyph = (struct glyph){ .w = cp == 'A' ? 30 : 10 + style,
	                        .h = 10, .left = cp == 'A' ? -10 : -1,
	                        .top = 9, .bmp = bitmap };
	return &glyph;
}

static bool covered(const struct damage *d, int x, int y)
{
	for (int i = 0; i < d->len; i++) {
		const struct damage_rect *r = &d->rects[i];
		if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h)
			return true;
	}
	return false;
}

static struct snapshot snaps[2], reference;
static struct canvas canvases[2], ref;
static struct damage damage, ref_damage;
static uint32_t *shown;
static int last = -1;

static void frame(int at, struct font *font, const struct theme *theme, struct frame *fr)
{
	reference.valid = false;
	render(&ref, &reference, font, theme, fr, NULL, &ref_damage);
	render(&canvases[at], &snaps[at], font, theme, fr,
	       last < 0 ? NULL : &snaps[last], &damage);
	size_t size = (size_t)ref.w * ref.h * sizeof *ref.px;
	CHECK(!memcmp(canvases[at].px, ref.px, size));
	for (int i = 0; i < damage.len; i++) {
		const struct damage_rect *r = &damage.rects[i];
		CHECK(r->x >= 0 && r->y >= 0 && r->w > 0 && r->h > 0);
		CHECK(r->x + r->w <= ref.w && r->y + r->h <= ref.h);
	}
	if (last >= 0) {
		bool correct = true;
		for (int y = 0; y < ref.h; y++)
			for (int x = 0; x < ref.w; x++)
				if (shown[y * ref.w + x] != ref.px[y * ref.w + x] && !covered(&damage, x, y))
					correct = false;
		CHECK(correct);
	}
	if (damage.len) {
		memcpy(shown, ref.px, size);
		last = at;
	}
}

int main(void)
{
	uint32_t palette[16] = {0};
	term_setup(palette, 0xd0d0d0, 0x161616, 0);
	struct term t = { .cols = 8, .rows = 5 };
	t.screen = xcalloc(t.rows, sizeof *t.screen);
	for (int y = 0; y < t.rows; y++) {
		t.screen[y] = xcalloc(t.cols, sizeof **t.screen);
		for (int x = 0; x < t.cols; x++)
			t.screen[y][x] = (struct cell){ .cp = 'x', .fg = 0xd0d0d0, .bg = 0x161616 };
	}
	struct font font = { .cw = 8, .ch = 12, .baseline = 10, .ul_pos = 11, .ul_thick = 1 };
	struct theme theme = { .fg = 0xd0d0d0, .bg = 0x161616, .cursor = 0xffffff,
	                       .cursor_text = 0, .sel_fg = 0xffffff, .sel_bg = 0x234567 };
	struct frame fr = { .t = &t, .focused = true, .pad = 3 };
	ref = (struct canvas){ .w = 74, .h = 80, .px = xcalloc(74 * 80, 4) };
	shown = xcalloc(74 * 80, 4);
	for (int i = 0; i < 2; i++)
		canvases[i] = (struct canvas){ .w = ref.w, .h = ref.h, .px = xcalloc(74 * 80, 4) };

	frame(0, &font, &theme, &fr);
	t.screen[0][0].cp = 'B';
	frame(1, &font, &theme, &fr);
	t.screen[0][0].cp = 'x';
	frame(0, &font, &theme, &fr);
	CHECK(damage.len > 0); /* A -> B -> A still needs a surface commit. */
	frame(1, &font, &theme, &fr);
	CHECK(damage.len == 0); /* Repairing an old buffer alone needs no commit. */
	t.screen[0][3].cp = 'D';
	t.screen[4][3].cp = 'D';
	frame(0, &font, &theme, &fr);
	CHECK(damage.len == 2);
	CHECK(!covered(&damage, 30, 30)); /* Do not damage unchanged middle rows. */
	CHECK(damage.rects[0].w < ref.w);

	/* Replacement/removal of overhangs and a cursor on either half of a wide glyph. */
	t.screen[1][3].cp = 'A';
	frame(1, &font, &theme, &fr);
	t.screen[1][3].cp = 0;
	frame(0, &font, &theme, &fr);
	t.screen[2][3].attr = ATTR_WIDE | ATTR_ITALIC;
	t.screen[2][4].attr = ATTR_WDUMMY;
	t.screen[2][4].cp = 0;
	t.cursor_visible = true;
	t.cy = 2;
	t.cx = 4;
	for (int shape = CURSOR_BLOCK; shape <= CURSOR_BAR; shape++) {
		t.cursor_shape = shape;
		frame(shape % 2, &font, &theme, &fr);
	}
	fr.focused = false;
	frame(0, &font, &theme, &fr);
	t.sel_active = true;
	t.sel_a_line = 1;
	t.sel_b_line = 3;
	t.sel_a_col = 2;
	t.sel_b_col = 6;
	frame(1, &font, &theme, &fr);
	t.sel_active = false;
	t.cx = 0;
	frame(0, &font, &theme, &fr);

	struct cell bar[8] = {0};
	fr.tabbar = bar;
	frame(1, &font, &theme, &fr);
	CHECK(damage.len == 1 && damage.rects[0].h == ref.h);
	fr.tabbar = NULL;
	fr.pad = 5;
	frame(0, &font, &theme, &fr);
	CHECK(damage.len == 1 && damage.rects[0].h == ref.h);

	/* Reproducible edits exercise stale buffers, blending, backgrounds and styles. */
	uint32_t random = 12345;
	for (int i = 0; i < 500; i++) {
		random = random * 1664525u + 1013904223u;
		int y = random % t.rows, x = (random >> 8) % t.cols;
		struct cell *c = &t.screen[y][x];
		*c = (struct cell){ .cp = (random & 1) ? 'A' : 'B',
		                   .fg = random & 0xffffff, .bg = (random >> 4) & 0xffffff,
		                   .attr = (random >> 16) & 0x7f };
		frame((random >> 24) & 1, &font, &theme, &fr);
	}
	for (int i = 0; i < 2; i++) {
		free(canvases[i].px);
		free(snaps[i].cells);
	}
	free(ref.px);
	free(reference.cells);
	free(damage.rects);
	free(ref_damage.rects);
	free(shown);
	for (int y = 0; y < t.rows; y++)
		free(t.screen[y]);
	free(t.screen);
	if (fails)
		return 1;
	puts("all render tests passed");
	return 0;
}
