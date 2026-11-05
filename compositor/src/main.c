/*
 * bixis: a wlroots compositor with one fullscreen app per "slot".
 * A hotkey shows a slot and launches its app if it isn't running.
 */
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#include "config.h"

#define MODS_MASK (WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO)
#define LAUNCH_TIMEOUT 15 /* seconds a slot waits for its app's first window */
#define MAX_LISTENERS 32

struct slot {
	char name[64];
	struct wlr_scene_tree *tree;
	struct wl_list views;          /* view.slot_link, first = top */
	pid_t pid;
	bool launching;
	time_t launched;
};

struct server {
	struct wl_display *display;
	struct wl_event_loop *loop;
	struct wlr_backend *backend;
	struct wlr_session *session;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;

	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_scene_rect *bg;
	struct wlr_scene_tree *staging;   /* toplevels before their first map */
	struct wlr_output_layout *layout;
	struct wl_list outputs;

	struct wlr_xdg_shell *xdg_shell;
	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *xcursor;
	struct wlr_seat *seat;
	struct wl_list keyboards;
	struct wl_list views;

	struct slot slots[MAX_SLOTS];
	int nslots, active;
	struct { pid_t pid; int slot; } spawned[16]; /* `spawn` launches, a ring */
	int spawned_next;

	struct config cfg;
	const char *config_path;

	struct wl_listener listeners[MAX_LISTENERS]; /* removed on shutdown */
	int nlisteners;
};

struct output {
	struct wl_list link;
	struct server *server;
	struct wlr_output *wlr;
	struct wl_listener frame, request_state, destroy;
};

struct keyboard {
	struct wl_list link;
	struct server *server;
	struct wlr_keyboard *wlr;
	bool is_virtual;
	uint32_t consumed[16];  /* pressed keys taken by bindings */
	int nconsumed;
	struct wl_event_source *repeat_timer;
	uint32_t repeat_key;
	enum action repeat_action;
	struct wl_listener modifiers, key, destroy;
};

struct view {
	struct wl_list link;
	struct wl_list slot_link;
	struct server *server;
	struct wlr_xdg_toplevel *xdg;
	struct wlr_scene_tree *tree;
	struct wlr_xdg_toplevel_decoration_v1 *deco;
	int slot;
	bool mapped;
	bool floating;  /* dialogs and fixed-size windows: natural size, centered */
	struct wl_listener map, unmap, commit, destroy;
	struct wl_listener request_fullscreen, request_maximize;
	struct wl_listener deco_mode, deco_destroy;
};

struct popup {
	struct wlr_xdg_popup *xdg;
	struct wl_listener commit, destroy;
};

/* There is one server; global listeners reach it through server_of(). */
static struct server server;

static struct server *server_of(struct wl_listener *l)
{
	(void)l;
	return &server;
}

static void listen_global(struct server *s, struct wl_signal *sig, wl_notify_func_t fn)
{
	if (s->nlisteners == MAX_LISTENERS) {
		wlr_log(WLR_ERROR, "too many global listeners");
		abort();
	}
	struct wl_listener *l = &s->listeners[s->nlisteners++];
	l->notify = fn;
	wl_signal_add(sig, l);
}

/* ---- helpers ---- */

static struct wlr_box main_box(struct server *s)
{
	struct wlr_box box = { 0 };
	if (!wl_list_empty(&s->outputs)) {
		struct output *o = wl_container_of(s->outputs.next, o, link);
		wlr_output_layout_get_box(s->layout, o->wlr, &box);
	}
	return box;
}

static struct view *view_from_xdg(struct wlr_xdg_toplevel *xdg)
{
	struct wlr_scene_tree *tree = xdg ? xdg->base->data : NULL;
	return tree ? tree->node.data : NULL;
}

static const struct slot_cfg *slot_cfg(struct server *s, int i)
{
	return config_slot(&s->cfg, s->slots[i].name);
}

static int slot_find(struct server *s, const char *name)
{
	for (int i = 0; i < s->nslots; i++)
		if (!strcmp(s->slots[i].name, name))
			return i;
	return -1;
}

static void slot_add(struct server *s, const char *name)
{
	if (s->nslots == MAX_SLOTS || slot_find(s, name) >= 0)
		return;
	struct slot *sl = &s->slots[s->nslots++];
	snprintf(sl->name, sizeof sl->name, "%s", name);
	sl->tree = wlr_scene_tree_create(&s->scene->tree);
	wlr_scene_node_set_enabled(&sl->tree->node, false);
	wl_list_init(&sl->views);
}

/* "A=1 cmd args" -> "A=1 exec cmd args", so the app keeps the shell's pid. */
static char *exec_line(const char *cmd)
{
	const char *p = cmd;
	for (;;) {
		const char *w = p;
		while (*w == ' ' || *w == '\t')
			w++;
		if (!isalpha((unsigned char)*w) && *w != '_')
			break;
		while (isalnum((unsigned char)*w) || *w == '_')
			w++;
		if (*w != '=')
			break;
		while (*w && *w != ' ' && *w != '\t')
			w++;
		p = w;
	}
	char *line;
	if (asprintf(&line, "%.*s exec %s", (int)(p - cmd), cmd, p) < 0)
		return NULL;
	return line;
}

static void child_setup(void)
{
	sigset_t set;
	sigemptyset(&set);
	sigprocmask(SIG_SETMASK, &set, NULL); /* the event loop blocks signals */
	setsid();
}

static pid_t spawn(const char *cmd)
{
	char *line = exec_line(cmd);
	if (!line)
		return -1;

	pid_t pid = fork();
	if (pid == 0) {
		child_setup();
		execl("/bin/sh", "sh", "-c", line, (char *)NULL);
		_exit(127);
	}
	free(line);
	if (pid < 0)
		wlr_log_errno(WLR_ERROR, "fork");
	return pid;
}

/* Audio helpers run outside the event loop and never claim an application slot. */
static void volume_control(enum action act)
{
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log_errno(WLR_ERROR, "fork audio helper");
		return;
	}
	if (pid != 0)
		return;
	child_setup();
	if (act == ACT_VOLUME_MUTE)
		execlp("wpctl", "wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle", (char *)NULL);
	else
		execlp("wpctl", "wpctl", "set-volume", "-l", "1.0", "@DEFAULT_AUDIO_SINK@",
		       act == ACT_VOLUME_UP ? "5%+" : "5%-", (char *)NULL);
	if (errno == ENOENT) {
		if (act == ACT_VOLUME_MUTE)
			execlp("pactl", "pactl", "set-sink-mute", "@DEFAULT_SINK@", "toggle", (char *)NULL);
		else
			execlp("pactl", "pactl", "set-sink-volume", "@DEFAULT_SINK@",
			       act == ACT_VOLUME_UP ? "+5%" : "-5%", (char *)NULL);
	}
	perror("bixis: audio helper (install wpctl or pactl)");
	_exit(127);
}

/* ---- layout ---- */

static void center_view(struct view *v)
{
	struct wlr_box box = main_box(v->server);
	struct wlr_box geo = v->xdg->base->geometry;
	wlr_scene_node_set_position(&v->tree->node,
		(box.width - geo.width) / 2 - geo.x, (box.height - geo.height) / 2 - geo.y);
}

static void size_view(struct view *v)
{
	if (!v->xdg->base->initialized)
		return;
	if (v->floating) {
		center_view(v);
		return;
	}
	struct wlr_box box = main_box(v->server);
	if (box.width > 0)
		wlr_xdg_toplevel_set_size(v->xdg, box.width, box.height);
	wlr_scene_node_set_position(&v->tree->node, 0, 0);
}

static void arrange(struct server *s)
{
	struct wlr_box box = main_box(s), all;
	wlr_output_layout_get_box(s->layout, NULL, &all);
	wlr_scene_node_set_position(&s->bg->node, all.x, all.y);
	wlr_scene_rect_set_size(s->bg, all.width, all.height);
	for (int i = 0; i < s->nslots; i++)
		wlr_scene_node_set_position(&s->slots[i].tree->node, box.x, box.y);

	struct view *v;
	wl_list_for_each(v, &s->views, link)
		size_view(v);
}

/* ---- focus and slots ---- */

static struct view *view_at(struct server *s, double lx, double ly,
                            struct wlr_surface **surface, double *sx, double *sy)
{
	struct wlr_scene_node *node = wlr_scene_node_at(&s->scene->tree.node, lx, ly, sx, sy);
	if (!node || node->type != WLR_SCENE_NODE_BUFFER)
		return NULL;
	struct wlr_scene_surface *ss =
		wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
	if (!ss)
		return NULL;
	*surface = ss->surface;
	struct wlr_scene_tree *tree = node->parent;
	while (tree && !tree->node.data)
		tree = tree->node.parent;
	return tree ? tree->node.data : NULL;
}

static void pointer_refocus(struct server *s, uint32_t time)
{
	double sx, sy;
	struct wlr_surface *surface = NULL;
	view_at(s, s->cursor->x, s->cursor->y, &surface, &sx, &sy);
	if (!surface) {
		wlr_cursor_set_xcursor(s->cursor, s->xcursor, "default");
		wlr_seat_pointer_clear_focus(s->seat);
		return;
	}
	wlr_seat_pointer_notify_enter(s->seat, surface, sx, sy);
	wlr_seat_pointer_notify_motion(s->seat, time, sx, sy);
}

static void focus_view(struct view *v)
{
	struct server *s = v->server;
	struct wlr_surface *surface = v->xdg->base->surface;
	struct wlr_surface *prev = s->seat->keyboard_state.focused_surface;

	wl_list_remove(&v->slot_link);
	wl_list_insert(&s->slots[v->slot].views, &v->slot_link);
	wlr_scene_node_raise_to_top(&v->tree->node);
	if (prev == surface)
		return;

	struct wlr_xdg_toplevel *pt = prev ? wlr_xdg_toplevel_try_from_wlr_surface(prev) : NULL;
	if (pt)
		wlr_xdg_toplevel_set_activated(pt, false);
	wlr_xdg_toplevel_set_activated(v->xdg, true);

	struct wlr_keyboard *kb = wlr_seat_get_keyboard(s->seat);
	if (kb)
		wlr_seat_keyboard_notify_enter(s->seat, surface, kb->keycodes, kb->num_keycodes,
		                               &kb->modifiers);
	else
		wlr_seat_keyboard_notify_enter(s->seat, surface, NULL, 0, NULL);
}

static void focus_slot_top(struct server *s)
{
	struct slot *sl = &s->slots[s->active];
	if (!wl_list_empty(&sl->views)) {
		struct view *v = wl_container_of(sl->views.next, v, slot_link);
		focus_view(v);
		return;
	}
	struct wlr_surface *prev = s->seat->keyboard_state.focused_surface;
	struct wlr_xdg_toplevel *pt = prev ? wlr_xdg_toplevel_try_from_wlr_surface(prev) : NULL;
	if (pt)
		wlr_xdg_toplevel_set_activated(pt, false);
	wlr_seat_keyboard_notify_clear_focus(s->seat);
}

static void slot_show(struct server *s, int i)
{
	s->active = i;
	for (int j = 0; j < s->nslots; j++)
		wlr_scene_node_set_enabled(&s->slots[j].tree->node, j == i);
	focus_slot_top(s);
	pointer_refocus(s, 0);
}

static void slot_activate(struct server *s, const char *name)
{
	int i = slot_find(s, name);
	if (i < 0) {
		wlr_log(WLR_ERROR, "no slot named '%s'", name);
		return;
	}
	slot_show(s, i);

	struct slot *sl = &s->slots[i];
	const struct slot_cfg *c = slot_cfg(s, i);
	if (!wl_list_empty(&sl->views) || !c || !c->exec)
		return;
	if (sl->launching && time(NULL) - sl->launched < LAUNCH_TIMEOUT)
		return; /* still starting */

	sl->pid = spawn(c->exec);
	sl->launching = sl->pid > 0;
	sl->launched = time(NULL);
}

static int view_pick_slot(struct view *v)
{
	struct server *s = v->server;

	struct view *parent = view_from_xdg(v->xdg->parent);
	if (parent && parent->mapped)
		return parent->slot;

	/* whoever launched the process owns its windows, even if another slot has its app_id */
	pid_t pid = 0;
	wl_client_get_credentials(wl_resource_get_client(v->xdg->resource), &pid, NULL, NULL);
	for (int i = 0; i < s->nslots; i++)
		if (s->slots[i].pid > 0 && s->slots[i].pid == pid)
			return i;
	for (size_t i = 0; i < sizeof s->spawned / sizeof *s->spawned; i++)
		if (s->spawned[i].pid > 0 && s->spawned[i].pid == pid)
			return s->spawned[i].slot;

	/* a password prompt or similar pops up where the user is looking */
	if (v->floating)
		return s->active;

	const char *app_id = v->xdg->app_id;
	for (int i = 0; i < s->nslots; i++) {
		const struct slot_cfg *c = slot_cfg(s, i);
		if (app_id && c && c->app_id && !strcasecmp(app_id, c->app_id))
			return i;
	}

	/* wrappers (snap, flatpak) fork: the window comes from another pid */
	if (s->slots[s->active].launching)
		return s->active;
	for (int i = 0; i < s->nslots; i++)
		if (s->slots[i].launching && time(NULL) - s->slots[i].launched < LAUNCH_TIMEOUT)
			return i;
	return s->active;
}

/* ---- actions ---- */

static void reload(struct server *s);

static struct view *focused_view(struct server *s)
{
	struct wlr_surface *f = s->seat->keyboard_state.focused_surface;
	return f ? view_from_xdg(wlr_xdg_toplevel_try_from_wlr_surface(f)) : NULL;
}

static void run_action(struct server *s, const struct bind *b)
{
	struct slot *sl = &s->slots[s->active];
	struct view *v;

	switch (b->act) {
	case ACT_SLOT:
		slot_activate(s, b->arg);
		break;
	case ACT_SPAWN: {
		pid_t pid = spawn(b->arg);
		if (pid > 0) {
			int i = s->spawned_next++ % (int)(sizeof s->spawned / sizeof *s->spawned);
			s->spawned[i].pid = pid;
			s->spawned[i].slot = s->active;
		}
		break;
	}
	case ACT_CLOSE:
		if ((v = focused_view(s)))
			wlr_xdg_toplevel_send_close(v->xdg);
		break;
	case ACT_CYCLE:
		if (wl_list_length(&sl->views) > 1) {
			v = wl_container_of(sl->views.prev, v, slot_link);
			focus_view(v);
		}
		break;
	case ACT_RELOAD:
		reload(s);
		break;
	case ACT_EXIT:
		wl_display_terminate(s->display);
		break;
	case ACT_VOLUME_UP: case ACT_VOLUME_DOWN: case ACT_VOLUME_MUTE:
		volume_control(b->act);
		break;
	}
}

/* ---- keyboard ---- */

static void repeat_stop(struct keyboard *k)
{
	if (k->repeat_timer)
		wl_event_source_timer_update(k->repeat_timer, 0);
}

static int volume_repeat(void *data)
{
	struct keyboard *k = data;
	int rate = k->server->cfg.repeat_rate;
	if (rate > 0) {
		volume_control(k->repeat_action);
		wl_event_source_timer_update(k->repeat_timer, rate < 1000 ? 1000 / rate : 1);
	}
	return 0;
}

static void repeat_start(struct keyboard *k, enum action act, uint32_t key)
{
	if ((act != ACT_VOLUME_UP && act != ACT_VOLUME_DOWN) || k->server->cfg.repeat_rate <= 0)
		return;
	if (!k->repeat_timer)
		k->repeat_timer = wl_event_loop_add_timer(k->server->loop, volume_repeat, k);
	if (!k->repeat_timer)
		return;
	k->repeat_key = key;
	k->repeat_action = act;
	int delay = k->server->cfg.repeat_delay;
	wl_event_source_timer_update(k->repeat_timer, delay > 0 ? delay : 1);
}

/* Unshifted Latin symbol of a key, from the first layout that has one. */
static xkb_keysym_t base_sym(struct wlr_keyboard *kb, xkb_keycode_t kc)
{
	xkb_layout_index_t n = xkb_keymap_num_layouts_for_key(kb->keymap, kc);
	for (xkb_layout_index_t l = 0; l < n; l++) {
		const xkb_keysym_t *syms;
		if (xkb_keymap_key_get_syms_by_level(kb->keymap, kc, l, 0, &syms) > 0 &&
		    (syms[0] < 0x100 || syms[0] >= 0xfd00))
			return xkb_keysym_to_lower(syms[0]);
	}
	xkb_keysym_t sym = xkb_keysym_to_lower(xkb_state_key_get_one_sym(kb->xkb_state, kc));
	/* Shift+Tab maps to ISO_Left_Tab, but bindings use Tab */
	return sym == XKB_KEY_ISO_Left_Tab ? XKB_KEY_Tab : sym;
}

static bool handle_binding(struct keyboard *k, xkb_keycode_t kc)
{
	struct server *s = k->server;
	struct wlr_keyboard *kb = k->wlr;
	const xkb_keysym_t *syms;
	int n = xkb_state_key_get_syms(kb->xkb_state, kc, &syms);
	for (int i = 0; i < n; i++)
		if (syms[i] >= XKB_KEY_XF86Switch_VT_1 && syms[i] <= XKB_KEY_XF86Switch_VT_12) {
			if (s->session)
				wlr_session_change_vt(s->session, syms[i] - XKB_KEY_XF86Switch_VT_1 + 1);
			return true;
		}

	uint32_t mods = wlr_keyboard_get_modifiers(kb) & MODS_MASK;
	xkb_keysym_t sym = base_sym(kb, kc);
	for (int i = 0; i < s->cfg.nbinds; i++) {
		const struct bind *b = &s->cfg.binds[i];
		if (b->mods == mods && b->sym == sym) {
			run_action(s, b);
			repeat_start(k, b->act, kc - 8);
			return true;
		}
	}
	return false;
}

static void kb_key(struct wl_listener *l, void *data)
{
	struct keyboard *k = wl_container_of(l, k, key);
	struct server *s = k->server;
	struct wlr_keyboard_key_event *ev = data;

	if (ev->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		repeat_stop(k);
		if (handle_binding(k, ev->keycode + 8)) {
			if (k->nconsumed < 16)
				k->consumed[k->nconsumed++] = ev->keycode;
			return;
		}
	} else {
		if (ev->keycode == k->repeat_key)
			repeat_stop(k);
		for (int i = 0; i < k->nconsumed; i++)
			if (k->consumed[i] == ev->keycode) {
				k->consumed[i] = k->consumed[--k->nconsumed];
				return; /* the client never saw the press */
			}
	}
	wlr_seat_set_keyboard(s->seat, k->wlr);
	wlr_seat_keyboard_notify_key(s->seat, ev->time_msec, ev->keycode, ev->state);
}

static void kb_modifiers(struct wl_listener *l, void *data)
{
	(void)data;
	struct keyboard *k = wl_container_of(l, k, modifiers);
	repeat_stop(k);
	wlr_seat_set_keyboard(k->server->seat, k->wlr);
	wlr_seat_keyboard_notify_modifiers(k->server->seat, &k->wlr->modifiers);
}

static void update_caps(struct server *s)
{
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&s->keyboards))
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(s->seat, caps);
}

static void kb_destroy(struct wl_listener *l, void *data)
{
	(void)data;
	struct keyboard *k = wl_container_of(l, k, destroy);
	if (k->repeat_timer)
		wl_event_source_remove(k->repeat_timer);
	wl_list_remove(&k->modifiers.link);
	wl_list_remove(&k->key.link);
	wl_list_remove(&k->destroy.link);
	wl_list_remove(&k->link);
	update_caps(k->server);
	free(k);
}

static void kb_apply_config(struct server *s, struct keyboard *k)
{
	repeat_stop(k);
	if (k->is_virtual)
		return; /* virtual keyboards bring their own keymap */

	struct xkb_rule_names names = {
		.layout = s->cfg.kb_layout,
		.variant = s->cfg.kb_variant,
		.options = s->cfg.kb_options,
	};
	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!km) {
		wlr_log(WLR_ERROR, "bad keyboard layout settings, using defaults");
		km = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	}
	wlr_keyboard_set_keymap(k->wlr, km);
	xkb_keymap_unref(km);
	xkb_context_unref(ctx);
	wlr_keyboard_set_repeat_info(k->wlr, s->cfg.repeat_rate, s->cfg.repeat_delay);
}

static void add_keyboard(struct server *s, struct wlr_keyboard *wlr, bool is_virtual)
{
	struct keyboard *k = calloc(1, sizeof *k);
	k->server = s;
	k->wlr = wlr;
	k->is_virtual = is_virtual;
	kb_apply_config(s, k);

	k->modifiers.notify = kb_modifiers;
	wl_signal_add(&wlr->events.modifiers, &k->modifiers);
	k->key.notify = kb_key;
	wl_signal_add(&wlr->events.key, &k->key);
	k->destroy.notify = kb_destroy;
	wl_signal_add(&wlr->base.events.destroy, &k->destroy);

	wlr_seat_set_keyboard(s->seat, wlr);
	wl_list_insert(&s->keyboards, &k->link);
	update_caps(s);
}

static void new_input(struct wl_listener *l, void *data)
{
	struct server *s = server_of(l);
	struct wlr_input_device *dev = data;

	if (dev->type == WLR_INPUT_DEVICE_KEYBOARD)
		add_keyboard(s, wlr_keyboard_from_input_device(dev), false);
	else if (dev->type == WLR_INPUT_DEVICE_POINTER)
		wlr_cursor_attach_input_device(s->cursor, dev);
	update_caps(s);
}

static void new_virtual_keyboard(struct wl_listener *l, void *data)
{
	struct wlr_virtual_keyboard_v1 *vk = data;
	add_keyboard(server_of(l), &vk->keyboard, true);
}

/* ---- pointer ---- */

static void cursor_motion(struct wl_listener *l, void *data)
{
	struct server *s = server_of(l);
	struct wlr_pointer_motion_event *ev = data;
	wlr_cursor_move(s->cursor, &ev->pointer->base, ev->delta_x, ev->delta_y);
	pointer_refocus(s, ev->time_msec);
}

static void cursor_motion_absolute(struct wl_listener *l, void *data)
{
	struct server *s = server_of(l);
	struct wlr_pointer_motion_absolute_event *ev = data;
	wlr_cursor_warp_absolute(s->cursor, &ev->pointer->base, ev->x, ev->y);
	pointer_refocus(s, ev->time_msec);
}

static void cursor_button(struct wl_listener *l, void *data)
{
	struct server *s = server_of(l);
	struct wlr_pointer_button_event *ev = data;

	wlr_seat_pointer_notify_button(s->seat, ev->time_msec, ev->button, ev->state);
	if (ev->state != WL_POINTER_BUTTON_STATE_PRESSED)
		return;
	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct view *v = view_at(s, s->cursor->x, s->cursor->y, &surface, &sx, &sy);
	if (v && v->mapped)
		focus_view(v);
}

static void cursor_axis(struct wl_listener *l, void *data)
{
	struct server *s = server_of(l);
	struct wlr_pointer_axis_event *ev = data;
	wlr_seat_pointer_notify_axis(s->seat, ev->time_msec, ev->orientation, ev->delta,
	                             ev->delta_discrete, ev->source, ev->relative_direction);
}

static void cursor_frame(struct wl_listener *l, void *data)
{
	(void)data;
	wlr_seat_pointer_notify_frame(server_of(l)->seat);
}

static void request_cursor(struct wl_listener *l, void *data)
{
	struct server *s = server_of(l);
	struct wlr_seat_pointer_request_set_cursor_event *ev = data;
	if (s->seat->pointer_state.focused_client == ev->seat_client)
		wlr_cursor_set_surface(s->cursor, ev->surface, ev->hotspot_x, ev->hotspot_y);
}

static void request_cursor_shape(struct wl_listener *l, void *data)
{
	struct server *s = server_of(l);
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *ev = data;
	if (s->seat->pointer_state.focused_client == ev->seat_client)
		wlr_cursor_set_xcursor(s->cursor, s->xcursor, wlr_cursor_shape_v1_name(ev->shape));
}

static void request_selection(struct wl_listener *l, void *data)
{
	struct wlr_seat_request_set_selection_event *ev = data;
	wlr_seat_set_selection(server_of(l)->seat, ev->source, ev->serial);
}

static void request_primary_selection(struct wl_listener *l, void *data)
{
	struct wlr_seat_request_set_primary_selection_event *ev = data;
	wlr_seat_set_primary_selection(server_of(l)->seat, ev->source, ev->serial);
}

/* ---- outputs ---- */

static void output_frame(struct wl_listener *l, void *data)
{
	(void)data;
	struct output *o = wl_container_of(l, o, frame);
	struct wlr_scene_output *so = wlr_scene_get_scene_output(o->server->scene, o->wlr);
	struct timespec now;

	wlr_scene_output_commit(so, NULL);
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(so, &now);
}

static void output_request_state(struct wl_listener *l, void *data)
{
	struct output *o = wl_container_of(l, o, request_state);
	const struct wlr_output_event_request_state *ev = data;
	wlr_output_commit_state(o->wlr, ev->state);
	arrange(o->server);
}

static void output_destroy(struct wl_listener *l, void *data)
{
	(void)data;
	struct output *o = wl_container_of(l, o, destroy);
	wl_list_remove(&o->frame.link);
	wl_list_remove(&o->request_state.link);
	wl_list_remove(&o->destroy.link);
	wl_list_remove(&o->link);
	free(o);
}

/* 4K gets 200%; so do dense laptop panels, judged by DPI when the size is known. */
static float output_scale(struct server *s, struct wlr_output *o, int w)
{
	if (s->cfg.scale > 0)
		return s->cfg.scale;
	if (w >= 3840)
		return 2.0f;
	if (o->phys_width > 0 && w / (o->phys_width / 25.4) >= 180)
		return 2.0f;
	return 1.0f;
}

/* The configured mode, else the native resolution at its highest refresh rate
 * (some 4K monitors advertise 30 Hz as preferred). */
static void output_set_mode(struct server *s, struct wlr_output *o, struct wlr_output_state *st)
{
	const struct config *c = &s->cfg;
	struct wlr_output_mode *m, *best = NULL;

	if (c->mode_w) {
		wl_list_for_each(m, &o->modes, link) {
			if (m->width != c->mode_w || m->height != c->mode_h)
				continue;
			if (c->mode_refresh && abs(m->refresh - c->mode_refresh) > 1000)
				continue;
			if (!best || m->refresh > best->refresh)
				best = m;
		}
		if (!best) {
			if (wl_list_empty(&o->modes)) { /* nested or headless: any size goes */
				wlr_output_state_set_custom_mode(st, c->mode_w, c->mode_h, c->mode_refresh);
				return;
			}
			wlr_log(WLR_ERROR, "output %s has no mode %dx%d, using its preferred one",
			        o->name, c->mode_w, c->mode_h);
		}
	}
	if (!best && (best = wlr_output_preferred_mode(o))) {
		struct wlr_output_mode *pref = best;
		wl_list_for_each(m, &o->modes, link)
			if (m->width == pref->width && m->height == pref->height &&
			    m->refresh > best->refresh)
				best = m;
	}
	if (best)
		wlr_output_state_set_mode(st, best);
}

static void output_configure(struct server *s, struct wlr_output *o, bool init)
{
	struct wlr_output_state st;
	wlr_output_state_init(&st);
	if (init)
		wlr_output_state_set_enabled(&st, true);
	output_set_mode(s, o, &st);

	int w = o->width;
	if (st.committed & WLR_OUTPUT_STATE_MODE)
		w = st.mode_type == WLR_OUTPUT_STATE_MODE_FIXED ? st.mode->width : st.custom_mode.width;
	wlr_output_state_set_scale(&st, output_scale(s, o, w));

	if (!wlr_output_commit_state(o, &st))
		wlr_log(WLR_ERROR, "output %s: can't apply mode/scale", o->name);
	wlr_output_state_finish(&st);
	wlr_log(WLR_INFO, "output %s: %dx%d@%.2fHz scale %.2f", o->name, o->width, o->height,
	        o->refresh / 1000.0, o->scale);
}

static void new_output(struct wl_listener *l, void *data)
{
	struct server *s = server_of(l);
	struct wlr_output *wlr = data;

	wlr_output_init_render(wlr, s->allocator, s->renderer);
	output_configure(s, wlr, true);

	struct output *o = calloc(1, sizeof *o);
	o->server = s;
	o->wlr = wlr;
	o->frame.notify = output_frame;
	wl_signal_add(&wlr->events.frame, &o->frame);
	o->request_state.notify = output_request_state;
	wl_signal_add(&wlr->events.request_state, &o->request_state);
	o->destroy.notify = output_destroy;
	wl_signal_add(&wlr->events.destroy, &o->destroy);
	wl_list_insert(s->outputs.prev, &o->link);

	struct wlr_output_layout_output *lo = wlr_output_layout_add_auto(s->layout, wlr);
	struct wlr_scene_output *so = wlr_scene_output_create(s->scene, wlr);
	wlr_scene_output_layout_add_output(s->scene_layout, lo, so);
	arrange(s);
}

static void layout_change(struct wl_listener *l, void *data)
{
	(void)data;
	arrange(server_of(l));
}

/* ---- xdg toplevels ---- */

static bool wants_float(struct wlr_xdg_toplevel *t);
static void view_set_floating(struct view *v);

static void view_map(struct wl_listener *l, void *data)
{
	(void)data;
	struct view *v = wl_container_of(l, v, map);
	struct server *s = v->server;

	v->mapped = true;
	if (!v->floating && wants_float(v->xdg))
		view_set_floating(v);
	v->slot = view_pick_slot(v);
	struct slot *sl = &s->slots[v->slot];
	wlr_log(WLR_INFO, "window app_id=%s title=%s -> slot %s%s",
	        v->xdg->app_id ? v->xdg->app_id : "(none)",
	        v->xdg->title ? v->xdg->title : "(none)", sl->name,
	        v->floating ? " (floating)" : "");
	sl->launching = false;
	wlr_scene_node_reparent(&v->tree->node, sl->tree);
	wl_list_insert(&sl->views, &v->slot_link);
	size_view(v);
	if (v->slot == s->active)
		focus_view(v);
}

static void view_unmap(struct wl_listener *l, void *data)
{
	(void)data;
	struct view *v = wl_container_of(l, v, unmap);
	struct server *s = v->server;
	bool had_focus = s->seat->keyboard_state.focused_surface == v->xdg->base->surface;

	v->mapped = false;
	wl_list_remove(&v->slot_link);
	wl_list_init(&v->slot_link);
	if (had_focus && v->slot == s->active)
		focus_slot_top(s);
}

/* Windows that can't be resized (min == max size) are dialogs: don't stretch them. */
static bool wants_float(struct wlr_xdg_toplevel *t)
{
	const struct wlr_xdg_toplevel_state *st = &t->current;
	return t->parent || (st->min_width > 0 && st->min_height > 0 &&
	                     st->min_width == st->max_width && st->min_height == st->max_height);
}

/* GTK announces min/max size after the first commit: un-maximize late dialogs. */
static void view_set_floating(struct view *v)
{
	v->floating = true;
	wlr_xdg_toplevel_set_maximized(v->xdg, false);
	if (wl_resource_get_version(v->xdg->resource) >= XDG_TOPLEVEL_STATE_TILED_LEFT_SINCE_VERSION)
		wlr_xdg_toplevel_set_tiled(v->xdg, 0);
	wlr_xdg_toplevel_set_size(v->xdg, 0, 0);
}

static void view_commit(struct wl_listener *l, void *data)
{
	(void)data;
	struct view *v = wl_container_of(l, v, commit);

	if (v->xdg->base->initial_commit) {
		v->floating = wants_float(v->xdg);
		if (v->floating) {
			wlr_xdg_toplevel_set_size(v->xdg, 0, 0);
		} else {
			struct wlr_box box = main_box(v->server);
			wlr_xdg_toplevel_set_size(v->xdg, box.width, box.height);
			wlr_xdg_toplevel_set_maximized(v->xdg, true);
			if (wl_resource_get_version(v->xdg->resource) >= XDG_TOPLEVEL_STATE_TILED_LEFT_SINCE_VERSION)
				wlr_xdg_toplevel_set_tiled(v->xdg, WLR_EDGE_TOP | WLR_EDGE_BOTTOM |
				                                   WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
		}
		if (v->deco)
			wlr_xdg_toplevel_decoration_v1_set_mode(v->deco,
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
		return;
	}
	if (!v->floating && wants_float(v->xdg))
		view_set_floating(v);
	if (v->mapped && v->floating)
		center_view(v);
}

static void view_request_fullscreen(struct wl_listener *l, void *data)
{
	(void)data;
	struct view *v = wl_container_of(l, v, request_fullscreen);
	if (v->xdg->base->initialized)
		wlr_xdg_toplevel_set_fullscreen(v->xdg, v->xdg->requested.fullscreen);
}

static void view_request_maximize(struct wl_listener *l, void *data)
{
	(void)data;
	struct view *v = wl_container_of(l, v, request_maximize);
	if (v->xdg->base->initialized)
		wlr_xdg_surface_schedule_configure(v->xdg->base); /* stays as it is */
}

static void view_deco_unlink(struct view *v)
{
	if (!v->deco)
		return;
	wl_list_remove(&v->deco_mode.link);
	wl_list_remove(&v->deco_destroy.link);
	v->deco = NULL;
}

static void view_destroy(struct wl_listener *l, void *data)
{
	(void)data;
	struct view *v = wl_container_of(l, v, destroy);

	view_deco_unlink(v);
	wl_list_remove(&v->map.link);
	wl_list_remove(&v->unmap.link);
	wl_list_remove(&v->commit.link);
	wl_list_remove(&v->destroy.link);
	wl_list_remove(&v->request_fullscreen.link);
	wl_list_remove(&v->request_maximize.link);
	wl_list_remove(&v->slot_link);
	wl_list_remove(&v->link);
	free(v);
}

static void new_toplevel(struct wl_listener *l, void *data)
{
	struct server *s = server_of(l);
	struct wlr_xdg_toplevel *xdg = data;
	struct view *v = calloc(1, sizeof *v);

	v->server = s;
	v->xdg = xdg;
	v->slot = s->active;
	v->tree = wlr_scene_xdg_surface_create(s->staging, xdg->base);
	v->tree->node.data = v;
	xdg->base->data = v->tree;
	wl_list_init(&v->slot_link);

	v->map.notify = view_map;
	wl_signal_add(&xdg->base->surface->events.map, &v->map);
	v->unmap.notify = view_unmap;
	wl_signal_add(&xdg->base->surface->events.unmap, &v->unmap);
	v->commit.notify = view_commit;
	wl_signal_add(&xdg->base->surface->events.commit, &v->commit);
	v->destroy.notify = view_destroy;
	wl_signal_add(&xdg->events.destroy, &v->destroy);
	v->request_fullscreen.notify = view_request_fullscreen;
	wl_signal_add(&xdg->events.request_fullscreen, &v->request_fullscreen);
	v->request_maximize.notify = view_request_maximize;
	wl_signal_add(&xdg->events.request_maximize, &v->request_maximize);
	wl_list_insert(&s->views, &v->link);
}

/* We never draw decorations; telling clients "server side" makes them skip theirs. */
static void deco_set(struct view *v)
{
	if (v->deco && v->xdg->base->initialized)
		wlr_xdg_toplevel_decoration_v1_set_mode(v->deco,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void deco_mode(struct wl_listener *l, void *data)
{
	(void)data;
	struct view *v = wl_container_of(l, v, deco_mode);
	deco_set(v);
}

static void deco_destroy(struct wl_listener *l, void *data)
{
	(void)data;
	struct view *v = wl_container_of(l, v, deco_destroy);
	view_deco_unlink(v);
}

static void new_decoration(struct wl_listener *l, void *data)
{
	(void)l;
	struct wlr_xdg_toplevel_decoration_v1 *d = data;
	struct view *v = view_from_xdg(d->toplevel);
	if (!v || v->deco)
		return;
	v->deco = d;
	v->deco_mode.notify = deco_mode;
	wl_signal_add(&d->events.request_mode, &v->deco_mode);
	v->deco_destroy.notify = deco_destroy;
	wl_signal_add(&d->events.destroy, &v->deco_destroy);
	deco_set(v);
}

/* ---- xdg popups ---- */

static void popup_commit(struct wl_listener *l, void *data)
{
	(void)data;
	struct popup *p = wl_container_of(l, p, commit);
	if (!p->xdg->base->initial_commit)
		return;

	/* keep menus on screen: constraint box in the root toplevel's coordinates */
	struct wlr_scene_tree *tree = p->xdg->base->data;
	while (tree && !tree->node.data)
		tree = tree->node.parent;
	struct view *v = tree ? tree->node.data : NULL;
	if (v) {
		int lx = 0, ly = 0;
		wlr_scene_node_coords(&v->tree->node, &lx, &ly);
		struct wlr_box box = main_box(v->server);
		box.x -= lx;
		box.y -= ly;
		wlr_xdg_popup_unconstrain_from_box(p->xdg, &box);
	}
	wlr_xdg_surface_schedule_configure(p->xdg->base);
}

static void popup_destroy(struct wl_listener *l, void *data)
{
	(void)data;
	struct popup *p = wl_container_of(l, p, destroy);
	wl_list_remove(&p->commit.link);
	wl_list_remove(&p->destroy.link);
	free(p);
}

static void new_popup(struct wl_listener *l, void *data)
{
	(void)l;
	struct wlr_xdg_popup *xdg = data;
	struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg->parent);
	if (!parent || !parent->data)
		return;

	struct popup *p = calloc(1, sizeof *p);
	p->xdg = xdg;
	xdg->base->data = wlr_scene_xdg_surface_create(parent->data, xdg->base);
	p->commit.notify = popup_commit;
	wl_signal_add(&xdg->base->surface->events.commit, &p->commit);
	p->destroy.notify = popup_destroy;
	wl_signal_add(&xdg->events.destroy, &p->destroy);
}

/* ---- config reload, signals ---- */

static void reload(struct server *s)
{
	struct config c;
	if (!s->config_path || !config_load(&c, s->config_path)) {
		wlr_log(WLR_ERROR, "reload: keeping the current config");
		return;
	}
	config_free(&s->cfg);
	s->cfg = c;

	for (int i = 0; i < s->cfg.nslots; i++)
		slot_add(s, s->cfg.slots[i].name); /* slots are never removed: windows stay */

	struct keyboard *k;
	wl_list_for_each(k, &s->keyboards, link)
		kb_apply_config(s, k);

	wlr_scene_rect_set_color(s->bg, s->cfg.bg);
	struct output *o;
	wl_list_for_each(o, &s->outputs, link)
		output_configure(s, o->wlr, false);
	arrange(s);
	wlr_log(WLR_INFO, "config reloaded");
}

static int on_sigchld(int sig, void *data)
{
	(void)sig;
	struct server *s = data;
	pid_t pid;
	while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
		for (int i = 0; i < s->nslots; i++)
			if (s->slots[i].pid == pid)
				s->slots[i].pid = 0;
		for (size_t i = 0; i < sizeof s->spawned / sizeof *s->spawned; i++)
			if (s->spawned[i].pid == pid)
				s->spawned[i].pid = 0;
	}
	return 0;
}

static int on_sighup(int sig, void *data)
{
	(void)sig;
	reload(data);
	return 0;
}

static int on_sigterm(int sig, void *data)
{
	(void)sig;
	wl_display_terminate(((struct server *)data)->display);
	return 0;
}

static char *default_config_path(void)
{
	const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	char *path = NULL;
	if (xdg && *xdg) {
		if (asprintf(&path, "%s/bixis/config", xdg) < 0)
			return NULL;
	} else if (home) {
		if (asprintf(&path, "%s/.config/bixis/config", home) < 0)
			return NULL;
	}
	if (path && access(path, R_OK) != 0) {
		free(path);
		path = NULL;
	}
	return path;
}

static void usage(void)
{
	puts("usage: bixis [-c config] [-d] [-v]\n"
	     "  -c FILE  config file (default: ~/.config/bixis/config, else built-in)\n"
	     "  -d       debug log\n"
	     "  -v       version");
}

int main(int argc, char **argv)
{
	struct server *s = &server;
	enum wlr_log_importance log = WLR_ERROR;
	char *config_path = NULL;
	int opt;

	while ((opt = getopt(argc, argv, "c:dvh")) != -1) {
		switch (opt) {
		case 'c': config_path = strdup(optarg); break;
		case 'd': log = WLR_DEBUG; break;
		case 'v': puts("bixis " VERSION); return 0;
		default: usage(); return opt == 'h' ? 0 : 1;
		}
	}
	wlr_log_init(log, NULL);

	if (!config_path)
		config_path = default_config_path();
	if (!config_load(&s->cfg, config_path))
		return 1;
	s->config_path = config_path;
	fprintf(stderr, "bixis: config %s\n", config_path ? config_path : "<built-in>");

	s->display = wl_display_create();
	s->loop = wl_display_get_event_loop(s->display);
	s->backend = wlr_backend_autocreate(s->loop, &s->session);
	if (!s->backend) {
		fputs("bixis: can't create a backend (run from a TTY or inside Wayland)\n", stderr);
		return 1;
	}
	s->renderer = wlr_renderer_autocreate(s->backend);
	if (!s->renderer) {
		fputs("bixis: can't create a renderer\n", stderr);
		return 1;
	}
	wlr_renderer_init_wl_display(s->renderer, s->display);
	s->allocator = wlr_allocator_autocreate(s->backend, s->renderer);

	wlr_compositor_create(s->display, 6, s->renderer);
	wlr_subcompositor_create(s->display);
	wlr_data_device_manager_create(s->display);
	wlr_primary_selection_v1_device_manager_create(s->display);
	wlr_viewporter_create(s->display);
	wlr_presentation_create(s->display, s->backend, 2);
	wlr_single_pixel_buffer_manager_v1_create(s->display);
	wlr_fractional_scale_manager_v1_create(s->display, 1);
	wlr_screencopy_manager_v1_create(s->display);

	s->layout = wlr_output_layout_create(s->display);
	wlr_xdg_output_manager_v1_create(s->display, s->layout);
	wl_list_init(&s->outputs);
	listen_global(s, &s->layout->events.change, layout_change);
	listen_global(s, &s->backend->events.new_output, new_output);

	s->scene = wlr_scene_create();
	s->scene_layout = wlr_scene_attach_output_layout(s->scene, s->layout);
	s->bg = wlr_scene_rect_create(&s->scene->tree, 0, 0, s->cfg.bg);
	s->staging = wlr_scene_tree_create(&s->scene->tree);
	wlr_scene_node_set_enabled(&s->staging->node, false);
	for (int i = 0; i < s->cfg.nslots; i++)
		slot_add(s, s->cfg.slots[i].name);
	if (!s->nslots)
		slot_add(s, "main");
	wl_list_init(&s->views);

	s->xdg_shell = wlr_xdg_shell_create(s->display, 6);
	listen_global(s, &s->xdg_shell->events.new_toplevel, new_toplevel);
	listen_global(s, &s->xdg_shell->events.new_popup, new_popup);
	struct wlr_xdg_decoration_manager_v1 *deco = wlr_xdg_decoration_manager_v1_create(s->display);
	listen_global(s, &deco->events.new_toplevel_decoration, new_decoration);

	s->cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(s->cursor, s->layout);
	const char *size = getenv("XCURSOR_SIZE");
	s->xcursor = wlr_xcursor_manager_create(getenv("XCURSOR_THEME"), size ? atoi(size) : 24);
	listen_global(s, &s->cursor->events.motion, cursor_motion);
	listen_global(s, &s->cursor->events.motion_absolute, cursor_motion_absolute);
	listen_global(s, &s->cursor->events.button, cursor_button);
	listen_global(s, &s->cursor->events.axis, cursor_axis);
	listen_global(s, &s->cursor->events.frame, cursor_frame);

	wl_list_init(&s->keyboards);
	listen_global(s, &s->backend->events.new_input, new_input);
	s->seat = wlr_seat_create(s->display, "seat0");
	listen_global(s, &s->seat->events.request_set_cursor, request_cursor);
	listen_global(s, &s->seat->events.request_set_selection, request_selection);
	listen_global(s, &s->seat->events.request_set_primary_selection, request_primary_selection);
	struct wlr_cursor_shape_manager_v1 *shape = wlr_cursor_shape_manager_v1_create(s->display, 1);
	listen_global(s, &shape->events.request_set_shape, request_cursor_shape);
	struct wlr_virtual_keyboard_manager_v1 *vk = wlr_virtual_keyboard_manager_v1_create(s->display);
	listen_global(s, &vk->events.new_virtual_keyboard, new_virtual_keyboard);

	wl_event_loop_add_signal(s->loop, SIGCHLD, on_sigchld, s);
	wl_event_loop_add_signal(s->loop, SIGHUP, on_sighup, s);
	wl_event_loop_add_signal(s->loop, SIGINT, on_sigterm, s);
	wl_event_loop_add_signal(s->loop, SIGTERM, on_sigterm, s);

	const char *socket = wl_display_add_socket_auto(s->display);
	if (!socket || !wlr_backend_start(s->backend)) {
		fputs("bixis: can't start\n", stderr);
		return 1;
	}
	setenv("WAYLAND_DISPLAY", socket, true);
	unsetenv("DISPLAY");
	setenv("XDG_CURRENT_DESKTOP", "bixis", true);
	setenv("XDG_SESSION_TYPE", "wayland", true);
	setenv("MOZ_ENABLE_WAYLAND", "1", true);
	setenv("QT_QPA_PLATFORM", "wayland", true);
	setenv("ELECTRON_OZONE_PLATFORM_HINT", "wayland", true);
	setenv("GDK_BACKEND", "wayland", true);
	fprintf(stderr, "bixis: running on WAYLAND_DISPLAY=%s\n", socket);

	for (int i = 0; i < s->cfg.nautostart; i++)
		spawn(s->cfg.autostart[i]);

	wlr_cursor_set_xcursor(s->cursor, s->xcursor, "default");
	int start = s->cfg.start ? slot_find(s, s->cfg.start) : -1;
	if (start >= 0)
		slot_activate(s, s->cfg.start);
	else
		slot_show(s, 0);

	wl_display_run(s->display);

	wl_display_destroy_clients(s->display);
	for (int i = 0; i < s->nlisteners; i++)
		wl_list_remove(&s->listeners[i].link);
	wlr_scene_node_destroy(&s->scene->tree.node);
	wlr_xcursor_manager_destroy(s->xcursor);
	wlr_cursor_destroy(s->cursor);
	wlr_allocator_destroy(s->allocator);
	wlr_renderer_destroy(s->renderer);
	wlr_backend_destroy(s->backend);
	wl_display_destroy(s->display);
	config_free(&s->cfg);
	free(config_path);
	return 0;
}
