/* Real keyboard callbacks and audio subprocesses, using fake wpctl/pactl. */
#define main bixis_main
#include "../src/main.c"
#undef main

#include <sys/stat.h>
#include <wlr/interfaces/wlr_keyboard.h>

static int fails;
static char log_path[256];

#define CHECK(c) do { \
	if (!(c)) { \
		fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #c); \
		fails++; \
	} \
} while (0)

static void helper(const char *dir, const char *name)
{
	char path[256];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	FILE *f = fopen(path, "w");
	if (!f)
		abort();
	fprintf(f, "#!/bin/sh\nprintf '%s' >> \"$BIXIS_TEST_AUDIO_LOG\"\n"
	           "printf ' <%%s>' \"$@\" >> \"$BIXIS_TEST_AUDIO_LOG\"\n"
	           "printf '\\n' >> \"$BIXIS_TEST_AUDIO_LOG\"\n", name);
	fclose(f);
	CHECK(chmod(path, 0700) == 0);
}

static void expect_audio(const char *expected)
{
	int status = 0;
	CHECK(waitpid(-1, &status, 0) > 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	FILE *f = fopen(log_path, "r");
	char buf[512] = {0};
	if (f) {
		size_t n = fread(buf, 1, sizeof buf - 1, f);
		buf[n] = 0;
		fclose(f);
	}
	CHECK(!strcmp(buf, expected));
	f = fopen(log_path, "w");
	if (!f)
		abort();
	fclose(f);
}

static void no_child(void)
{
	int status;
	errno = 0;
	CHECK(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
}

static void key(struct keyboard *k, uint32_t code, bool down)
{
	struct wlr_keyboard_key_event ev = {
		.keycode = code,
		.state = down ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED,
	};
	kb_key(&k->key, &ev);
}

int main(void)
{
	char dir[] = "/tmp/bixis-volume-XXXXXX";
	if (!mkdtemp(dir))
		return 1;
	snprintf(log_path, sizeof log_path, "%s/log", dir);
	CHECK(setenv("BIXIS_TEST_AUDIO_LOG", log_path, 1) == 0);
	CHECK(setenv("PATH", dir, 1) == 0);
	helper(dir, "wpctl");
	helper(dir, "pactl");

	/* Existing configs inherit media defaults; explicit mappings override them. */
	char config_path[256];
	snprintf(config_path, sizeof config_path, "%s/config", dir);
	FILE *f = fopen(config_path, "w");
	if (!f)
		abort();
	fputs("[bindings]\nXF86AudioMute = volume_down\n", f);
	fclose(f);
	CHECK(config_load(&server.cfg, config_path));
	CHECK(server.cfg.nbinds == 3);
	CHECK(server.cfg.binds[0].act == ACT_VOLUME_DOWN);
	config_free(&server.cfg);
	CHECK(config_load(&server.cfg, NULL));
	server.cfg.repeat_delay = 10;
	server.cfg.repeat_rate = 20;
	server.loop = wl_event_loop_create();
	if (!server.loop)
		abort();

	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_rule_names names = { .layout = "us,ru" };
	struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!km)
		abort();
	static const struct wlr_keyboard_impl impl = { .name = "test" };
	struct wlr_keyboard kb;
	wlr_keyboard_init(&kb, &impl, "test");
	CHECK(wlr_keyboard_set_keymap(&kb, km));
	struct keyboard k = { .server = &server, .wlr = &kb };

	/* Linux evdev volume keys; both press and release stay out of clients. */
	key(&k, 115, true);
	expect_audio("wpctl <set-volume> <-l> <1.0> <@DEFAULT_AUDIO_SINK@> <5%+>\n");
	CHECK(k.nconsumed == 1);
	CHECK(wl_event_loop_dispatch(server.loop, 100) == 0);
	expect_audio("wpctl <set-volume> <-l> <1.0> <@DEFAULT_AUDIO_SINK@> <5%+>\n");
	key(&k, 115, false);
	CHECK(k.nconsumed == 0);
	CHECK(wl_event_loop_dispatch(server.loop, 60) == 0);
	no_child();

	/* Russian layout uses the same media key bindings. */
	xkb_state_update_mask(kb.xkb_state, 0, 0, 0, 0, 0, 1);
	key(&k, 114, true);
	expect_audio("wpctl <set-volume> <-l> <1.0> <@DEFAULT_AUDIO_SINK@> <5%->\n");
	key(&k, 114, false);
	key(&k, 113, true);
	expect_audio("wpctl <set-mute> <@DEFAULT_AUDIO_SINK@> <toggle>\n");
	CHECK(wl_event_loop_dispatch(server.loop, 60) == 0);
	no_child(); /* holding mute must not toggle repeatedly */
	key(&k, 113, false);
	CHECK(!handle_binding(&k, 96 + 8)); /* ordinary F12 is not a volume key */

	/* Missing wpctl uses PulseAudio; disabled repeat stays disabled. */
	char path[256];
	snprintf(path, sizeof path, "%s/wpctl", dir);
	CHECK(unlink(path) == 0);
	server.cfg.repeat_rate = 0;
	key(&k, 115, true);
	expect_audio("pactl <set-sink-volume> <@DEFAULT_SINK@> <+5%>\n");
	CHECK(wl_event_loop_dispatch(server.loop, 60) == 0);
	no_child();
	key(&k, 115, false);
	key(&k, 114, true);
	expect_audio("pactl <set-sink-volume> <@DEFAULT_SINK@> <-5%>\n");
	key(&k, 114, false);
	key(&k, 113, true);
	expect_audio("pactl <set-sink-mute> <@DEFAULT_SINK@> <toggle>\n");
	key(&k, 113, false);
	CHECK(server.spawned_next == 0);

	if (k.repeat_timer)
		wl_event_source_remove(k.repeat_timer);
	wlr_keyboard_finish(&kb);
	xkb_keymap_unref(km);
	xkb_context_unref(ctx);
	wl_event_loop_destroy(server.loop);
	config_free(&server.cfg);
	snprintf(path, sizeof path, "%s/pactl", dir);
	unlink(path);
	unlink(config_path);
	unlink(log_path);
	rmdir(dir);
	if (fails)
		return 1;
	puts("all volume tests passed");
	return 0;
}
