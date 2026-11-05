#ifndef BIXIS_RENDER_H
#define BIXIS_RENDER_H

#include <stdbool.h>
#include <stdint.h>

#include "font.h"
#include "term.h"

struct theme {
	uint32_t fg, bg;
	uint32_t cursor, cursor_text;
	uint32_t sel_bg, sel_fg;
	uint32_t tab_bg;
};

struct canvas {
	uint32_t *px;
	int w, h;           /* pixels, stride == w */
};

/* What a buffer currently shows; rows that match are skipped. */
struct snapshot {
	struct cell *cells;
	int cols, rows;
	int w, h, cw, ch, pad;
	bool valid;
	bool bar;   /* tab bar state this snapshot was built for */
};

struct damage_rect {
	int x, y, w, h;
};

struct damage {
	struct damage_rect *rects;
	int len, cap;
};

struct frame {
	struct term *t;
	const struct cell *tabbar;  /* t->cols cells, NULL when hidden */
	bool focused;
	int pad;                    /* pixels */
};

/* Repaint relative to this buffer, but damage relative to the last commit.
 * presented may be snap itself; NULL means no frame has been submitted yet. */
void render(struct canvas *cv, struct snapshot *snap, struct font *font,
            const struct theme *th, const struct frame *fr,
            const struct snapshot *presented, struct damage *damage);

#endif
