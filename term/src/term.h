#ifndef BIXIS_TERM_H
#define BIXIS_TERM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

enum {
	ATTR_BOLD      = 1 << 0,
	ATTR_DIM       = 1 << 1,
	ATTR_ITALIC    = 1 << 2,
	ATTR_UNDERLINE = 1 << 3,
	ATTR_REVERSE   = 1 << 4,
	ATTR_INVISIBLE = 1 << 5,
	ATTR_STRIKE    = 1 << 6,
	ATTR_WIDE      = 1 << 7,  /* first half of a double-width glyph */
	ATTR_WDUMMY    = 1 << 8,  /* second half, never drawn */
	/* render-only flags, never stored in the grid */
	ATTR_SELECTED  = 1 << 12,
	ATTR_CURSOR    = 1 << 13,
};

enum cursor_shape { CURSOR_BLOCK, CURSOR_UNDERLINE, CURSOR_BAR };

enum mouse_mode { MOUSE_OFF, MOUSE_BUTTON, MOUSE_DRAG, MOUSE_MOTION };
enum mouse_event { MOUSE_PRESS, MOUSE_RELEASE, MOUSE_MOVE };
enum { MOUSE_SHIFT = 4, MOUSE_ALT = 8, MOUSE_CTRL = 16 };

struct cell {
	uint32_t cp;      /* 0 = empty */
	uint32_t fg, bg;  /* 0xRRGGBB */
	uint16_t attr;
	uint16_t _pad;
};

struct sbline {
	int len, cap;
	struct cell *cells;
};

struct saved_cursor {
	int cx, cy;
	struct cell tmpl;
	bool charset[2];
	int gl;
	bool origin, wrapnext;
};

#define TERM_MAX_PARAMS 16
#define TERM_OSC_MAX    512

struct term {
	int cols, rows;
	struct cell **screen;          /* active screen: pri or alt */
	struct cell **pri, **alt;
	bool alt_active;

	int cx, cy;
	bool wrapnext;
	struct cell tmpl;              /* current pen: fg, bg, attr */
	int top, bot;                  /* scroll region, inclusive */
	bool *tabs;
	bool charset[2];               /* true = DEC special graphics */
	int gl;
	struct saved_cursor saved[2];  /* per screen */
	uint32_t lastc;

	/* modes */
	bool autowrap, origin, insert;
	bool cursor_visible, app_cursor, app_keypad;
	bool bracketed_paste, focus_events;
	enum mouse_mode mouse_mode;
	bool mouse_sgr;
	enum cursor_shape cursor_shape;

	/* parser */
	int state;
	uint32_t params[TERM_MAX_PARAMS];
	uint32_t colon;                /* bit i: params[i] was preceded by ':' */
	int np;
	char priv;
	char inter[2];
	int ninter;
	bool str_esc;
	char osc[TERM_OSC_MAX];
	int osc_len;
	uint32_t utf8_cp;
	int utf8_need;

	/* scrollback ring (primary screen only) */
	struct sbline *sb;
	int sb_cap, sb_len, sb_head;   /* head = index of oldest line */
	uint64_t pushed;               /* total lines ever pushed */
	int view;                      /* lines scrolled back, 0 = live */

	/* selection, absolute line numbers (see term_abs_line) */
	bool sel_active;
	int64_t sel_a_line, sel_b_line;
	int sel_a_col, sel_b_col;

	/* pty */
	int fd;
	pid_t pid;
	char *wq;
	size_t wq_off, wq_len, wq_cap;
	bool dead;

	char title[128];
	bool title_changed;
};

/* Palette shared by all terminals: 0-15 from config, 16-255 computed. */
void term_setup(const uint32_t pal16[16], uint32_t fg, uint32_t bg, int scrollback);

struct term *term_new(int cols, int rows, int pw, int ph);
void term_free(struct term *t);
void term_resize(struct term *t, int cols, int rows, int pw, int ph);

void term_input(struct term *t, const uint8_t *buf, size_t n);
/* Reads from the pty and feeds the parser. Returns false when the child is gone. */
bool term_read(struct term *t);
void term_write(struct term *t, const char *buf, size_t n);
void term_flush(struct term *t);
void term_paste(struct term *t, const char *buf, size_t n);
/* Zero-based cell coordinates; buttons 0..2, 3 = none, 64/65 = wheel up/down. */
bool term_mouse(struct term *t, int button, int col, int row, unsigned mods,
                enum mouse_event event);

void term_scroll_view(struct term *t, int delta);

/* Line at absolute index; returns NULL if out of range. *len is its width. */
const struct cell *term_abs_line(const struct term *t, int64_t abs, int *len);
int64_t term_view_top(const struct term *t);
bool term_cell_selected(const struct term *t, int64_t abs, int col);
char *term_selection_text(const struct term *t);

#endif
