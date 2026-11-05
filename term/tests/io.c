/* Exercise backpressure, clipboard lifetimes and background title callbacks. */
#define main bixis_main
#include "../src/main.c"
#undef main

static int fails;
#define CHECK(c) do { \
	if (!(c)) { \
		fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #c); \
		fails++; \
	} \
} while (0)

static void queue(void)
{
	int fds[2];
	if (pipe2(fds, O_NONBLOCK | O_CLOEXEC))
		abort();
	size_t n = 2 * 1024 * 1024, extra = 128 * 1024;
	char *data = xmalloc(n + extra), *got = xmalloc(n + extra);
	for (size_t i = 0; i < n + extra; i++)
		data[i] = i % 251;
	struct term t = { .fd = fds[1] };
	term_write(&t, data, n);
	CHECK(t.wq_len > 0);
	ssize_t count = read(fds[0], got, n);
	CHECK(count > 0);
	size_t used = count > 0 ? (size_t)count : 0;
	term_flush(&t);
	CHECK(t.wq_off > 0);
	/* Append while the front of the allocation has already been consumed. */
	term_write(&t, data + n, extra);
	for (int i = 0; i < 10000 && used < n + extra; i++) {
		count = read(fds[0], got + used, n + extra - used);
		if (count > 0)
			used += count;
		else
			CHECK(count < 0 && errno == EAGAIN);
		term_flush(&t);
	}
	CHECK(used == n + extra && !memcmp(data, got, used));
	CHECK(t.wq_len == 0 && t.wq_off == 0);
	CHECK(t.wq == NULL && t.wq_cap == 0);
	close(fds[0]);
	close(fds[1]);
	free(t.wq);
	free(data);
	free(got);
}

static struct clipboard *clipboard(size_t len)
{
	struct clipboard *clip = xcalloc(1, sizeof *clip);
	clip->len = len;
	clip->refs = 1;
	clip->text = xmalloc(len + 1);
	for (size_t i = 0; i < len; i++)
		clip->text[i] = 'a' + i % 26;
	clip->text[len] = 0;
	return clip;
}

static void clipboard_send(void)
{
	int a[2], b[2];
	if (pipe2(a, O_NONBLOCK | O_CLOEXEC) || pipe2(b, O_NONBLOCK | O_CLOEXEC))
		abort();
	size_t size = 1024 * 1024;
	app.copy = clipboard(size);
	/* No reader is consuming: these callbacks must return immediately. */
	src_send(NULL, NULL, NULL, a[1]);
	src_send(NULL, NULL, NULL, b[1]);
	CHECK(app.sends && app.sends->next);
	CHECK(fcntl(b[1], F_GETFL) & O_NONBLOCK);
	CHECK(fcntl(b[1], F_GETFD) & FD_CLOEXEC);
	CHECK(app.copy->refs == 3);
	clipboard_unref(app.copy);
	app.copy = clipboard(4); /* New selection while the old transfers are pending. */
	close(a[0]);            /* First receiver disconnects without reading. */
	size_t total = 0;
	bool eof = false, correct = true;
	char data[65536];
	for (int step = 0; step < 10000 && (!eof || app.sends); step++) {
		ssize_t n = read(b[0], data, sizeof data);
		if (n > 0) {
			for (ssize_t i = 0; i < n; i++)
				if (data[i] != (char)('a' + (total + i) % 26))
					correct = false;
			total += n;
		} else if (n == 0) {
			eof = true;
		} else {
			CHECK(errno == EAGAIN);
		}
		struct pollfd pfd[2];
		int count = 0;
		for (struct transfer *tr = app.sends; tr; tr = tr->next)
			pfd[count++] = (struct pollfd){ .fd = tr->fd, .events = POLLOUT };
		CHECK(poll(pfd, count, 0) >= 0);
		transfers_ready(pfd);
	}
	CHECK(eof && !app.sends && total == size && correct);
	close(b[0]);
	clipboard_unref(app.copy);
	app.copy = NULL;
	if (pipe2(a, O_NONBLOCK | O_CLOEXEC))
		abort();
	src_send(NULL, NULL, NULL, a[1]);
	CHECK(read(a[0], data, sizeof data) == 0);
	close(a[0]);
}

static void clipboard_receive(void)
{
	int src[2], dst[2];
	if (pipe2(src, O_NONBLOCK | O_CLOEXEC) || pipe2(dst, O_NONBLOCK | O_CLOEXEC))
		abort();
	struct term t = { .fd = dst[1], .bracketed_paste = true };
	const char text[] = "one\r\ntwo\n\033[201~";
	CHECK(write(src[1], text, sizeof text - 1) == sizeof text - 1);
	close(src[1]);
	app.paste_fd = src[0];
	app.paste_tab = &t;
	paste_read();
	char got[128] = {0};
	const char expected[] = "\033[200~one\rtwo\r[201~\033[201~";
	ssize_t n = read(dst[0], got, sizeof got);
	CHECK(n == sizeof expected - 1 && !memcmp(got, expected, sizeof expected - 1));
	CHECK(app.paste_fd == -1 && app.paste_tab == NULL);
	CHECK(app.paste_buf == NULL && app.paste_cap == 0);
	close(dst[0]);
	close(dst[1]);
	free(t.wq);

	/* An always-readable source yields after 256 KiB, then completes normally. */
	int fd = memfd_create("bixis-test-paste", MFD_CLOEXEC);
	CHECK(fd >= 0);
	char *large = xmalloc(1024 * 1024);
	memset(large, 'x', 1024 * 1024);
	CHECK(write(fd, large, 1024 * 1024) == 1024 * 1024);
	CHECK(lseek(fd, 0, SEEK_SET) == 0);
	app.paste_fd = fd;
	app.paste_tab = NULL;
	paste_read();
	CHECK(app.paste_fd >= 0 && app.paste_len == 256 * 1024);
	for (int i = 0; i < 5 && app.paste_fd >= 0; i++)
		paste_read();
	CHECK(app.paste_fd == -1 && app.paste_buf == NULL && app.paste_cap == 0);
	free(large);
}

static void titles(void)
{
	struct term a = {0}, b = {0};
	const char title[] = "\033]2;build job\007";
	term_input(&a, (const uint8_t *)title, sizeof title - 1);
	term_input(&b, (const uint8_t *)title, sizeof title - 1);
	CHECK(a.title_changed && b.title_changed);
	app.tabs[0] = &a;
	app.tabs[1] = &b;
	app.cur = 0;
	app.ntabs = 2;
	app.cols = 1; /* Even an off-screen tab must consume its title update. */
	build_tabbar();
	CHECK(a.title_changed && !b.title_changed);
	term_input(&b, (const uint8_t *)title, sizeof title - 1);
	CHECK(!b.title_changed);
	const char changed[] = "\033]2;finished\007";
	term_input(&b, (const uint8_t *)changed, sizeof changed - 1);
	CHECK(b.title_changed);
	free(app.tabbar);
}

int main(void)
{
	signal(SIGPIPE, SIG_IGN);
	alarm(10); /* Catch accidental blocking writes in a regression. */
	queue();
	clipboard_send();
	clipboard_receive();
	titles();
	alarm(0);
	if (fails)
		return 1;
	puts("all I/O tests passed");
	return 0;
}
