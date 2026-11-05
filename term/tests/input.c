/* Exercise real Wayland input callbacks without a display or child process. */
#define main bixis_main
#include "../src/main.c"
#undef main

static int fails, read_fd;

#define CHECK(c) do { \
	if (!(c)) { \
		fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #c); \
		fails++; \
	} \
} while (0)

static void expect(const char *s)
{
	char buf[1024];
	ssize_t n = read(read_fd, buf, sizeof buf);
	if (n < 0 && errno == EAGAIN)
		n = 0;
	CHECK(n == (ssize_t)strlen(s));
	if (n == (ssize_t)strlen(s))
		CHECK(!memcmp(buf, s, n));
}

static void wheel(int steps)
{
	ptr_axis_discrete(NULL, NULL, WL_POINTER_AXIS_VERTICAL_SCROLL, steps);
	ptr_axis(NULL, NULL, 0, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(steps * 10));
	ptr_frame(NULL, NULL);
}

static void shift(bool on)
{
	xkb_mod_index_t idx = xkb_keymap_mod_get_index(app.keymap, XKB_MOD_NAME_SHIFT);
	xkb_state_update_mask(app.xkb_state, on ? 1u << idx : 0, 0, 0, 0, 0, 0);
}

int main(void)
{
	int fds[2];
	if (pipe2(fds, O_NONBLOCK) < 0)
		return 1;
	read_fd = fds[0];
	struct term t = { .cols = 300, .rows = 100, .fd = fds[1], .sb_len = 100,
	                 .alt_active = true, .mouse_mode = MOUSE_BUTTON, .mouse_sgr = true };
	struct font font = { .cw = 16, .ch = 32 };
	app.tabs[0] = &t;
	app.ntabs = 1;
	app.font = &font;
	app.scale = 2;
	app.cols = t.cols;
	app.rows = t.rows;
	app.px = PADDING + 10 * 8 + 1;
	app.py = PADDING + 4 * 16 + 1;
	app.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	app.keymap = xkb_keymap_new_from_names(app.xkb, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	app.xkb_state = xkb_state_new(app.keymap);
	if (!app.xkb || !app.keymap || !app.xkb_state)
		return 1;

	/* Shift-arrows select on the primary screen, but reach full-screen apps. */
	t.alt_active = false;
	t.cx = 5; t.cy = 2;
	shift(true);
	key_press(KEY_LEFT, true);
	CHECK(t.sel_active && t.sel_a_col == 4 && t.sel_b_col == 4);
	key_press(KEY_LEFT, false);
	CHECK(t.sel_a_col == 3 && t.sel_b_col == 4);
	key_press(KEY_RIGHT, false);
	CHECK(t.sel_a_col == 4 && t.sel_b_col == 4);
	key_press(KEY_RIGHT, false);
	CHECK(!t.sel_active);
	expect("");
	t.alt_active = true;
	key_press(KEY_LEFT, true);
	expect("\033[1;2D");
	shift(false);
	t.key_selecting = false;

	/* One physical wheel notch = one report, never arrows or duplicate axis events. */
	wheel(-1);
	expect("\033[<64;11;5M");
	wheel(2);
	expect("\033[<65;11;5M\033[<65;11;5M");
	CHECK(t.view == 0);
	ptr_axis(NULL, NULL, 0, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(-5));
	expect("");
	ptr_axis(NULL, NULL, 0, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(-5));
	expect("\033[<64;11;5M");
	ptr_axis_stop(NULL, NULL, 0, WL_POINTER_AXIS_VERTICAL_SCROLL);
	CHECK(app.wheel_acc == 0);

	/* Mouse input works on the primary screen too. Shift keeps local scrollback. */
	t.alt_active = false;
	wheel(-1);
	expect("\033[<64;11;5M");
	shift(true);
	wheel(-1);
	expect("");
	CHECK(t.view == WHEEL_LINES);
	shift(false);
	wheel(-1);
	expect("");
	CHECK(t.view == 2 * WHEEL_LINES);
	t.view = 0;

	/* Applications without mouse reporting retain the less/man arrow fallback. */
	t.alt_active = true;
	t.mouse_mode = MOUSE_OFF;
	wheel(-1);
	expect("\033[A\033[A\033[A");
	t.app_cursor = true;
	wheel(1);
	expect("\033OB\033OB\033OB");
	shift(true);
	wheel(-1);
	expect("");
	shift(false);

	/* Tab bar is not part of the application grid; coordinates exclude it. */
	t.mouse_mode = MOUSE_DRAG;
	app.bar = true;
	app.py += 16;
	wheel(-1);
	expect("\033[<64;11;5M");
	app.py = 5;
	wheel(-1);
	expect("");
	app.py = PADDING + 4 * 16 + 1 + 16;

	ptr_button(NULL, NULL, 1, 0, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
	expect("\033[<0;11;5M");
	CHECK(!app.selecting);
	ptr_motion(NULL, NULL, 0, wl_fixed_from_double(app.px + 8), wl_fixed_from_double(app.py));
	expect("\033[<32;12;5M");
	ptr_motion(NULL, NULL, 0, wl_fixed_from_double(app.px + 1), wl_fixed_from_double(app.py));
	expect(""); /* no duplicate movement within a cell */
	ptr_button(NULL, NULL, 1, 0, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
	expect("\033[<0;12;5m");
	ptr_motion(NULL, NULL, 0, wl_fixed_from_double(app.px + 8), wl_fixed_from_double(app.py));
	expect(""); /* drag mode ignores motion without a button */
	t.mouse_mode = MOUSE_MOTION;
	ptr_motion(NULL, NULL, 0, wl_fixed_from_double(app.px + 8), wl_fixed_from_double(app.py));
	expect("\033[<35;14;5M");
	shift(true);
	ptr_button(NULL, NULL, 1, 0, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
	CHECK(app.selecting);
	ptr_button(NULL, NULL, 1, 0, BTN_LEFT, WL_POINTER_BUTTON_STATE_RELEASED);
	CHECK(!app.selecting);
	expect("");
	shift(false);

	ptr_button(NULL, NULL, 1, 0, BTN_RIGHT, WL_POINTER_BUTTON_STATE_PRESSED);
	expect("\033[<2;14;5M");
	shift(true);
	ptr_button(NULL, NULL, 1, 0, BTN_RIGHT, WL_POINTER_BUTTON_STATE_RELEASED);
	expect("\033[<6;14;5m"); /* Shift during a drag must not leave a button stuck. */
	shift(false);

	/* SGR handles 4K-wide grids, legacy encoding stops at its byte limit. */
	CHECK(term_mouse(&t, 64, 299, 99, MOUSE_CTRL | MOUSE_ALT, MOUSE_PRESS));
	expect("\033[<88;300;100M");
	t.mouse_sgr = false;
	CHECK(term_mouse(&t, 0, 0, 0, 0, MOUSE_PRESS));
	expect("\033[M !!");
	CHECK(term_mouse(&t, 2, 0, 0, MOUSE_CTRL, MOUSE_RELEASE));
	expect("\033[M3!!");
	CHECK(term_mouse(&t, 65, 0, 0, 0, MOUSE_PRESS));
	expect("\033[Ma!!");
	CHECK(!term_mouse(&t, 64, 223, 0, 0, MOUSE_PRESS));
	CHECK(!term_mouse(&t, 64, 0, 0, 0, MOUSE_RELEASE));
	expect("");

	xkb_state_unref(app.xkb_state);
	xkb_keymap_unref(app.keymap);
	xkb_context_unref(app.xkb);
	close(fds[0]);
	close(fds[1]);
	free(t.wq);
	if (fails)
		return 1;
	puts("all input tests passed");
	return 0;
}
