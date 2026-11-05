/* VT emulator: parser, screen model, scrollback and pty. No Wayland here. */
#include "term.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <pwd.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <wchar.h>

enum {
	S_GROUND, S_ESC, S_ESC_INT, S_CSI, S_CSI_INT, S_CSI_IGNORE,
	S_OSC, S_STR, /* S_STR: DCS/SOS/PM/APC, swallowed */
};

static uint32_t palette[256];
static uint32_t def_fg, def_bg;
static int sb_lines;

/* DEC special graphics for 0x5f..0x7e */
static const uint16_t dec_graphics[32] = {
	0x0020, 0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0,
	0x00b1, 0x2424, 0x240b, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c,
	0x23ba, 0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c, 0x2524, 0x2534,
	0x252c, 0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3, 0x00b7,
};

static const struct cell empty_line[1];

void term_setup(const uint32_t pal16[16], uint32_t fg, uint32_t bg, int scrollback)
{
	static const uint8_t lvl[6] = { 0, 95, 135, 175, 215, 255 };

	memcpy(palette, pal16, 16 * sizeof *palette);
	for (int i = 0; i < 216; i++)
		palette[16 + i] = lvl[i / 36] << 16 | lvl[i / 6 % 6] << 8 | lvl[i % 6];
	for (int i = 0; i < 24; i++) {
		uint32_t v = 8 + i * 10;
		palette[232 + i] = v << 16 | v << 8 | v;
	}
	def_fg = fg;
	def_bg = bg;
	sb_lines = scrollback;
}

/* ---- grid helpers ---- */

static struct cell blank(const struct term *t)
{
	return (struct cell){ .fg = t->tmpl.fg, .bg = t->tmpl.bg };
}

static void fill(struct cell *c, int n, struct cell b)
{
	for (int i = 0; i < n; i++)
		c[i] = b;
}

/*
 * Every screen row has a hidden header cell in front of it. ROW_USED is an
 * upper bound of the non-blank cells (blank: empty, default bg, no attrs), so
 * clearing a row and trimming it for scrollback cost O(text), not O(cols).
 */
#define ROW_USED(r) ((r)[-1].cp)

static struct cell *row_new(int cols)
{
	struct cell *r = xmalloc((cols + 1) * sizeof *r);
	r++;
	fill(r, cols, (struct cell){ .fg = def_fg, .bg = def_bg });
	ROW_USED(r) = 0;
	return r;
}

static void row_free(struct cell *r)
{
	free(r - 1);
}

static void row_clear(struct term *t, struct cell *r, struct cell b)
{
	if (b.bg == def_bg) {
		fill(r, ROW_USED(r), b);
		ROW_USED(r) = 0;
	} else {
		fill(r, t->cols, b);
		ROW_USED(r) = t->cols;
	}
}

static struct cell **screen_new(int cols, int rows)
{
	struct cell **s = xcalloc(rows, sizeof *s);
	for (int y = 0; y < rows; y++)
		s[y] = row_new(cols);
	return s;
}

static void screen_free(struct cell **s, int rows)
{
	if (!s)
		return;
	for (int y = 0; y < rows; y++)
		row_free(s[y]);
	free(s);
}

static bool cell_blank(const struct cell *c)
{
	return (c->cp == 0 || c->cp == ' ') && c->bg == def_bg && !c->attr;
}

/* ---- scrollback ---- */

static struct sbline *sb_at(const struct term *t, int i)
{
	return &t->sb[(t->sb_head + i) % t->sb_cap];
}

static void sb_push(struct term *t, const struct cell *row)
{
	t->pushed++;
	if (!t->sb_cap)
		return;

	int len = MIN((int)ROW_USED(row), t->cols);
	while (len > 0 && cell_blank(&row[len - 1]))
		len--;

	struct sbline *l;
	if (t->sb_len < t->sb_cap) {
		l = sb_at(t, t->sb_len++);
	} else {
		l = sb_at(t, 0); /* evict the oldest line, reuse its buffer if it fits */
		t->sb_head = (t->sb_head + 1) % t->sb_cap;
	}
	if (len > l->cap || l->cap > 2 * len + 16) {
		free(l->cells);
		l->cap = len;
		l->cells = len ? xmalloc(len * sizeof *row) : NULL;
	}
	l->len = len;
	memcpy(l->cells, row, len * sizeof *row);
	if (t->view)
		t->view = MIN(t->view + 1, t->sb_len);
}

static void sb_clear(struct term *t)
{
	for (int i = 0; i < t->sb_cap; i++) {
		free(t->sb[i].cells);
		t->sb[i] = (struct sbline){ 0 };
	}
	t->sb_len = t->sb_head = t->view = 0;
}

const struct cell *term_abs_line(const struct term *t, int64_t abs, int *len)
{
	int64_t first = (int64_t)t->pushed;

	if (abs >= first) {
		if (abs - first >= t->rows)
			return NULL;
		*len = t->cols;
		return t->screen[abs - first];
	}
	int64_t i = abs - (first - t->sb_len);
	if (i < 0 || t->alt_active)
		return NULL;
	struct sbline *l = sb_at(t, (int)i);
	*len = l->len;
	return l->cells ? l->cells : empty_line;
}

int64_t term_view_top(const struct term *t)
{
	return (int64_t)t->pushed - t->view;
}

void term_scroll_view(struct term *t, int delta)
{
	if (t->alt_active)
		return;
	t->view = CLAMP(t->view + delta, 0, t->sb_len);
}

/* ---- selection ---- */

static void sel_bounds(const struct term *t, int64_t *l0, int *c0, int64_t *l1, int *c1)
{
	bool fwd = t->sel_a_line < t->sel_b_line ||
		(t->sel_a_line == t->sel_b_line && t->sel_a_col <= t->sel_b_col);
	*l0 = fwd ? t->sel_a_line : t->sel_b_line;
	*c0 = fwd ? t->sel_a_col : t->sel_b_col;
	*l1 = fwd ? t->sel_b_line : t->sel_a_line;
	*c1 = fwd ? t->sel_b_col : t->sel_a_col;
}

bool term_cell_selected(const struct term *t, int64_t abs, int col)
{
	if (!t->sel_active)
		return false;
	int64_t l0, l1;
	int c0, c1;
	sel_bounds(t, &l0, &c0, &l1, &c1);
	if (abs < l0 || abs > l1)
		return false;
	if (abs == l0 && col < c0)
		return false;
	if (abs == l1 && col > c1)
		return false;
	return true;
}

char *term_selection_text(const struct term *t)
{
	if (!t->sel_active)
		return NULL;
	int64_t l0, l1;
	int c0, c1;
	sel_bounds(t, &l0, &c0, &l1, &c1);

	size_t cap = 256, len = 0;
	char *out = xmalloc(cap);
	for (int64_t l = l0; l <= l1; l++) {
		int n;
		const struct cell *c = term_abs_line(t, l, &n);
		if (!c)
			continue;
		int x0 = l == l0 ? c0 : 0;
		int x1 = l == l1 ? MIN(c1 + 1, n) : n;
		size_t line_start = len;
		for (int x = x0; x < x1; x++) {
			if (c[x].attr & ATTR_WDUMMY)
				continue;
			if (cap - len < 8)
				out = xrealloc(out, cap *= 2);
			len += utf8_encode(out + len, c[x].cp ? c[x].cp : ' ');
		}
		while (len > line_start && out[len - 1] == ' ')
			len--;
		if (l != l1) {
			if (cap - len < 8)
				out = xrealloc(out, cap *= 2);
			out[len++] = '\n';
		}
	}
	out[len] = 0;
	return out;
}

/* ---- cursor and scrolling ---- */

static void move_to(struct term *t, int x, int y)
{
	int y0 = t->origin ? t->top : 0;
	int y1 = t->origin ? t->bot : t->rows - 1;

	t->cx = CLAMP(x, 0, t->cols - 1);
	t->cy = CLAMP(y, y0, y1);
	t->wrapnext = false;
}

static void scroll_up(struct term *t, int top, int bot, int n, bool to_sb)
{
	int h = bot - top + 1;
	n = MIN(n, h);
	to_sb = to_sb && top == 0 && !t->alt_active;

	struct cell b = blank(t);
	for (int i = 0; i < n; i++) {
		struct cell *row = t->screen[top];
		if (to_sb)
			sb_push(t, row);
		memmove(&t->screen[top], &t->screen[top + 1], (h - 1) * sizeof row);
		t->screen[bot] = row;
		row_clear(t, row, b);
	}
}

static void scroll_down(struct term *t, int top, int bot, int n)
{
	int h = bot - top + 1;
	n = MIN(n, h);

	struct cell b = blank(t);
	for (int i = 0; i < n; i++) {
		struct cell *row = t->screen[bot];
		memmove(&t->screen[top + 1], &t->screen[top], (h - 1) * sizeof row);
		t->screen[top] = row;
		row_clear(t, row, b);
	}
}

static void index_down(struct term *t)
{
	if (t->cy == t->bot)
		scroll_up(t, t->top, t->bot, 1, true);
	else if (t->cy < t->rows - 1)
		t->cy++;
}

static void index_up(struct term *t)
{
	if (t->cy == t->top)
		scroll_down(t, t->top, t->bot, 1);
	else if (t->cy > 0)
		t->cy--;
}

static void erase(struct term *t, int y, int x0, int x1)
{
	struct cell *r = t->screen[y];
	struct cell b = blank(t);
	int used = ROW_USED(r);

	x0 = CLAMP(x0, 0, t->cols);
	x1 = CLAMP(x1, 0, t->cols);
	if (b.bg == def_bg) {
		/* cells past ROW_USED are blank already */
		if (x0 < used)
			fill(r + x0, MIN(x1, used) - x0, b);
		if (x1 >= used)
			ROW_USED(r) = MIN(used, x0);
	} else {
		fill(r + x0, x1 - x0, b);
		ROW_USED(r) = MAX(used, x1);
	}
}

static void tab_forward(struct term *t)
{
	int x = t->cx + 1;
	while (x < t->cols - 1 && !t->tabs[x])
		x++;
	t->cx = MIN(x, t->cols - 1);
}

static void tab_back(struct term *t)
{
	int x = t->cx - 1;
	while (x > 0 && !t->tabs[x])
		x--;
	t->cx = MAX(x, 0);
}

static void save_cursor(struct term *t)
{
	struct saved_cursor *s = &t->saved[t->alt_active];
	s->cx = t->cx;
	s->cy = t->cy;
	s->tmpl = t->tmpl;
	s->charset[0] = t->charset[0];
	s->charset[1] = t->charset[1];
	s->gl = t->gl;
	s->origin = t->origin;
	s->wrapnext = t->wrapnext;
}

static void restore_cursor(struct term *t)
{
	struct saved_cursor *s = &t->saved[t->alt_active];
	t->tmpl = s->tmpl;
	t->charset[0] = s->charset[0];
	t->charset[1] = s->charset[1];
	t->gl = s->gl;
	t->origin = s->origin;
	t->cx = CLAMP(s->cx, 0, t->cols - 1);
	t->cy = CLAMP(s->cy, 0, t->rows - 1);
	t->wrapnext = s->wrapnext;
}

static void switch_screen(struct term *t, bool alt)
{
	if (alt == t->alt_active)
		return;
	t->alt_active = alt;
	t->screen = alt ? t->alt : t->pri;
	t->view = 0;
	t->sel_active = false;
	t->key_selecting = false;
}

static void clear_screen(struct term *t)
{
	for (int y = 0; y < t->rows; y++)
		erase(t, y, 0, t->cols);
}

static void reset_tabs(struct term *t)
{
	for (int x = 0; x < t->cols; x++)
		t->tabs[x] = x && x % 8 == 0;
}

static void soft_reset(struct term *t)
{
	t->tmpl = (struct cell){ .fg = def_fg, .bg = def_bg };
	t->top = 0;
	t->bot = t->rows - 1;
	t->autowrap = true;
	t->origin = t->insert = false;
	t->cursor_visible = true;
	t->app_cursor = t->app_keypad = false;
	t->charset[0] = t->charset[1] = false;
	t->gl = 0;
	t->wrapnext = false;
	save_cursor(t);
}

static void full_reset(struct term *t)
{
	switch_screen(t, false);
	soft_reset(t);
	t->bracketed_paste = t->focus_events = false;
	t->mouse_mode = MOUSE_OFF;
	t->mouse_sgr = false;
	t->cursor_shape = CURSOR_BLOCK;
	t->cx = t->cy = 0;
	reset_tabs(t);
	clear_screen(t);
	t->saved[1] = t->saved[0];
}

/* ---- printing ---- */

static void put_cell(struct term *t, int x, uint32_t cp, uint16_t extra)
{
	struct cell *row = t->screen[t->cy];

	/* never leave half of a wide glyph behind */
	if ((row[x].attr & ATTR_WDUMMY) && x > 0 && !(extra & ATTR_WDUMMY)) {
		row[x - 1].cp = 0;
		row[x - 1].attr &= ~ATTR_WIDE;
	}
	if ((row[x].attr & ATTR_WIDE) && x + 1 < t->cols) {
		row[x + 1].cp = 0;
		row[x + 1].attr &= ~ATTR_WDUMMY;
	}
	row[x] = t->tmpl;
	row[x].cp = cp;
	row[x].attr |= extra;
	if ((int)ROW_USED(row) <= x)
		ROW_USED(row) = x + 1;
}

static void print(struct term *t, uint32_t cp)
{
	if (t->charset[t->gl] && cp >= 0x5f && cp <= 0x7e)
		cp = dec_graphics[cp - 0x5f];

	int w = cp < 0x7f ? 1 : wcwidth(cp);
	if (w == 0)
		return; /* combining marks: not supported yet */
	if (w < 0 || t->cols < 2)
		w = 1;

	if (t->wrapnext) {
		t->cx = 0;
		index_down(t);
		t->wrapnext = false;
	}
	if (w == 2 && t->cx == t->cols - 1) {
		if (!t->autowrap)
			return;
		put_cell(t, t->cx, 0, 0);
		t->cx = 0;
		index_down(t);
	}
	if (t->insert) {
		struct cell *row = t->screen[t->cy];
		memmove(row + t->cx + w, row + t->cx, (t->cols - t->cx - w) * sizeof *row);
		ROW_USED(row) = MIN((int)ROW_USED(row) + w, t->cols);
	}

	put_cell(t, t->cx, cp, w == 2 ? ATTR_WIDE : 0);
	if (w == 2)
		put_cell(t, t->cx + 1, 0, ATTR_WDUMMY);
	t->lastc = cp;

	if (t->cx + w >= t->cols) {
		t->cx = t->cols - 1;
		t->wrapnext = t->autowrap;
	} else {
		t->cx += w;
	}
}

/* ---- pty output (responses, keys) ---- */

void term_write(struct term *t, const char *buf, size_t n)
{
	if (t->dead || !n)
		return;
	if (!t->wq_len) {
		ssize_t w = write(t->fd, buf, n);
		if (w > 0) {
			buf += w;
			n -= w;
		} else if (w < 0 && errno != EAGAIN && errno != EINTR) {
			return;
		}
		if (!n)
			return;
	}
	if (t->wq_off + t->wq_len + n > t->wq_cap) {
		/* Compact only when appending needs the space, never on each write. */
		if (t->wq_off) {
			memmove(t->wq, t->wq + t->wq_off, t->wq_len);
			t->wq_off = 0;
		}
		if (t->wq_len + n > t->wq_cap) {
			t->wq_cap = MAX(t->wq_cap * 2, t->wq_len + n);
			t->wq = xrealloc(t->wq, t->wq_cap);
		}
	}
	memcpy(t->wq + t->wq_off + t->wq_len, buf, n);
	t->wq_len += n;
}

void term_flush(struct term *t)
{
	size_t budget = 256 * 1024;
	while (t->wq_len && budget) {
		ssize_t w = write(t->fd, t->wq + t->wq_off, MIN(t->wq_len, budget));
		if (w <= 0)
			return;
		t->wq_off += w;
		t->wq_len -= w;
		budget -= w;
	}
	if (!t->wq_len) {
		t->wq_off = 0;
		/* A large paste must not permanently inflate every tab's allocation. */
		if (t->wq_cap >= 1024 * 1024) {
			free(t->wq);
			t->wq = NULL;
			t->wq_cap = 0;
		}
	}
}

static void reply(struct term *t, const char *s)
{
	term_write(t, s, strlen(s));
}

void term_paste(struct term *t, const char *buf, size_t n)
{
	char *s = xmalloc(n + 1);
	size_t len = 0;

	/* LF -> CR like other terminals; drop ESC so a paste can't end bracketed mode */
	for (size_t i = 0; i < n; i++) {
		if (buf[i] == '\r' && i + 1 < n && buf[i + 1] == '\n')
			continue;
		if (buf[i] == '\n')
			s[len++] = '\r';
		else if (buf[i] != 0x1b || !t->bracketed_paste)
			s[len++] = buf[i];
	}
	if (t->bracketed_paste)
		reply(t, "\033[200~");
	term_write(t, s, len);
	if (t->bracketed_paste)
		reply(t, "\033[201~");
	free(s);
}

bool term_mouse(struct term *t, int button, int col, int row, unsigned mods,
                enum mouse_event event)
{
	if (t->mouse_mode == MOUSE_OFF || t->view ||
	    col < 0 || col >= t->cols || row < 0 || row >= t->rows)
		return false;
	if (event == MOUSE_MOVE &&
	    (t->mouse_mode == MOUSE_BUTTON || (t->mouse_mode == MOUSE_DRAG && button == 3)))
		return false;
	if (event == MOUSE_RELEASE && button >= 64)
		return false; /* wheels have no release reports */

	int code = button | mods;
	if (event == MOUSE_MOVE)
		code |= 32;
	char buf[64];
	int n;
	if (t->mouse_sgr) {
		n = snprintf(buf, sizeof buf, "\033[<%d;%d;%d%c", code, col + 1, row + 1,
		             event == MOUSE_RELEASE ? 'm' : 'M');
	} else {
		if (col >= 223 || row >= 223)
			return false; /* legacy encoding cannot represent larger coordinates */
		if (event == MOUSE_RELEASE)
			code = 3 | mods;
		memcpy(buf, "\033[M", 3);
		buf[3] = code + 32;
		buf[4] = col + 33;
		buf[5] = row + 33;
		n = 6;
	}
	term_write(t, buf, n);
	return true;
}

/* ---- escape sequences ---- */

static int P(const struct term *t, int i, int def)
{
	return i < t->np && t->params[i] ? (int)t->params[i] : def;
}

static void control(struct term *t, uint8_t b)
{
	switch (b) {
	case '\b':
		if (t->cx > 0)
			t->cx--;
		t->wrapnext = false;
		break;
	case '\t':
		tab_forward(t);
		break;
	case '\n': case '\v': case '\f':
		index_down(t);
		t->wrapnext = false;
		break;
	case '\r':
		t->cx = 0;
		t->wrapnext = false;
		break;
	case 0x0e:
		t->gl = 1;
		break;
	case 0x0f:
		t->gl = 0;
		break;
	}
}

static void esc_dispatch(struct term *t, uint8_t b)
{
	if (t->ninter) {
		if (t->inter[0] == '(' || t->inter[0] == ')')
			t->charset[t->inter[0] == ')'] = b == '0';
		return;
	}
	switch (b) {
	case '7': save_cursor(t); break;
	case '8': restore_cursor(t); break;
	case 'D': index_down(t); break;
	case 'E': t->cx = 0; index_down(t); break;
	case 'H': t->tabs[t->cx] = true; break;
	case 'M': index_up(t); break;
	case 'c': full_reset(t); break;
	case '=': t->app_keypad = true; break;
	case '>': t->app_keypad = false; break;
	}
	t->wrapnext = false;
}

static int ext_color(struct term *t, int i, uint32_t *out)
{
	if (i + 1 >= t->np)
		return i;

	int subs = 0;
	for (int j = i + 1; j < t->np && (t->colon >> j & 1); j++)
		subs++;

	switch (t->params[i + 1]) {
	case 5:
		if (i + 2 < t->np)
			*out = palette[t->params[i + 2] & 255];
		return i + 2;
	case 2: {
		int k = i + 2 + (subs >= 5); /* 38:2:<colorspace>:r:g:b */
		if (k + 2 < t->np)
			*out = (t->params[k] & 255) << 16 | (t->params[k + 1] & 255) << 8 |
			       (t->params[k + 2] & 255);
		return k + 2;
	}
	}
	return i + 1;
}

static void sgr(struct term *t)
{
	struct cell *p = &t->tmpl;

	if (!t->np) {
		*p = (struct cell){ .fg = def_fg, .bg = def_bg };
		return;
	}
	for (int i = 0; i < t->np; i++) {
		uint32_t v = t->params[i], dummy;
		switch (v) {
		case 0: *p = (struct cell){ .fg = def_fg, .bg = def_bg }; break;
		case 1: p->attr |= ATTR_BOLD; break;
		case 2: p->attr |= ATTR_DIM; break;
		case 3: p->attr |= ATTR_ITALIC; break;
		case 4:
			if (i + 1 < t->np && (t->colon >> (i + 1) & 1) && t->params[++i] == 0)
				p->attr &= ~ATTR_UNDERLINE;
			else
				p->attr |= ATTR_UNDERLINE;
			break;
		case 7: p->attr |= ATTR_REVERSE; break;
		case 8: p->attr |= ATTR_INVISIBLE; break;
		case 9: p->attr |= ATTR_STRIKE; break;
		case 21: p->attr |= ATTR_UNDERLINE; break;
		case 22: p->attr &= ~(ATTR_BOLD | ATTR_DIM); break;
		case 23: p->attr &= ~ATTR_ITALIC; break;
		case 24: p->attr &= ~ATTR_UNDERLINE; break;
		case 27: p->attr &= ~ATTR_REVERSE; break;
		case 28: p->attr &= ~ATTR_INVISIBLE; break;
		case 29: p->attr &= ~ATTR_STRIKE; break;
		case 38: i = ext_color(t, i, &p->fg); break;
		case 39: p->fg = def_fg; break;
		case 48: i = ext_color(t, i, &p->bg); break;
		case 49: p->bg = def_bg; break;
		case 58: i = ext_color(t, i, &dummy); break; /* underline color: ignored */
		default:
			if (v >= 30 && v <= 37)
				p->fg = palette[v - 30];
			else if (v >= 40 && v <= 47)
				p->bg = palette[v - 40];
			else if (v >= 90 && v <= 97)
				p->fg = palette[v - 90 + 8];
			else if (v >= 100 && v <= 107)
				p->bg = palette[v - 100 + 8];
		}
	}
}

static void set_private_mode(struct term *t, int mode, bool on)
{
	switch (mode) {
	case 1: t->app_cursor = on; break;
	case 6: t->origin = on; move_to(t, 0, on ? t->top : 0); break;
	case 7: t->autowrap = on; if (!on) t->wrapnext = false; break;
	case 25: t->cursor_visible = on; break;
	case 47: switch_screen(t, on); break;
	case 1047:
		if (!on && t->alt_active)
			clear_screen(t);
		switch_screen(t, on);
		break;
	case 1048: on ? save_cursor(t) : restore_cursor(t); break;
	case 1049:
		if (on) {
			save_cursor(t);
			switch_screen(t, true);
			clear_screen(t);
		} else {
			switch_screen(t, false);
			restore_cursor(t);
		}
		break;
	case 1004: t->focus_events = on; break;
	case 1000: case 1002: case 1003:
		t->mouse_mode = !on ? MOUSE_OFF : mode == 1000 ? MOUSE_BUTTON :
		                mode == 1002 ? MOUSE_DRAG : MOUSE_MOTION;
		break;
	case 1006: t->mouse_sgr = on; break;
	case 2004: t->bracketed_paste = on; break;
	}
}

static void csi_dispatch(struct term *t, uint8_t b)
{
	char buf[64];
	struct cell *row = t->screen[t->cy];
	int n = P(t, 0, 1);

	if (t->priv == '?') {
		if (b == 'h' || b == 'l')
			for (int i = 0; i < t->np; i++)
				set_private_mode(t, t->params[i], b == 'h');
		return;
	}
	if (t->priv == '>') {
		if (b == 'c' && !P(t, 0, 0))
			reply(t, "\033[>1;100;0c");
		return;
	}
	if (t->priv)
		return;
	if (t->ninter) {
		if (t->inter[0] == ' ' && b == 'q') {
			int s = P(t, 0, 1);
			t->cursor_shape = s <= 2 ? CURSOR_BLOCK : s <= 4 ? CURSOR_UNDERLINE : CURSOR_BAR;
		} else if (t->inter[0] == '!' && b == 'p') {
			soft_reset(t);
		}
		return;
	}

	switch (b) {
	case '@':
		n = MIN(n, t->cols - t->cx);
		memmove(row + t->cx + n, row + t->cx, (t->cols - t->cx - n) * sizeof *row);
		ROW_USED(row) = MIN((int)ROW_USED(row) + n, t->cols);
		erase(t, t->cy, t->cx, t->cx + n);
		break;
	case 'A': t->cy = MAX(t->cy - n, t->cy >= t->top ? t->top : 0); t->wrapnext = false; break;
	case 'B': case 'e':
		t->cy = MIN(t->cy + n, t->cy <= t->bot ? t->bot : t->rows - 1);
		t->wrapnext = false;
		break;
	case 'C': case 'a': move_to(t, t->cx + n, t->cy); break;
	case 'D': move_to(t, t->cx - n, t->cy); break;
	case 'E': move_to(t, 0, t->cy + n); break;
	case 'F': move_to(t, 0, t->cy - n); break;
	case 'G': case '`': move_to(t, n - 1, t->cy); break;
	case 'H': case 'f':
		move_to(t, P(t, 1, 1) - 1, (t->origin ? t->top : 0) + n - 1);
		break;
	case 'd': move_to(t, t->cx, (t->origin ? t->top : 0) + n - 1); break;
	case 'I': while (n--) tab_forward(t); break;
	case 'Z': while (n--) tab_back(t); break;
	case 'J':
		switch (P(t, 0, 0)) {
		case 0:
			erase(t, t->cy, t->cx, t->cols);
			for (int y = t->cy + 1; y < t->rows; y++)
				erase(t, y, 0, t->cols);
			break;
		case 1:
			erase(t, t->cy, 0, t->cx + 1);
			for (int y = 0; y < t->cy; y++)
				erase(t, y, 0, t->cols);
			break;
		case 2: clear_screen(t); break;
		case 3: sb_clear(t); break;
		}
		break;
	case 'K':
		switch (P(t, 0, 0)) {
		case 0: erase(t, t->cy, t->cx, t->cols); break;
		case 1: erase(t, t->cy, 0, t->cx + 1); break;
		case 2: erase(t, t->cy, 0, t->cols); break;
		}
		break;
	case 'L':
		if (t->cy >= t->top && t->cy <= t->bot) {
			scroll_down(t, t->cy, t->bot, n);
			t->cx = 0;
			t->wrapnext = false;
		}
		break;
	case 'M':
		if (t->cy >= t->top && t->cy <= t->bot) {
			scroll_up(t, t->cy, t->bot, n, false);
			t->cx = 0;
			t->wrapnext = false;
		}
		break;
	case 'P':
		n = MIN(n, t->cols - t->cx);
		memmove(row + t->cx, row + t->cx + n, (t->cols - t->cx - n) * sizeof *row);
		erase(t, t->cy, t->cols - n, t->cols);
		break;
	case 'S': scroll_up(t, t->top, t->bot, n, true); break;
	case 'T': if (t->np <= 1) scroll_down(t, t->top, t->bot, n); break;
	case 'X': erase(t, t->cy, t->cx, t->cx + n); break;
	case 'b':
		if (t->lastc)
			for (n = MIN(n, t->cols * t->rows); n > 0; n--)
				print(t, t->lastc);
		break;
	case 'c':
		if (!P(t, 0, 0))
			reply(t, "\033[?62;22c");
		break;
	case 'g':
		if (P(t, 0, 0) == 0)
			t->tabs[t->cx] = false;
		else if (P(t, 0, 0) == 3)
			memset(t->tabs, 0, t->cols);
		break;
	case 'h': case 'l':
		for (int i = 0; i < t->np; i++)
			if (t->params[i] == 4)
				t->insert = b == 'h';
		break;
	case 'm': sgr(t); break;
	case 'n':
		if (P(t, 0, 0) == 5) {
			reply(t, "\033[0n");
		} else if (P(t, 0, 0) == 6) {
			snprintf(buf, sizeof buf, "\033[%d;%dR",
			         t->cy - (t->origin ? t->top : 0) + 1, t->cx + 1);
			reply(t, buf);
		}
		break;
	case 'r': {
		int top = P(t, 0, 1) - 1, bot = P(t, 1, t->rows) - 1;
		bot = MIN(bot, t->rows - 1);
		if (top < bot) {
			t->top = top;
			t->bot = bot;
			move_to(t, 0, t->origin ? t->top : 0);
		}
		break;
	}
	case 's': save_cursor(t); break;
	case 'u': restore_cursor(t); break;
	case 't':
		if (P(t, 0, 0) == 18) {
			snprintf(buf, sizeof buf, "\033[8;%d;%dt", t->rows, t->cols);
			reply(t, buf);
		}
		break;
	}
}

static void osc_dispatch(struct term *t)
{
	char buf[64];

	t->osc[t->osc_len] = 0;
	char *arg = strchr(t->osc, ';');
	if (!arg)
		return;
	*arg++ = 0;
	int cmd = atoi(t->osc);

	if (cmd == 52 && t->clipboard_set) {
		char *encoded = strchr(arg, ';');
		if (!encoded || t->osc_len >= TERM_OSC_MAX - 1) return;
		encoded++;
		if (!strcmp(encoded, "?")) return;
		const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		char *out = xmalloc(strlen(encoded) * 3 / 4 + 2);
		size_t n = 0; unsigned acc = 0, bits = 0;
		for (const char *p = encoded; *p && *p != '='; p++) {
			const char *q = strchr(alphabet, *p);
			if (!q) { free(out); return; }
			acc = (acc << 6) | (unsigned)(q - alphabet);
			bits += 6;
			if (bits >= 8) { bits -= 8; out[n++] = (char)(acc >> bits); }
		}
		out[n] = 0;
		if (!memchr(out, 0, n)) t->clipboard_set(out);
		free(out);
	} else if (cmd == 0 || cmd == 2 || cmd == 777) {
		if (cmd != 777 && t->title_locked)
			return;
		if (cmd == 777)
			t->title_locked = *arg != '\0';
		size_t n = MIN(strlen(arg), sizeof t->title - 1);
		while (n && n < strlen(arg) && ((uint8_t)arg[n] & 0xc0) == 0x80)
			n--; /* don't cut a UTF-8 sequence */
		if (strlen(t->title) == n && !memcmp(t->title, arg, n))
			return;
		memcpy(t->title, arg, n);
		t->title[n] = 0;
		t->title_changed = true;
	} else if ((cmd == 10 || cmd == 11) && !strcmp(arg, "?")) {
		uint32_t c = cmd == 10 ? def_fg : def_bg;
		snprintf(buf, sizeof buf, "\033]%d;rgb:%02x%02x/%02x%02x/%02x%02x\033\\", cmd,
		         c >> 16 & 255, c >> 16 & 255, c >> 8 & 255, c >> 8 & 255, c & 255, c & 255);
		reply(t, buf);
	}
}

static void utf8_byte(struct term *t, uint8_t b)
{
	if (t->utf8_need) {
		if ((b & 0xc0) == 0x80) {
			t->utf8_cp = t->utf8_cp << 6 | (b & 0x3f);
			if (--t->utf8_need == 0)
				print(t, t->utf8_cp <= 0x10ffff ? t->utf8_cp : 0xfffd);
			return;
		}
		t->utf8_need = 0;
		print(t, 0xfffd);
	}
	if ((b & 0xe0) == 0xc0) {
		t->utf8_cp = b & 0x1f;
		t->utf8_need = 1;
	} else if ((b & 0xf0) == 0xe0) {
		t->utf8_cp = b & 0x0f;
		t->utf8_need = 2;
	} else if ((b & 0xf8) == 0xf0) {
		t->utf8_cp = b & 0x07;
		t->utf8_need = 3;
	} else {
		print(t, 0xfffd);
	}
}

static void csi_start(struct term *t)
{
	memset(t->params, 0, sizeof t->params);
	t->np = 0;
	t->colon = 0;
	t->priv = 0;
	t->ninter = 0;
	t->state = S_CSI;
}

static void collect(struct term *t, uint8_t b)
{
	if (t->ninter < (int)sizeof t->inter)
		t->inter[t->ninter++] = b;
}

static void feed(struct term *t, uint8_t b)
{
	if (t->state == S_OSC || t->state == S_STR) {
		if (t->str_esc) {
			t->str_esc = false;
			if (b == '\\') {
				if (t->state == S_OSC)
					osc_dispatch(t);
				t->state = S_GROUND;
				return;
			}
			t->state = S_ESC;
			t->ninter = 0;
			feed(t, b);
			return;
		}
		if (b == 0x07) {
			if (t->state == S_OSC)
				osc_dispatch(t);
			t->state = S_GROUND;
		} else if (b == 0x1b) {
			t->str_esc = true;
		} else if (b == 0x18 || b == 0x1a) {
			t->state = S_GROUND;
		} else if (t->state == S_OSC && b >= 0x20 && t->osc_len < TERM_OSC_MAX - 1) {
			t->osc[t->osc_len++] = b;
		}
		return;
	}

	if (b < 0x20) {
		if (t->utf8_need) {
			t->utf8_need = 0;
			print(t, 0xfffd);
		}
		if (b == 0x1b) {
			t->state = S_ESC;
			t->ninter = 0;
		} else if (b == 0x18 || b == 0x1a) {
			t->state = S_GROUND;
		} else {
			control(t, b);
		}
		return;
	}
	if (b == 0x7f)
		return;

	switch (t->state) {
	case S_GROUND:
		if (b < 0x80 && !t->utf8_need)
			print(t, b);
		else
			utf8_byte(t, b);
		return;
	case S_ESC:
		if (b <= 0x2f) {
			collect(t, b);
			t->state = S_ESC_INT;
		} else if (b == '[') {
			csi_start(t);
		} else if (b == ']') {
			t->osc_len = 0;
			t->state = S_OSC;
		} else if (b == 'P' || b == 'X' || b == '^' || b == '_') {
			t->state = S_STR;
		} else {
			esc_dispatch(t, b);
			t->state = S_GROUND;
		}
		return;
	case S_ESC_INT:
		if (b <= 0x2f) {
			collect(t, b);
		} else {
			esc_dispatch(t, b);
			t->state = S_GROUND;
		}
		return;
	case S_CSI:
		if (b >= '0' && b <= '9') {
			if (!t->np)
				t->np = 1;
			uint32_t *p = &t->params[t->np - 1];
			*p = MIN(*p * 10 + (b - '0'), 65535u);
		} else if (b == ';' || b == ':') {
			if (!t->np)
				t->np = 1;
			if (t->np < TERM_MAX_PARAMS) {
				if (b == ':')
					t->colon |= 1u << t->np;
				t->np++;
			}
		} else if (b >= 0x3c && b <= 0x3f) {
			if (!t->np && !t->priv)
				t->priv = b;
			else
				t->state = S_CSI_IGNORE;
		} else if (b <= 0x2f) {
			collect(t, b);
			t->state = S_CSI_INT;
		} else if (b >= 0x40 && b <= 0x7e) {
			t->state = S_GROUND;
			csi_dispatch(t, b);
		} else {
			t->state = S_CSI_IGNORE;
		}
		return;
	case S_CSI_INT:
		if (b <= 0x2f) {
			collect(t, b);
		} else if (b >= 0x40 && b <= 0x7e) {
			t->state = S_GROUND;
			csi_dispatch(t, b);
		} else {
			t->state = S_CSI_IGNORE;
		}
		return;
	case S_CSI_IGNORE:
		if (b >= 0x40 && b <= 0x7e)
			t->state = S_GROUND;
		return;
	}
}

void term_input(struct term *t, const uint8_t *buf, size_t n)
{
	for (size_t i = 0; i < n; i++)
		feed(t, buf[i]);
}

bool term_read(struct term *t)
{
	static uint8_t buf[65536];

	/* cap per call so one busy tab can't starve input and rendering */
	for (int i = 0; i < 4; i++) {
		ssize_t n = read(t->fd, buf, sizeof buf);
		if (n > 0) {
			term_input(t, buf, n);
			if ((size_t)n < sizeof buf)
				break;
		} else if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
			break;
		} else {
			t->dead = true;
			return false;
		}
	}
	return true;
}

/* ---- lifecycle ---- */

static void spawn(struct term *t, struct winsize *ws)
{
	pid_t pid = forkpty(&t->fd, NULL, NULL, ws);
	if (pid < 0) {
		perror("bixis-term: forkpty");
		t->fd = -1;
		t->dead = true;
		return;
	}
	if (pid == 0) {
		signal(SIGCHLD, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);
		setenv("TERM", "xterm-256color", 1);
		setenv("COLORTERM", "truecolor", 1);
		unsetenv("COLUMNS");
		unsetenv("LINES");

		const char *sh = getenv("SHELL");
		if (!sh || !*sh) {
			struct passwd *pw = getpwuid(getuid());
			sh = pw && pw->pw_shell && *pw->pw_shell ? pw->pw_shell : "/bin/sh";
		}
		/* Bash accepts slash-named functions, so this needs no root-level file. */
		const char *name = strrchr(sh, '/');
		name = name ? name + 1 : sh;
		if (!strcmp(name, "bash")) {
			int rc = memfd_create("bixis-bashrc", 0);
			if (rc >= 0) {
				char script[1024], path[64];
				int n = snprintf(script, sizeof script,
				    "[ ! -r \"$HOME/.bashrc\" ] || . \"$HOME/.bashrc\"\n"
				    "function /set_title { printf '\\033]777;%%s\\007' \"$*\"; }\n"
				    "exec %d<&-\n", rc);
				if (write(rc, script, n) == n && lseek(rc, 0, SEEK_SET) == 0) {
					snprintf(path, sizeof path, "/proc/self/fd/%d", rc);
					execl(sh, sh, "--rcfile", path, "-i", (char *)NULL);
				}
				close(rc);
			}
		}
		execl(sh, sh, (char *)NULL);
		_exit(127);
	}
	t->pid = pid;
	fcntl(t->fd, F_SETFL, O_NONBLOCK);
	fcntl(t->fd, F_SETFD, FD_CLOEXEC);
}

struct term *term_new(int cols, int rows, int pw, int ph)
{
	struct term *t = xcalloc(1, sizeof *t);

	t->cols = MAX(cols, 1);
	t->rows = MAX(rows, 1);
	t->pri = screen_new(t->cols, t->rows);
	t->alt = screen_new(t->cols, t->rows);
	t->screen = t->pri;
	t->tabs = xcalloc(t->cols, 1);
	t->sb_cap = sb_lines;
	if (t->sb_cap)
		t->sb = xcalloc(t->sb_cap, sizeof *t->sb);
	strcpy(t->title, "bixis-term");

	reset_tabs(t);
	soft_reset(t);
	t->saved[1] = t->saved[0];

	struct winsize ws = { t->rows, t->cols, t->cols * pw, t->rows * ph };
	spawn(t, &ws);
	return t;
}

void term_free(struct term *t)
{
	if (t->fd >= 0) {
		close(t->fd);
		kill(t->pid, SIGHUP);
	}
	screen_free(t->pri, t->rows);
	screen_free(t->alt, t->rows);
	sb_clear(t);
	free(t->sb);
	free(t->tabs);
	free(t->wq);
	free(t);
}

static struct cell **screen_resize(struct term *t, struct cell **s, int cols, int rows,
                                   int shift, bool to_sb)
{
	struct cell **ns = xcalloc(rows, sizeof *ns);
	struct cell b = { .fg = def_fg, .bg = def_bg };

	for (int y = 0; y < shift; y++) {
		if (to_sb)
			sb_push(t, s[y]);
		row_free(s[y]);
	}
	for (int y = 0; y < rows; y++) {
		int oy = y + shift;
		if (oy >= t->rows) {
			ns[y] = row_new(cols);
			continue;
		}
		struct cell *r = (struct cell *)xrealloc(s[oy] - 1, (cols + 1) * sizeof *r) + 1;
		if (cols > t->cols) {
			fill(r + t->cols, cols - t->cols, b);
		} else {
			ROW_USED(r) = MIN((int)ROW_USED(r), cols);
			if (r[cols - 1].attr & ATTR_WIDE)
				r[cols - 1] = b;
		}
		ns[y] = r;
	}
	for (int y = rows + shift; y < t->rows; y++)
		row_free(s[y]);
	free(s);
	return ns;
}

void term_resize(struct term *t, int cols, int rows, int pw, int ph)
{
	cols = MAX(cols, 1);
	rows = MAX(rows, 1);

	if (cols != t->cols || rows != t->rows) {
		/* keep the cursor line visible when shrinking: push lines into scrollback */
		int shift_cur = MAX(0, t->cy - rows + 1);
		int shift_pri = t->alt_active ? MAX(0, t->saved[0].cy - rows + 1) : shift_cur;
		int shift_alt = t->alt_active ? shift_cur : 0;

		t->pri = screen_resize(t, t->pri, cols, rows, shift_pri, true);
		t->alt = screen_resize(t, t->alt, cols, rows, shift_alt, false);
		t->screen = t->alt_active ? t->alt : t->pri;
		t->cy -= shift_cur;
		t->saved[0].cy -= shift_pri;
		t->saved[1].cy -= shift_alt;

		bool *tabs = xcalloc(cols, 1);
		for (int x = 0; x < cols; x++)
			tabs[x] = x < t->cols ? t->tabs[x] : x % 8 == 0;
		free(t->tabs);
		t->tabs = tabs;

		t->cols = cols;
		t->rows = rows;
		t->top = 0;
		t->bot = rows - 1;
		t->cx = CLAMP(t->cx, 0, cols - 1);
		t->cy = CLAMP(t->cy, 0, rows - 1);
		t->wrapnext = false;
		t->view = MIN(t->view, t->sb_len);
		t->sel_active = false;
		t->key_selecting = false;
	}

	if (t->fd >= 0) {
		struct winsize ws = { rows, cols, cols * pw, rows * ph };
		ioctl(t->fd, TIOCSWINSZ, &ws);
	}
}
