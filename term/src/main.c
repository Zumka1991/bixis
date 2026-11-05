/* bixis-term: Wayland window, input, clipboard, tabs and the event loop. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <locale.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "cursor-shape-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include "font.h"
#include "render.h"
#include "term.h"
#include "util.h"

enum { CTRL = 1, SHIFT = 2, ALT = 4, SUPER = 8 };

enum action {
	ACT_TAB_NEW, ACT_TAB_CLOSE, ACT_TAB_NEXT, ACT_TAB_PREV, ACT_TAB_GOTO,
	ACT_COPY, ACT_PASTE,
	ACT_SCROLL_PAGE, ACT_SCROLL_TOP, ACT_SCROLL_BOTTOM,
	ACT_FONT_SIZE,
};

struct binding {
	unsigned mods;
	xkb_keysym_t sym;
	enum action act;
	int arg;
};

#include "config.h"

struct buffer {
	struct wl_buffer *wl;
	uint32_t *px;
	int w, h;
	size_t size;
	bool busy;
	struct snapshot snap;
};

struct offer {
	const char *mime;
	int rank;
};

static struct {
	struct wl_display *dpy;
	struct wl_compositor *comp;
	struct wl_shm *shm;
	struct xdg_wm_base *wm;
	struct wl_seat *seat;
	struct wl_keyboard *kbd;
	struct wl_pointer *ptr;
	struct wl_data_device_manager *ddm;
	struct wl_data_device *ddev;
	struct zxdg_decoration_manager_v1 *deco_mgr;
	struct wp_cursor_shape_manager_v1 *cursor_mgr;
	struct wp_cursor_shape_device_v1 *cursor_dev;

	struct wl_surface *surf;
	struct xdg_surface *xsurf;
	struct xdg_toplevel *top;
	int width, height;          /* logical */
	int pend_w, pend_h;
	int scale;
	bool configured, frame_pending, dirty, focused, running;
	struct buffer bufs[2];

	struct font *font;
	double font_size;
	int cols, rows;
	bool bar;

	struct term *tabs[MAX_TABS];
	int ntabs, cur;
	int tab_x0[MAX_TABS], tab_x1[MAX_TABS];
	struct cell *tabbar;
	int tabbar_cap;

	struct xkb_context *xkb;
	struct xkb_keymap *keymap;
	struct xkb_state *xkb_state;
	int repeat_fd, repeat_rate, repeat_delay;
	uint32_t repeat_key;
	uint32_t serial;

	double px, py;
	bool selecting, wheel_discrete;
	double wheel_acc;

	struct wl_data_offer *sel_offer, *dnd_offer;
	struct wl_data_source *source;
	char *copy_text;
	int paste_fd;
	struct term *paste_tab;
	char *paste_buf;
	size_t paste_len, paste_cap;
} app;

static const struct theme theme = {
	.fg = COLOR_FG, .bg = COLOR_BG,
	.cursor = COLOR_CURSOR, .cursor_text = COLOR_CURSOR_TEXT,
	.sel_bg = COLOR_SEL_BG, .sel_fg = COLOR_SEL_FG,
	.tab_bg = COLOR_TAB_BG,
};

static const char *const text_mimes[] = {
	"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING",
};

static struct term *cur(void)
{
	return app.tabs[app.cur];
}

/* ---- layout, font, tabs ---- */

static void grid_size(int ntabs)
{
	int W = app.width * app.scale, H = app.height * app.scale;
	int pad = PADDING * app.scale;

	app.bar = ntabs > 1;
	app.cols = MAX(1, (W - 2 * pad) / app.font->cw);
	app.rows = MAX(1, (H - 2 * pad - (app.bar ? app.font->ch : 0)) / app.font->ch);
}

static void layout(void)
{
	grid_size(app.ntabs);
	for (int i = 0; i < app.ntabs; i++)
		term_resize(app.tabs[i], app.cols, app.rows, app.font->cw, app.font->ch);
	app.dirty = true;
}

static void invalidate(void)
{
	for (int i = 0; i < 2; i++)
		app.bufs[i].snap.valid = false;
	app.dirty = true;
}

static void set_font(double size)
{
	size = CLAMP(size, 4.0, 72.0);
	struct font *f = font_open(FONT, size, app.scale);
	if (app.font)
		font_close(app.font);
	app.font = f;
	app.font_size = size;
	invalidate();
	if (app.configured)
		layout();
}

static void update_title(void)
{
	xdg_toplevel_set_title(app.top, cur()->title);
	cur()->title_changed = false;
}

static void tab_new(void)
{
	if (app.ntabs >= MAX_TABS)
		return;
	grid_size(app.ntabs + 1);
	struct term *t = term_new(app.cols, app.rows, app.font->cw, app.font->ch);

	int at = app.ntabs ? app.cur + 1 : 0;
	memmove(&app.tabs[at + 1], &app.tabs[at], (app.ntabs - at) * sizeof *app.tabs);
	app.tabs[at] = t;
	app.ntabs++;
	app.cur = at;
	layout();
	update_title();
}

static void tab_close(int i)
{
	struct term *t = app.tabs[i];
	if (app.paste_tab == t)
		app.paste_tab = NULL;
	term_free(t);
	memmove(&app.tabs[i], &app.tabs[i + 1], (app.ntabs - i - 1) * sizeof *app.tabs);
	if (!--app.ntabs) {
		app.running = false;
		return;
	}
	if (app.cur > i || app.cur >= app.ntabs)
		app.cur--;
	app.selecting = false;
	layout();
	update_title();
}

static void tab_select(int i)
{
	if (i < 0 || i >= app.ntabs || i == app.cur)
		return;
	app.cur = i;
	app.selecting = false;
	app.dirty = true;
	update_title();
}

static uint32_t utf8_next(const char **s)
{
	const uint8_t *p = (const uint8_t *)*s;
	uint32_t c = *p++;
	int n = 0;

	if (c >= 0xf0) { c &= 0x07; n = 3; }
	else if (c >= 0xe0) { c &= 0x0f; n = 2; }
	else if (c >= 0xc0) { c &= 0x1f; n = 1; }
	else if (c >= 0x80) c = 0xfffd;
	while (n-- && (*p & 0xc0) == 0x80)
		c = c << 6 | (*p++ & 0x3f);
	*s = (const char *)p;
	return c;
}

static void build_tabbar(void)
{
	int cols = app.cols;
	if (app.tabbar_cap < cols) {
		app.tabbar_cap = cols;
		app.tabbar = xrealloc(app.tabbar, cols * sizeof *app.tabbar);
	}
	struct cell gap = { .fg = COLOR_TAB_FG, .bg = COLOR_TAB_BG };
	for (int x = 0; x < cols; x++)
		app.tabbar[x] = gap;

	int w = CLAMP(cols / app.ntabs, 6, 30);
	for (int i = 0, x = 0; i < app.ntabs; i++, x += w) {
		app.tab_x0[i] = x;
		app.tab_x1[i] = MIN(x + w, cols);
		if (x >= cols)
			continue;

		bool active = i == app.cur;
		struct cell c = {
			.fg = active ? COLOR_TAB_ACTIVE_FG : COLOR_TAB_FG,
			.bg = active ? COLOR_TAB_ACTIVE_BG : COLOR_TAB_BG,
			.attr = active ? ATTR_BOLD : 0,
		};
		char label[160];
		snprintf(label, sizeof label, " %d %s", i + 1, app.tabs[i]->title);
		const char *s = label;
		for (int k = 0; k < w - 1 && x + k < cols; k++) {
			c.cp = *s ? utf8_next(&s) : 0;
			app.tabbar[x + k] = c;
		}
	}
}

/* ---- buffers and drawing ---- */

static void buf_release(void *data, struct wl_buffer *wl)
{
	(void)wl;
	((struct buffer *)data)->busy = false;
}

static const struct wl_buffer_listener buf_listener = { .release = buf_release };

static void buf_destroy(struct buffer *b)
{
	if (!b->wl)
		return;
	wl_buffer_destroy(b->wl);
	munmap(b->px, b->size);
	b->wl = NULL;
	b->snap.valid = false;
}

static bool buf_create(struct buffer *b, int w, int h)
{
	size_t size = (size_t)w * h * 4;
	int fd = memfd_create("bixis-term", MFD_CLOEXEC);
	if (fd < 0)
		return false;
	if (ftruncate(fd, size) < 0) {
		close(fd);
		return false;
	}
	void *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (px == MAP_FAILED) {
		close(fd);
		return false;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(app.shm, fd, size);
	b->wl = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_XRGB8888);
	wl_buffer_add_listener(b->wl, &buf_listener, b);
	wl_shm_pool_destroy(pool);
	close(fd);

	b->px = px;
	b->w = w;
	b->h = h;
	b->size = size;
	b->busy = false;
	b->snap.valid = false;
	return true;
}

static struct buffer *buf_get(int w, int h)
{
	for (int i = 0; i < 2; i++) {
		struct buffer *b = &app.bufs[i];
		if (b->busy)
			continue;
		if (!b->wl || b->w != w || b->h != h) {
			buf_destroy(b);
			if (!buf_create(b, w, h))
				return NULL;
		}
		return b;
	}
	return NULL;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
	(void)data;
	(void)time;
	wl_callback_destroy(cb);
	app.frame_pending = false;
}

static const struct wl_callback_listener frame_listener = { .done = frame_done };

static void redraw(void)
{
	int W = app.width * app.scale, H = app.height * app.scale;
	struct buffer *b = buf_get(W, H);
	if (!b)
		return; /* both busy: a release will bring us back */
	app.dirty = false;

	if (app.bar)
		build_tabbar();
	struct frame fr = {
		.t = cur(),
		.tabbar = app.bar ? app.tabbar : NULL,
		.focused = app.focused,
		.pad = PADDING * app.scale,
	};
	struct canvas cv = { b->px, W, H };
	int y0, y1;
	render(&cv, &b->snap, app.font, &theme, &fr, &y0, &y1);
	if (cur()->title_changed)
		update_title();
	if (y1 <= y0)
		return;

	wl_surface_attach(app.surf, b->wl, 0, 0);
	wl_surface_damage_buffer(app.surf, 0, y0, W, y1 - y0);
	wl_callback_add_listener(wl_surface_frame(app.surf), &frame_listener, NULL);
	wl_surface_commit(app.surf);
	b->busy = true;
	app.frame_pending = true;
}

/* ---- clipboard ---- */

static void offer_offer(void *data, struct wl_data_offer *o, const char *mime)
{
	(void)o;
	struct offer *of = data;
	for (int i = 0; i < (int)LEN(text_mimes); i++)
		if (!strcmp(mime, text_mimes[i]) && i < of->rank) {
			of->rank = i;
			of->mime = text_mimes[i];
		}
}

static void offer_noop_u32(void *data, struct wl_data_offer *o, uint32_t v)
{
	(void)data; (void)o; (void)v;
}

static const struct wl_data_offer_listener offer_listener = {
	.offer = offer_offer,
	.source_actions = offer_noop_u32,
	.action = offer_noop_u32,
};

static void offer_free(struct wl_data_offer *o)
{
	if (!o)
		return;
	free(wl_data_offer_get_user_data(o));
	wl_data_offer_destroy(o);
}

static void dd_data_offer(void *data, struct wl_data_device *dd, struct wl_data_offer *o)
{
	(void)data; (void)dd;
	struct offer *of = xcalloc(1, sizeof *of);
	of->rank = INT_MAX;
	wl_data_offer_add_listener(o, &offer_listener, of);
}

static void dd_enter(void *data, struct wl_data_device *dd, uint32_t serial,
                     struct wl_surface *s, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *o)
{
	(void)data; (void)dd; (void)serial; (void)s; (void)x; (void)y;
	offer_free(app.dnd_offer);
	app.dnd_offer = o;
}

static void dd_leave(void *data, struct wl_data_device *dd)
{
	(void)data; (void)dd;
	offer_free(app.dnd_offer);
	app.dnd_offer = NULL;
}

static void dd_motion(void *data, struct wl_data_device *dd, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{
	(void)data; (void)dd; (void)t; (void)x; (void)y;
}

static void dd_drop(void *data, struct wl_data_device *dd)
{
	dd_leave(data, dd);
}

static void dd_selection(void *data, struct wl_data_device *dd, struct wl_data_offer *o)
{
	(void)data; (void)dd;
	offer_free(app.sel_offer);
	app.sel_offer = o;
}

static const struct wl_data_device_listener dd_listener = {
	.data_offer = dd_data_offer,
	.enter = dd_enter,
	.leave = dd_leave,
	.motion = dd_motion,
	.drop = dd_drop,
	.selection = dd_selection,
};

static void src_target(void *data, struct wl_data_source *s, const char *mime)
{
	(void)data; (void)s; (void)mime;
}

static void src_send(void *data, struct wl_data_source *s, const char *mime, int32_t fd)
{
	(void)data; (void)s; (void)mime;
	const char *p = app.copy_text;
	size_t left = p ? strlen(p) : 0;

	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
	while (left) {
		ssize_t n = write(fd, p, left);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		p += n;
		left -= n;
	}
	close(fd);
}

static void src_cancelled(void *data, struct wl_data_source *s)
{
	(void)data;
	wl_data_source_destroy(s);
	if (s == app.source) {
		app.source = NULL;
		free(app.copy_text);
		app.copy_text = NULL;
	}
}

static void src_noop(void *data, struct wl_data_source *s)
{
	(void)data; (void)s;
}

static void src_action(void *data, struct wl_data_source *s, uint32_t a)
{
	(void)data; (void)s; (void)a;
}

static const struct wl_data_source_listener src_listener = {
	.target = src_target,
	.send = src_send,
	.cancelled = src_cancelled,
	.dnd_drop_performed = src_noop,
	.dnd_finished = src_noop,
	.action = src_action,
};

static void copy(void)
{
	char *text = term_selection_text(cur());
	if (!text || !*text || !app.ddev) {
		free(text);
		return;
	}
	if (app.source)
		wl_data_source_destroy(app.source);
	free(app.copy_text);
	app.copy_text = text;
	app.source = wl_data_device_manager_create_data_source(app.ddm);
	wl_data_source_add_listener(app.source, &src_listener, NULL);
	for (size_t i = 0; i < LEN(text_mimes); i++)
		wl_data_source_offer(app.source, text_mimes[i]);
	wl_data_device_set_selection(app.ddev, app.source, app.serial);
}

static void paste(void)
{
	struct term *t = cur();

	if (app.source && app.copy_text) {
		term_paste(t, app.copy_text, strlen(app.copy_text));
		return;
	}
	if (!app.sel_offer || app.paste_fd >= 0)
		return;
	struct offer *of = wl_data_offer_get_user_data(app.sel_offer);
	if (!of || !of->mime)
		return;

	int fds[2];
	if (pipe2(fds, O_CLOEXEC) < 0)
		return;
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	wl_data_offer_receive(app.sel_offer, of->mime, fds[1]);
	close(fds[1]);
	wl_display_flush(app.dpy);
	app.paste_fd = fds[0];
	app.paste_tab = t;
	app.paste_len = 0;
}

static void paste_read(void)
{
	char buf[4096];

	for (;;) {
		ssize_t n = read(app.paste_fd, buf, sizeof buf);
		if (n > 0) {
			if (app.paste_len + n > app.paste_cap) {
				app.paste_cap = MAX(app.paste_cap * 2, app.paste_len + n);
				app.paste_buf = xrealloc(app.paste_buf, app.paste_cap);
			}
			memcpy(app.paste_buf + app.paste_len, buf, n);
			app.paste_len += n;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			return;
		break;
	}
	close(app.paste_fd);
	app.paste_fd = -1;
	if (app.paste_tab && app.paste_len)
		term_paste(app.paste_tab, app.paste_buf, app.paste_len);
	app.paste_tab = NULL;
	app.paste_len = 0;
}

/* ---- keyboard ---- */

static unsigned mods(void)
{
	static const struct { const char *name; unsigned bit; } map[] = {
		{ XKB_MOD_NAME_CTRL, CTRL }, { XKB_MOD_NAME_SHIFT, SHIFT },
		{ XKB_MOD_NAME_ALT, ALT },   { XKB_MOD_NAME_LOGO, SUPER },
	};
	unsigned m = 0;
	for (size_t i = 0; i < LEN(map); i++)
		if (xkb_state_mod_name_is_active(app.xkb_state, map[i].name, XKB_STATE_MODS_EFFECTIVE) > 0)
			m |= map[i].bit;
	return m;
}

/* Unshifted Latin symbol of a key, from the first layout that has one. */
static xkb_keysym_t base_sym(xkb_keycode_t kc)
{
	xkb_layout_index_t n = xkb_keymap_num_layouts_for_key(app.keymap, kc);
	for (xkb_layout_index_t l = 0; l < n; l++) {
		const xkb_keysym_t *syms;
		if (xkb_keymap_key_get_syms_by_level(app.keymap, kc, l, 0, &syms) > 0 &&
		    (syms[0] < 0x100 || syms[0] >= 0xfd00))
			return xkb_keysym_to_lower(syms[0]);
	}
	return xkb_keysym_to_lower(xkb_state_key_get_one_sym(app.xkb_state, kc));
}

static int ctrl_char(xkb_keysym_t s)
{
	if (s >= XKB_KEY_a && s <= XKB_KEY_z)
		return s - XKB_KEY_a + 1;
	switch (s) {
	case XKB_KEY_at: case XKB_KEY_space: case XKB_KEY_2: return 0;
	case XKB_KEY_bracketleft: case XKB_KEY_3: return 0x1b;
	case XKB_KEY_backslash: case XKB_KEY_4: return 0x1c;
	case XKB_KEY_bracketright: case XKB_KEY_5: return 0x1d;
	case XKB_KEY_asciicircum: case XKB_KEY_6: return 0x1e;
	case XKB_KEY_underscore: case XKB_KEY_7: case XKB_KEY_slash: return 0x1f;
	case XKB_KEY_8: return 0x7f;
	}
	return -1;
}

static int encode_key(struct term *t, xkb_keycode_t kc, unsigned m, char *buf, size_t size)
{
	xkb_keysym_t sym = xkb_state_key_get_one_sym(app.xkb_state, kc);
	int mod = 1 + (m & SHIFT ? 1 : 0) + (m & ALT ? 2 : 0) + (m & CTRL ? 4 : 0);
	char final = 0;
	int num = 0;
	bool ss3 = false;

	switch (sym) {
	case XKB_KEY_Up:    case XKB_KEY_KP_Up:    final = 'A'; ss3 = t->app_cursor; break;
	case XKB_KEY_Down:  case XKB_KEY_KP_Down:  final = 'B'; ss3 = t->app_cursor; break;
	case XKB_KEY_Right: case XKB_KEY_KP_Right: final = 'C'; ss3 = t->app_cursor; break;
	case XKB_KEY_Left:  case XKB_KEY_KP_Left:  final = 'D'; ss3 = t->app_cursor; break;
	case XKB_KEY_Home:  case XKB_KEY_KP_Home:  final = 'H'; ss3 = t->app_cursor; break;
	case XKB_KEY_End:   case XKB_KEY_KP_End:   final = 'F'; ss3 = t->app_cursor; break;
	case XKB_KEY_F1: final = 'P'; ss3 = true; break;
	case XKB_KEY_F2: final = 'Q'; ss3 = true; break;
	case XKB_KEY_F3: final = 'R'; ss3 = true; break;
	case XKB_KEY_F4: final = 'S'; ss3 = true; break;
	case XKB_KEY_Insert:    case XKB_KEY_KP_Insert: num = 2; break;
	case XKB_KEY_Delete:    case XKB_KEY_KP_Delete: num = 3; break;
	case XKB_KEY_Prior:     case XKB_KEY_KP_Prior:  num = 5; break;
	case XKB_KEY_Next:      case XKB_KEY_KP_Next:   num = 6; break;
	case XKB_KEY_F5: num = 15; break;
	case XKB_KEY_F6: num = 17; break;
	case XKB_KEY_F7: num = 18; break;
	case XKB_KEY_F8: num = 19; break;
	case XKB_KEY_F9: num = 20; break;
	case XKB_KEY_F10: num = 21; break;
	case XKB_KEY_F11: num = 23; break;
	case XKB_KEY_F12: num = 24; break;
	case XKB_KEY_BackSpace:
		if (m & CTRL)
			return snprintf(buf, size, "\x08");
		return snprintf(buf, size, m & ALT ? "\033\x7f" : "\x7f");
	case XKB_KEY_Return: case XKB_KEY_KP_Enter:
		return snprintf(buf, size, m & ALT ? "\033\r" : "\r");
	case XKB_KEY_ISO_Left_Tab:
		return snprintf(buf, size, "\033[Z");
	case XKB_KEY_Escape:
		return snprintf(buf, size, "\033");
	}

	if (final) {
		if (mod > 1)
			return snprintf(buf, size, "\033[1;%d%c", mod, final);
		return snprintf(buf, size, ss3 ? "\033O%c" : "\033[%c", final);
	}
	if (num) {
		if (mod > 1)
			return snprintf(buf, size, "\033[%d;%d~", num, mod);
		return snprintf(buf, size, "\033[%d~", num);
	}

	/* text; xkb already applies Ctrl for Latin keys */
	int n = xkb_state_key_get_utf8(app.xkb_state, kc, buf + 1, size - 1);
	if ((m & CTRL) && (n <= 0 || (uint8_t)buf[1] >= 0x80)) {
		int c = ctrl_char(base_sym(kc));
		if (c >= 0) {
			buf[1] = c;
			n = 1;
		}
	}
	if (n <= 0)
		return 0;
	if (m & ALT) {
		buf[0] = 0x1b;
		return n + 1;
	}
	memmove(buf, buf + 1, n);
	return n;
}

static void scroll_lines(int n)
{
	struct term *t = cur();

	if (t->alt_active) {
		/* less, man, vim without mouse mode: wheel = arrows */
		const char *seq = n < 0 ? (t->app_cursor ? "\033OA" : "\033[A")
		                        : (t->app_cursor ? "\033OB" : "\033[B");
		for (int i = abs(n); i > 0; i--)
			term_write(t, seq, 3);
		return;
	}
	term_scroll_view(t, -n);
	app.dirty = true;
}

static bool action(const struct binding *b)
{
	struct term *t = cur();

	switch (b->act) {
	case ACT_TAB_NEW: tab_new(); break;
	case ACT_TAB_CLOSE: tab_close(app.cur); break;
	case ACT_TAB_NEXT: tab_select((app.cur + 1) % app.ntabs); break;
	case ACT_TAB_PREV: tab_select((app.cur + app.ntabs - 1) % app.ntabs); break;
	case ACT_TAB_GOTO: tab_select(b->arg); break;
	case ACT_COPY: copy(); break;
	case ACT_PASTE: paste(); break;
	case ACT_SCROLL_PAGE:
		if (t->alt_active)
			return false;
		term_scroll_view(t, -b->arg * MAX(1, t->rows - 1));
		app.dirty = true;
		break;
	case ACT_SCROLL_TOP:
		if (t->alt_active)
			return false;
		term_scroll_view(t, t->sb_len);
		app.dirty = true;
		break;
	case ACT_SCROLL_BOTTOM:
		if (t->alt_active)
			return false;
		t->view = 0;
		app.dirty = true;
		break;
	case ACT_FONT_SIZE:
		set_font(b->arg ? app.font_size + b->arg : FONT_SIZE);
		break;
	}
	return true;
}

/* Returns true if the key was taken by a binding. */
static bool key_press(uint32_t key, bool allow_bindings)
{
	xkb_keycode_t kc = key + 8;
	unsigned m = mods();

	if (allow_bindings) {
		xkb_keysym_t base = base_sym(kc);
		for (size_t i = 0; i < LEN(bindings); i++)
			if (bindings[i].mods == m && bindings[i].sym == base && action(&bindings[i]))
				return true;
	}

	char buf[64];
	struct term *t = cur();
	int n = encode_key(t, kc, m, buf, sizeof buf);
	if (n > 0) {
		if (t->view) {
			t->view = 0;
			app.dirty = true;
		}
		term_write(t, buf, n);
	}
	return false;
}

static void repeat_stop(void)
{
	struct itimerspec ts = { 0 };
	timerfd_settime(app.repeat_fd, 0, &ts, NULL);
	app.repeat_key = 0;
}

static void repeat_start(uint32_t key)
{
	struct itimerspec ts = {
		.it_interval = { 0, 1000000000L / app.repeat_rate },
		.it_value = { app.repeat_delay / 1000, app.repeat_delay % 1000 * 1000000L },
	};
	app.repeat_key = key;
	timerfd_settime(app.repeat_fd, 0, &ts, NULL);
}

static void repeat_fire(void)
{
	uint64_t n;
	if (read(app.repeat_fd, &n, sizeof n) != sizeof n || !app.repeat_key)
		return;
	for (n = MIN(n, 8); n > 0; n--)
		key_press(app.repeat_key, false);
}

static void kbd_keymap(void *data, struct wl_keyboard *k, uint32_t fmt, int32_t fd, uint32_t size)
{
	(void)data; (void)k;
	if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED)
		return;
	struct xkb_keymap *km = xkb_keymap_new_from_string(app.xkb, map,
		XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	if (!km)
		return;
	xkb_state_unref(app.xkb_state);
	xkb_keymap_unref(app.keymap);
	app.keymap = km;
	app.xkb_state = xkb_state_new(km);
}

static void kbd_enter(void *data, struct wl_keyboard *k, uint32_t serial,
                      struct wl_surface *s, struct wl_array *keys)
{
	(void)data; (void)k; (void)s; (void)keys;
	app.serial = serial;
	app.focused = true;
	app.dirty = true;
	if (app.ntabs && cur()->focus_events)
		term_write(cur(), "\033[I", 3);
}

static void kbd_leave(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s)
{
	(void)data; (void)k; (void)serial; (void)s;
	app.focused = false;
	app.dirty = true;
	repeat_stop();
	if (app.ntabs && cur()->focus_events)
		term_write(cur(), "\033[O", 3);
}

static void kbd_key(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t time,
                    uint32_t key, uint32_t state)
{
	(void)data; (void)k; (void)time;
	if (!app.xkb_state || !app.ntabs)
		return;
	app.serial = serial;

	if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		bool bound = key_press(key, true);
		if (!bound && app.repeat_rate > 0 && xkb_keymap_key_repeats(app.keymap, key + 8))
			repeat_start(key);
	} else if (key == app.repeat_key) {
		repeat_stop();
	}
}

static void kbd_modifiers(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t dep,
                          uint32_t lat, uint32_t lock, uint32_t group)
{
	(void)data; (void)k; (void)serial;
	if (app.xkb_state)
		xkb_state_update_mask(app.xkb_state, dep, lat, lock, 0, 0, group);
}

static void kbd_repeat_info(void *data, struct wl_keyboard *k, int32_t rate, int32_t delay)
{
	(void)data; (void)k;
	app.repeat_rate = rate;
	app.repeat_delay = delay;
}

static const struct wl_keyboard_listener kbd_listener = {
	.keymap = kbd_keymap,
	.enter = kbd_enter,
	.leave = kbd_leave,
	.key = kbd_key,
	.modifiers = kbd_modifiers,
	.repeat_info = kbd_repeat_info,
};

/* ---- pointer ---- */

/* Surface coords -> grid cell. False if the point is on the tab bar. */
static bool cell_at(double sx, double sy, int *col, int *row)
{
	int pad = PADDING * app.scale;
	int bar = app.bar ? app.font->ch : 0;
	double x = sx * app.scale - pad, y = sy * app.scale - bar - pad;

	*col = CLAMP((int)floor(x / app.font->cw), 0, app.cols - 1);
	*row = CLAMP((int)floor(y / app.font->ch), 0, app.rows - 1);
	return sy * app.scale >= bar;
}

static void ptr_enter(void *data, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
	(void)data; (void)p; (void)s;
	app.px = wl_fixed_to_double(x);
	app.py = wl_fixed_to_double(y);
	if (app.cursor_dev)
		wp_cursor_shape_device_v1_set_shape(app.cursor_dev, serial,
			WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
}

static void ptr_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
	(void)data; (void)p; (void)serial; (void)s;
}

static void ptr_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	(void)data; (void)p; (void)time;
	app.px = wl_fixed_to_double(x);
	app.py = wl_fixed_to_double(y);
	if (!app.selecting || !app.ntabs)
		return;

	struct term *t = cur();
	int col, row;
	cell_at(app.px, app.py, &col, &row);
	int64_t abs = term_view_top(t) + row;
	if (!t->sel_active && abs == t->sel_a_line && col == t->sel_a_col)
		return;
	if (t->sel_active && abs == t->sel_b_line && col == t->sel_b_col)
		return;
	t->sel_active = true;
	t->sel_b_line = abs;
	t->sel_b_col = col;
	app.dirty = true;
}

static void ptr_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time,
                       uint32_t button, uint32_t state)
{
	(void)data; (void)p; (void)time;
	bool down = state == WL_POINTER_BUTTON_STATE_PRESSED;
	app.serial = serial;
	if (!app.ntabs)
		return;

	if (button == BTN_LEFT) {
		if (!down) {
			app.selecting = false;
			return;
		}
		int col, row;
		if (!cell_at(app.px, app.py, &col, &row)) {
			int x = (int)floor((app.px * app.scale - PADDING * app.scale) / app.font->cw);
			for (int i = 0; i < app.ntabs; i++)
				if (x >= app.tab_x0[i] && x < app.tab_x1[i])
					tab_select(i);
			return;
		}
		struct term *t = cur();
		if (t->sel_active)
			app.dirty = true;
		t->sel_active = false;
		t->sel_a_line = t->sel_b_line = term_view_top(t) + row;
		t->sel_a_col = t->sel_b_col = col;
		app.selecting = true;
	} else if (button == BTN_MIDDLE && down) {
		paste();
	}
}

static void ptr_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t v)
{
	(void)data; (void)p; (void)time;
	if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL || app.wheel_discrete || !app.ntabs)
		return;
	app.wheel_acc += wl_fixed_to_double(v);
	int lines = (int)(app.wheel_acc / 10);
	if (lines) {
		app.wheel_acc -= lines * 10;
		scroll_lines(lines);
	}
}

static void ptr_frame(void *data, struct wl_pointer *p)
{
	(void)data; (void)p;
	app.wheel_discrete = false;
}

static void ptr_axis_source(void *data, struct wl_pointer *p, uint32_t src)
{
	(void)data; (void)p; (void)src;
}

static void ptr_axis_stop(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis)
{
	(void)data; (void)p; (void)time; (void)axis;
	app.wheel_acc = 0;
}

static void ptr_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis, int32_t d)
{
	(void)data; (void)p;
	if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL || !app.ntabs)
		return;
	app.wheel_discrete = true;
	scroll_lines(d * WHEEL_LINES);
}

static const struct wl_pointer_listener ptr_listener = {
	.enter = ptr_enter,
	.leave = ptr_leave,
	.motion = ptr_motion,
	.button = ptr_button,
	.axis = ptr_axis,
	.frame = ptr_frame,
	.axis_source = ptr_axis_source,
	.axis_stop = ptr_axis_stop,
	.axis_discrete = ptr_axis_discrete,
};

/* ---- seat, surface, shell ---- */

static void seat_caps(void *data, struct wl_seat *s, uint32_t caps)
{
	(void)data;
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !app.kbd) {
		app.kbd = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(app.kbd, &kbd_listener, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && app.kbd) {
		wl_keyboard_release(app.kbd);
		app.kbd = NULL;
	}
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !app.ptr) {
		app.ptr = wl_seat_get_pointer(s);
		wl_pointer_add_listener(app.ptr, &ptr_listener, NULL);
		if (app.cursor_mgr)
			app.cursor_dev = wp_cursor_shape_manager_v1_get_pointer(app.cursor_mgr, app.ptr);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && app.ptr) {
		if (app.cursor_dev)
			wp_cursor_shape_device_v1_destroy(app.cursor_dev);
		app.cursor_dev = NULL;
		wl_pointer_release(app.ptr);
		app.ptr = NULL;
	}
}

static void seat_name(void *data, struct wl_seat *s, const char *name)
{
	(void)data; (void)s; (void)name;
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_caps,
	.name = seat_name,
};

static void surf_output(void *data, struct wl_surface *s, struct wl_output *o)
{
	(void)data; (void)s; (void)o;
}

static void surf_scale(void *data, struct wl_surface *s, int32_t factor)
{
	(void)data; (void)s;
	if (factor < 1 || factor == app.scale)
		return;
	app.scale = factor;
	wl_surface_set_buffer_scale(app.surf, factor);
	set_font(app.font_size);
}

static void surf_transform(void *data, struct wl_surface *s, uint32_t tr)
{
	(void)data; (void)s; (void)tr;
}

static const struct wl_surface_listener surf_listener = {
	.enter = surf_output,
	.leave = surf_output,
	.preferred_buffer_scale = surf_scale,
	.preferred_buffer_transform = surf_transform,
};

static void wm_ping(void *data, struct xdg_wm_base *wm, uint32_t serial)
{
	(void)data;
	xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

static void xsurf_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
	(void)data;
	xdg_surface_ack_configure(xs, serial);

	int w = app.pend_w, h = app.pend_h;
	if (w <= 0 || h <= 0) {
		w = app.width;
		h = app.height;
	}
	if (w <= 0 || h <= 0) {
		w = DEFAULT_COLS * app.font->cw / app.scale + 2 * PADDING;
		h = DEFAULT_ROWS * app.font->ch / app.scale + 2 * PADDING;
	}
	bool changed = w != app.width || h != app.height;
	app.width = w;
	app.height = h;
	app.configured = true;

	if (!app.ntabs)
		tab_new();
	else if (changed)
		layout();
	app.dirty = true;
}

static const struct xdg_surface_listener xsurf_listener = { .configure = xsurf_configure };

static void top_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h,
                          struct wl_array *states)
{
	(void)data; (void)t; (void)states;
	app.pend_w = w;
	app.pend_h = h;
}

static void top_close(void *data, struct xdg_toplevel *t)
{
	(void)data; (void)t;
	app.running = false;
}

static void top_bounds(void *data, struct xdg_toplevel *t, int32_t w, int32_t h)
{
	(void)data; (void)t; (void)w; (void)h;
}

static void top_caps(void *data, struct xdg_toplevel *t, struct wl_array *caps)
{
	(void)data; (void)t; (void)caps;
}

static const struct xdg_toplevel_listener top_listener = {
	.configure = top_configure,
	.close = top_close,
	.configure_bounds = top_bounds,
	.wm_capabilities = top_caps,
};

static void reg_global(void *data, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t ver)
{
	(void)data;
#define IS(x) (!strcmp(iface, x##_interface.name))
	if (IS(wl_compositor)) {
		app.comp = wl_registry_bind(r, name, &wl_compositor_interface, MIN(ver, 6));
	} else if (IS(wl_shm)) {
		app.shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	} else if (IS(xdg_wm_base)) {
		app.wm = wl_registry_bind(r, name, &xdg_wm_base_interface, MIN(ver, 5));
		xdg_wm_base_add_listener(app.wm, &wm_listener, NULL);
	} else if (IS(wl_seat) && !app.seat) {
		app.seat = wl_registry_bind(r, name, &wl_seat_interface, MIN(ver, 7));
		wl_seat_add_listener(app.seat, &seat_listener, NULL);
	} else if (IS(wl_data_device_manager)) {
		app.ddm = wl_registry_bind(r, name, &wl_data_device_manager_interface, MIN(ver, 3));
	} else if (IS(zxdg_decoration_manager_v1)) {
		app.deco_mgr = wl_registry_bind(r, name, &zxdg_decoration_manager_v1_interface, 1);
	} else if (IS(wp_cursor_shape_manager_v1)) {
		app.cursor_mgr = wl_registry_bind(r, name, &wp_cursor_shape_manager_v1_interface, 1);
	}
#undef IS
}

static void reg_remove(void *data, struct wl_registry *r, uint32_t name)
{
	(void)data; (void)r; (void)name;
}

static const struct wl_registry_listener reg_listener = {
	.global = reg_global,
	.global_remove = reg_remove,
};

/* ---- main loop ---- */

static void run(void)
{
	struct pollfd pfd[3 + MAX_TABS];

	while (app.running) {
		if (app.dirty && app.configured && !app.frame_pending && app.ntabs)
			redraw();

		while (wl_display_prepare_read(app.dpy) != 0)
			wl_display_dispatch_pending(app.dpy);
		if (wl_display_flush(app.dpy) < 0 && errno != EAGAIN) {
			wl_display_cancel_read(app.dpy);
			break;
		}

		int n = 0;
		pfd[n++] = (struct pollfd){ .fd = wl_display_get_fd(app.dpy), .events = POLLIN };
		pfd[n++] = (struct pollfd){ .fd = app.repeat_fd, .events = POLLIN };
		pfd[n++] = (struct pollfd){ .fd = app.paste_fd, .events = POLLIN };
		for (int i = 0; i < app.ntabs; i++)
			pfd[n++] = (struct pollfd){
				.fd = app.tabs[i]->fd,
				.events = POLLIN | (app.tabs[i]->wq_len ? POLLOUT : 0),
			};

		if (poll(pfd, n, -1) < 0) {
			wl_display_cancel_read(app.dpy);
			if (errno == EINTR)
				continue;
			break;
		}
		if (pfd[0].revents & POLLIN) {
			if (wl_display_read_events(app.dpy) < 0)
				break;
		} else {
			wl_display_cancel_read(app.dpy);
		}
		if (pfd[0].revents & (POLLERR | POLLHUP))
			break;

		/* ptys before dispatching Wayland events: those may add or close tabs */
		for (int i = 0; i < app.ntabs; i++) {
			struct term *t = app.tabs[i];
			short ev = pfd[3 + i].revents;
			if (ev & POLLOUT)
				term_flush(t);
			if (ev & (POLLIN | POLLHUP | POLLERR)) {
				term_read(t);
				if (t == cur())
					app.dirty = true;
				else if (t->title_changed && app.bar)
					app.dirty = true;
			}
		}
		for (int i = app.ntabs - 1; i >= 0; i--)
			if (app.tabs[i]->dead)
				tab_close(i);
		if (!app.running)
			break;
		if (pfd[1].revents & POLLIN)
			repeat_fire();
		if (pfd[2].revents)
			paste_read();

		if (wl_display_dispatch_pending(app.dpy) < 0)
			break;
	}
}

int main(int argc, char **argv)
{
	if (argc > 1 && (!strcmp(argv[1], "-v") || !strcmp(argv[1], "--version"))) {
		puts("bixis-term " VERSION);
		return 0;
	}

	setlocale(LC_CTYPE, "");
	if (MB_CUR_MAX < 2)
		setlocale(LC_CTYPE, "C.UTF-8");
	signal(SIGCHLD, SIG_IGN); /* auto-reap shells */
	signal(SIGPIPE, SIG_IGN);
	term_setup(palette, COLOR_FG, COLOR_BG, SCROLLBACK);

	app.scale = 1;
	app.paste_fd = -1;
	app.running = true;

	app.dpy = wl_display_connect(NULL);
	if (!app.dpy) {
		fputs("bixis-term: can't connect to a Wayland display\n", stderr);
		return 1;
	}
	struct wl_registry *reg = wl_display_get_registry(app.dpy);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(app.dpy);
	if (!app.comp || !app.shm || !app.wm) {
		fputs("bixis-term: compositor lacks wl_compositor/wl_shm/xdg_wm_base\n", stderr);
		return 1;
	}

	app.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	app.repeat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (app.seat && app.ddm) {
		app.ddev = wl_data_device_manager_get_data_device(app.ddm, app.seat);
		wl_data_device_add_listener(app.ddev, &dd_listener, NULL);
	}
	set_font(FONT_SIZE);

	app.surf = wl_compositor_create_surface(app.comp);
	wl_surface_add_listener(app.surf, &surf_listener, NULL);
	app.xsurf = xdg_wm_base_get_xdg_surface(app.wm, app.surf);
	xdg_surface_add_listener(app.xsurf, &xsurf_listener, NULL);
	app.top = xdg_surface_get_toplevel(app.xsurf);
	xdg_toplevel_add_listener(app.top, &top_listener, NULL);
	xdg_toplevel_set_app_id(app.top, "bixis-term");
	xdg_toplevel_set_title(app.top, "bixis-term");
	if (app.deco_mgr) {
		struct zxdg_toplevel_decoration_v1 *d =
			zxdg_decoration_manager_v1_get_toplevel_decoration(app.deco_mgr, app.top);
		zxdg_toplevel_decoration_v1_set_mode(d, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
	wl_surface_commit(app.surf);

	run();

	while (app.ntabs)
		term_free(app.tabs[--app.ntabs]);
	wl_display_disconnect(app.dpy);
	return 0;
}
