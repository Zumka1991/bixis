/* Repeatable CPU-only workloads. No display, shell or audio services needed. */
#include <locale.h>
#include <time.h>

#include "../src/term.c"
#include "render.h"

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static struct term *model(int cols, int rows)
{
	struct term *t = xcalloc(1, sizeof *t);
	t->cols = cols;
	t->rows = rows;
	t->fd = -1;
	t->pri = screen_new(cols, rows);
	t->alt = screen_new(cols, rows);
	t->screen = t->pri;
	t->tabs = xcalloc(cols, 1);
	t->sb_cap = 5000;
	t->sb = xcalloc(t->sb_cap, sizeof *t->sb);
	reset_tabs(t);
	soft_reset(t);
	return t;
}

static void parser(const char *label, const char *s, int cols, int rows)
{
	struct term *t = model(cols, rows);
	size_t n = strlen(s), count = 16 * 1024 * 1024 / n;
	double start = now();
	for (size_t i = 0; i < count; i++)
		term_input(t, (const uint8_t *)s, n);
	printf("parser %-12s %.1f MiB/s\n", label, count * n / (now() - start) / 1048576);
	term_free(t);
}

static size_t pixels(const struct damage *damage)
{
	size_t n = 0;
	for (int i = 0; i < damage->len; i++)
		n += (size_t)damage->rects[i].w * damage->rects[i].h;
	return n;
}

static void queue(size_t size)
{
	int fds[2];
	if (pipe2(fds, O_NONBLOCK))
		abort();
	struct term t = { .fd = fds[1] };
	char *data = xmalloc(size), buf[65536];
	memset(data, 'x', size);
	term_write(&t, data, size);
	size_t steps = 0;
	double start = now();
	while (t.wq_len) {
		if (read(fds[0], buf, sizeof buf) <= 0)
			abort();
		term_flush(&t);
		steps++;
	}
	printf("queue %2zu MiB        %.3f ms (%zu partial flushes)\n",
	       size / 1048576, (now() - start) * 1000, steps);
	close(fds[0]);
	close(fds[1]);
	free(data);
	free(t.wq);
}

int main(void)
{
	setlocale(LC_CTYPE, "C.UTF-8");
	uint32_t palette[16] = {0};
	term_setup(palette, 0xd0d0d0, 0x161616, 5000);
	struct font *font = font_open("monospace", 11, 2);
	int cols = (3840 - 24) / font->cw, rows = (2160 - 24) / font->ch;
	printf("4K/200%%: %dx%d cells, %dx%d px/cell\n", cols, rows, font->cw, font->ch);
	parser("short ASCII", "INFO request=123456 status=ok elapsed=0.002s\r\n", cols, rows);
	parser("ANSI", "\033[32mINFO\033[0m request=123456 status=ok elapsed=0.002s\r\n", cols, rows);
	parser("UTF-8", "Проверка скорости терминала: сообщение принято успешно\r\n", cols, rows);
	struct term *t = model(cols, rows);
	t->cursor_visible = false;
	for (int y = 0; y < rows; y++)
		for (int x = 0; x < cols; x++) {
			t->screen[y][x] = t->tmpl;
			t->screen[y][x].cp = 'a' + x % 26;
		}
	struct canvas cv = { .w = 3840, .h = 2160, .px = xcalloc(3840 * 2160, 4) };
	struct snapshot snap = {0};
	struct damage damage = {0};
	struct frame frame = { .t = t, .focused = true, .pad = 12 };
	struct theme theme = { .fg = 0xd0d0d0, .bg = 0x161616 };
	render(&cv, &snap, font, &theme, &frame, NULL, &damage);
	double start = now();
	for (int i = 0; i < 10000; i++)
		render(&cv, &snap, font, &theme, &frame, &snap, &damage);
	printf("render unchanged    %.3f ms/call\n", (now() - start) * 1000 / 10000);
	start = now();
	for (int i = 0; i < 1000; i++) {
		t->screen[rows - 1][0].cp = 'A' + i % 26;
		render(&cv, &snap, font, &theme, &frame, &snap, &damage);
	}
	printf("render one cell     %.3f ms/call, %zu damaged pixels\n",
	       (now() - start) * 1000 / 1000, pixels(&damage));
	t->screen[0][0].cp = 'Z';
	t->screen[rows - 1][0].cp = 'Z';
	render(&cv, &snap, font, &theme, &frame, &snap, &damage);
	printf("two distant rows    %d rectangles, %zu damaged pixels\n", damage.len, pixels(&damage));
	start = now();
	for (int i = 0; i < 100; i++) {
		for (int y = 0; y < rows; y++)
			for (int x = 0; x < cols; x++)
				t->screen[y][x].cp = 'a' + i % 26;
		render(&cv, &snap, font, &theme, &frame, &snap, &damage);
	}
	printf("render full grid    %.3f ms/call (includes cell edits)\n", (now() - start) * 10);
	free(cv.px);
	free(snap.cells);
	free(damage.rects);
	term_free(t);
	font_close(font);
	queue(1024 * 1024);
	queue(8 * 1024 * 1024);
	queue(32 * 1024 * 1024);
	return 0;
}
