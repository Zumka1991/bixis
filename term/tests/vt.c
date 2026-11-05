/* Emulator tests: feed escape sequences, check the grid. Run with `make check`. */
#include "term.h"
#include "util.h"

#include <locale.h>
#include <stdlib.h>
#include <string.h>

static int fails;

#define CHECK(c) do { \
	if (!(c)) { \
		fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #c); \
		fails++; \
	} \
} while (0)

#define CHECK_ROW(t, y, s) do { \
	const char *got_ = row(t, y); \
	if (strcmp(got_, s)) { \
		fprintf(stderr, "%s:%d: FAIL: row %d = \"%s\", want \"%s\"\n", \
		        __FILE__, __LINE__, y, got_, s); \
		fails++; \
	} \
} while (0)

static void feed(struct term *t, const char *s)
{
	term_input(t, (const uint8_t *)s, strlen(s));
}

static const char *line(const struct cell *c, int n)
{
	static char buf[4096];
	int len = 0;
	for (int x = 0; x < n; x++)
		if (!(c[x].attr & ATTR_WDUMMY))
			len += utf8_encode(buf + len, c[x].cp ? c[x].cp : ' ');
	while (len && buf[len - 1] == ' ')
		len--;
	buf[len] = 0;
	return buf;
}

static const char *row(struct term *t, int y)
{
	return line(t->screen[y], t->cols);
}

static struct term *mk(int cols, int rows)
{
	return term_new(cols, rows, 8, 16);
}

static void test_basic(void)
{
	struct term *t = mk(20, 5);
	feed(t, "hello\r\nworld");
	CHECK_ROW(t, 0, "hello");
	CHECK_ROW(t, 1, "world");
	CHECK(t->cx == 5 && t->cy == 1);
	feed(t, "\033[3;4HX");
	CHECK(t->screen[2][3].cp == 'X');
	feed(t, "\033[2J");
	CHECK_ROW(t, 0, "");
	CHECK_ROW(t, 1, "");
	term_free(t);
}

static void test_wrap(void)
{
	struct term *t = mk(10, 3);
	feed(t, "0123456789");
	CHECK(t->cy == 0 && t->wrapnext);
	feed(t, "ab");
	CHECK_ROW(t, 0, "0123456789");
	CHECK_ROW(t, 1, "ab");
	feed(t, "\033[?7l\r\n0123456789xyz");
	CHECK_ROW(t, 2, "012345678z");
	term_free(t);
}

static void test_utf8_and_wide(void)
{
	struct term *t = mk(20, 3);
	const char *s = "Привет 你";
	/* split in the middle of a multibyte sequence */
	term_input(t, (const uint8_t *)s, 3);
	term_input(t, (const uint8_t *)s + 3, strlen(s) - 3);
	CHECK_ROW(t, 0, "Привет 你");
	CHECK(t->screen[0][7].attr & ATTR_WIDE);
	CHECK(t->screen[0][8].attr & ATTR_WDUMMY);
	CHECK(t->cx == 9);
	/* overwriting half of a wide char clears the other half */
	feed(t, "\033[1;9Hx");
	CHECK(!(t->screen[0][7].attr & ATTR_WIDE) && t->screen[0][7].cp == 0);
	term_free(t);
}

static void test_scroll_region(void)
{
	struct term *t = mk(10, 5);
	feed(t, "a\r\nb\r\nc\r\nd\r\ne");
	feed(t, "\033[2;4r");          /* rows 2..4 */
	feed(t, "\033[4;1H\n");        /* LF at bottom margin scrolls region only */
	CHECK_ROW(t, 0, "a");
	CHECK_ROW(t, 1, "c");
	CHECK_ROW(t, 2, "d");
	CHECK_ROW(t, 3, "");
	CHECK_ROW(t, 4, "e");
	CHECK(t->sb_len == 0);         /* region scroll doesn't feed scrollback */
	feed(t, "\033[2;1H\033M");     /* reverse index at top margin */
	CHECK_ROW(t, 1, "");
	CHECK_ROW(t, 2, "c");
	term_free(t);
}

static void test_scrollback(void)
{
	struct term *t = mk(10, 3);
	for (int i = 1; i <= 10; i++) {
		char buf[16];
		snprintf(buf, sizeof buf, "%s%d", i > 1 ? "\r\n" : "", i);
		feed(t, buf);
	}
	CHECK_ROW(t, 0, "8");
	CHECK_ROW(t, 2, "10");
	CHECK(t->sb_len == 7);
	int n;
	const struct cell *c = term_abs_line(t, 0, &n);
	CHECK(c && !strcmp(line(c, n), "1"));
	term_scroll_view(t, 2);
	CHECK(term_view_top(t) == 5);
	c = term_abs_line(t, term_view_top(t), &n);
	CHECK(c && !strcmp(line(c, n), "6"));
	feed(t, "\033[3J");
	CHECK(t->sb_len == 0 && t->view == 0);
	term_free(t);
}

static void test_alt_screen(void)
{
	struct term *t = mk(10, 3);
	feed(t, "shell$ ");
	feed(t, "\033[?1049h");
	CHECK(t->alt_active);
	CHECK_ROW(t, 0, "");
	feed(t, "\033[2;2Hvim");
	feed(t, "\033[?1049l");
	CHECK(!t->alt_active);
	CHECK_ROW(t, 0, "shell$");
	CHECK(t->cx == 7 && t->cy == 0);
	term_free(t);
}

static void test_edit(void)
{
	struct term *t = mk(12, 3);
	feed(t, "abcdef\r\033[2@");
	CHECK_ROW(t, 0, "  abcdef");
	feed(t, "\033[2P");
	CHECK_ROW(t, 0, "abcdef");
	feed(t, "\033[1;3H\033[K");
	CHECK_ROW(t, 0, "ab");
	feed(t, "\r\033[3X");
	CHECK_ROW(t, 0, "");
	feed(t, "\ra\033[3b");
	CHECK_ROW(t, 0, "aaaa");
	feed(t, "\r\n\033(0lqk\033(B");
	CHECK(t->screen[1][0].cp == 0x250c && t->screen[1][1].cp == 0x2500);
	term_free(t);
}

static void test_mouse_modes(void)
{
	struct term *t = mk(20, 5);
	struct term *other = mk(20, 5);
	CHECK(t->mouse_mode == MOUSE_OFF && !t->mouse_sgr);
	feed(t, "\033[?1000;1006h");
	CHECK(t->mouse_mode == MOUSE_BUTTON && t->mouse_sgr);
	CHECK(other->mouse_mode == MOUSE_OFF && !other->mouse_sgr);
	feed(t, "\033[?1002h");
	CHECK(t->mouse_mode == MOUSE_DRAG);
	feed(t, "\033[?1003h");
	CHECK(t->mouse_mode == MOUSE_MOTION);
	feed(t, "\033[?1003;1006l");
	CHECK(t->mouse_mode == MOUSE_OFF && !t->mouse_sgr);
	feed(t, "\033[?1002;1006h\033c");
	CHECK(t->mouse_mode == MOUSE_OFF && !t->mouse_sgr);
	term_free(other);
	term_free(t);
}

static void test_sgr(void)
{
	struct term *t = mk(10, 2);
	feed(t, "\033[1;4;38;2;10;20;30mA\033[0;48:2::1:2:3mB\033[38;5;196mC\033[0mD");
	struct cell *r = t->screen[0];
	CHECK((r[0].attr & (ATTR_BOLD | ATTR_UNDERLINE)) == (ATTR_BOLD | ATTR_UNDERLINE));
	CHECK(r[0].fg == 0x0a141e);
	CHECK(r[1].bg == 0x010203 && !(r[1].attr & ATTR_BOLD));
	CHECK(r[2].fg == 0xff0000);
	CHECK(r[3].attr == 0);
	term_free(t);
}

static void test_resize(void)
{
	struct term *t = mk(10, 5);
	feed(t, "1\r\n2\r\n3\r\n4\r\n5");
	term_resize(t, 6, 3, 8, 16);
	CHECK(t->cy == 2);
	CHECK_ROW(t, 2, "5");          /* cursor line stays visible */
	CHECK(t->sb_len == 2);
	term_resize(t, 20, 6, 8, 16);
	CHECK(t->cols == 20 && t->rows == 6);
	CHECK_ROW(t, 2, "5");
	term_free(t);
}

static void test_selection(void)
{
	struct term *t = mk(20, 3);
	feed(t, "hello world\r\nsecond");
	t->sel_active = true;
	t->sel_a_line = term_view_top(t);
	t->sel_a_col = 6;
	t->sel_b_line = term_view_top(t) + 1;
	t->sel_b_col = 2;
	char *s = term_selection_text(t);
	CHECK(s && !strcmp(s, "world\nsec"));
	free(s);
	term_free(t);
}

static void test_osc_title(void)
{
	struct term *t = mk(10, 2);
	feed(t, "\033]0;my title\007x\033]2;second\033\\y");
	CHECK(!strcmp(t->title, "second"));
	CHECK_ROW(t, 0, "xy");
	term_free(t);
}

int main(void)
{
	setlocale(LC_CTYPE, "C.UTF-8");
	setenv("SHELL", "/bin/cat", 1); /* a child that never prints by itself */
	static const uint32_t pal[16] = {
		0x000000, 0xff0000, 0x00ff00, 0xffff00, 0x0000ff, 0xff00ff, 0x00ffff, 0xffffff,
	};
	term_setup(pal, 0xd0d0d0, 0x161616, 100);

	test_basic();
	test_wrap();
	test_utf8_and_wide();
	test_scroll_region();
	test_scrollback();
	test_alt_screen();
	test_mouse_modes();
	test_edit();
	test_sgr();
	test_resize();
	test_selection();
	test_osc_title();

	if (fails) {
		fprintf(stderr, "%d check(s) failed\n", fails);
		return 1;
	}
	puts("all tests passed");
	return 0;
}
