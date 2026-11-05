/* INI-style config: [keyboard], [output], [slot NAME], [bindings]. */
#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <wlr/types/wlr_keyboard.h>

#include "default_config.h"

#define MOD_PLACEHOLDER (1u << 31)

enum section { SEC_GENERAL, SEC_KEYBOARD, SEC_OUTPUT, SEC_SLOT, SEC_BINDINGS, SEC_UNKNOWN };

static char *trim(char *s)
{
	while (isspace((unsigned char)*s))
		s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = 0;
	return s;
}

static void set_str(char **dst, const char *val)
{
	free(*dst);
	*dst = *val ? strdup(val) : NULL;
}

static bool parse_mod(const char *s, uint32_t *out)
{
	if (!strcasecmp(s, "super") || !strcasecmp(s, "logo") || !strcasecmp(s, "win"))
		*out = WLR_MODIFIER_LOGO;
	else if (!strcasecmp(s, "alt"))
		*out = WLR_MODIFIER_ALT;
	else if (!strcasecmp(s, "ctrl") || !strcasecmp(s, "control"))
		*out = WLR_MODIFIER_CTRL;
	else if (!strcasecmp(s, "shift"))
		*out = WLR_MODIFIER_SHIFT;
	else if (!strcasecmp(s, "mod"))
		*out = MOD_PLACEHOLDER;
	else
		return false;
	return true;
}

static bool parse_keys(char *spec, uint32_t *mods, xkb_keysym_t *sym)
{
	char *save, *tok = strtok_r(spec, "+", &save);

	*mods = 0;
	while (tok) {
		char *next = strtok_r(NULL, "+", &save);
		tok = trim(tok);
		if (!next) {
			*sym = xkb_keysym_to_lower(xkb_keysym_from_name(tok, XKB_KEYSYM_CASE_INSENSITIVE));
			return *sym != XKB_KEY_NoSymbol;
		}
		uint32_t m;
		if (!parse_mod(tok, &m))
			return false;
		*mods |= m;
		tok = next;
	}
	return false;
}

static bool parse_action(char *val, struct bind *b)
{
	static const struct { const char *name; enum action act; bool arg; } acts[] = {
		{ "slot", ACT_SLOT, true },   { "spawn", ACT_SPAWN, true },
		{ "close", ACT_CLOSE, false }, { "cycle", ACT_CYCLE, false },
		{ "reload", ACT_RELOAD, false }, { "exit", ACT_EXIT, false },
	};
	char *arg = val;
	while (*arg && !isspace((unsigned char)*arg))
		arg++;
	if (*arg)
		*arg++ = 0;
	arg = trim(arg);

	for (size_t i = 0; i < sizeof acts / sizeof *acts; i++) {
		if (strcasecmp(val, acts[i].name))
			continue;
		if (acts[i].arg != !!*arg)
			return false;
		b->act = acts[i].act;
		b->arg = *arg ? strdup(arg) : NULL;
		return true;
	}
	return false;
}

static bool parse_color(const char *s, float out[4])
{
	unsigned v;
	if (*s == '#')
		s++;
	if (strlen(s) != 6 || sscanf(s, "%6x", &v) != 1)
		return false;
	out[0] = (v >> 16 & 255) / 255.0f;
	out[1] = (v >> 8 & 255) / 255.0f;
	out[2] = (v & 255) / 255.0f;
	out[3] = 1.0f;
	return true;
}

static void parse(struct config *c, char *text, const char *src)
{
	enum section sec = SEC_GENERAL;
	struct slot_cfg *slot = NULL;
	int lineno = 0;

#define WARN(...) do { \
		fprintf(stderr, "bixis: %s:%d: ", src, lineno); \
		fprintf(stderr, __VA_ARGS__); \
		fputc('\n', stderr); \
	} while (0)

	for (char *p = text, *nl; p; p = nl) {
		nl = strchr(p, '\n');
		if (nl)
			*nl++ = 0;
		lineno++;
		char *line = trim(p);
		if (!*line || *line == '#')
			continue;

		if (*line == '[') {
			char *end = strchr(line, ']');
			if (!end) {
				WARN("missing ']'");
				continue;
			}
			*end = 0;
			char *name = trim(line + 1);
			if (!strcasecmp(name, "keyboard")) {
				sec = SEC_KEYBOARD;
			} else if (!strcasecmp(name, "output")) {
				sec = SEC_OUTPUT;
			} else if (!strcasecmp(name, "bindings")) {
				sec = SEC_BINDINGS;
			} else if (!strncasecmp(name, "slot", 4) && isspace((unsigned char)name[4])) {
				name = trim(name + 5);
				sec = SEC_UNKNOWN;
				if (config_slot(c, name)) {
					WARN("duplicate slot '%s'", name);
				} else if (c->nslots == MAX_SLOTS) {
					WARN("too many slots (max %d)", MAX_SLOTS);
				} else {
					slot = &c->slots[c->nslots++];
					slot->name = strdup(name);
					sec = SEC_SLOT;
				}
			} else {
				WARN("unknown section [%s]", name);
				sec = SEC_UNKNOWN;
			}
			continue;
		}

		char *eq = strchr(line, '=');
		if (!eq) {
			WARN("expected key = value");
			continue;
		}
		*eq = 0;
		char *key = trim(line), *val = trim(eq + 1);

		switch (sec) {
		case SEC_GENERAL:
			if (!strcasecmp(key, "mod")) {
				uint32_t m;
				if (!parse_mod(val, &m) || m == MOD_PLACEHOLDER)
					WARN("bad modifier '%s'", val);
				else
					c->mod = m;
			} else if (!strcasecmp(key, "start")) {
				set_str(&c->start, val);
			} else if (!strcasecmp(key, "autostart")) {
				if (*val) {
					c->autostart = realloc(c->autostart,
					                       (c->nautostart + 1) * sizeof *c->autostart);
					c->autostart[c->nautostart++] = strdup(val);
				}
			} else {
				WARN("unknown key '%s'", key);
			}
			break;
		case SEC_KEYBOARD:
			if (!strcasecmp(key, "layout"))
				set_str(&c->kb_layout, val);
			else if (!strcasecmp(key, "variant"))
				set_str(&c->kb_variant, val);
			else if (!strcasecmp(key, "options"))
				set_str(&c->kb_options, val);
			else if (!strcasecmp(key, "repeat_rate"))
				c->repeat_rate = atoi(val);
			else if (!strcasecmp(key, "repeat_delay"))
				c->repeat_delay = atoi(val);
			else
				WARN("unknown key '%s'", key);
			break;
		case SEC_OUTPUT:
			if (!strcasecmp(key, "scale")) {
				float s = strtof(val, NULL);
				if (!strcasecmp(val, "auto"))
					c->scale = 0;
				else if (s < 0.5f || s > 4.0f)
					WARN("scale must be auto or 0.5..4");
				else
					c->scale = s;
			} else if (!strcasecmp(key, "mode")) {
				int w = 0, h = 0;
				float hz = 0;
				if (!*val || !strcasecmp(val, "auto")) {
					c->mode_w = c->mode_h = c->mode_refresh = 0;
				} else if (sscanf(val, "%dx%d@%f", &w, &h, &hz) < 2 || w <= 0 || h <= 0) {
					WARN("bad mode '%s', expected WIDTHxHEIGHT[@HZ]", val);
				} else {
					c->mode_w = w;
					c->mode_h = h;
					c->mode_refresh = (int)(hz * 1000 + 0.5f);
				}
			} else if (!strcasecmp(key, "background")) {
				if (!parse_color(val, c->bg))
					WARN("bad color '%s', expected #rrggbb", val);
			} else {
				WARN("unknown key '%s'", key);
			}
			break;
		case SEC_SLOT:
			if (!strcasecmp(key, "exec"))
				set_str(&slot->exec, val);
			else if (!strcasecmp(key, "app_id"))
				set_str(&slot->app_id, val);
			else
				WARN("unknown key '%s'", key);
			break;
		case SEC_BINDINGS: {
			struct bind b = { 0 };
			if (!parse_keys(key, &b.mods, &b.sym)) {
				WARN("bad key combination");
				break;
			}
			if (!parse_action(val, &b)) {
				WARN("bad action '%s'", val);
				break;
			}
			c->binds = realloc(c->binds, (c->nbinds + 1) * sizeof *c->binds);
			c->binds[c->nbinds++] = b;
			break;
		}
		case SEC_UNKNOWN:
			break;
		}
	}
#undef WARN

	for (int i = 0; i < c->nbinds; i++)
		if (c->binds[i].mods & MOD_PLACEHOLDER)
			c->binds[i].mods = (c->binds[i].mods & ~MOD_PLACEHOLDER) | c->mod;
}

static char *read_file(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	size_t cap = 4096, len = 0;
	char *buf = malloc(cap);
	size_t n;
	while ((n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
		len += n;
		if (cap - len < 2)
			buf = realloc(buf, cap *= 2);
	}
	fclose(f);
	buf[len] = 0;
	return buf;
}

bool config_load(struct config *c, const char *path)
{
	memset(c, 0, sizeof *c);
	c->mod = WLR_MODIFIER_LOGO;
	c->repeat_rate = 25;
	c->repeat_delay = 600;
	c->scale = 0;
	parse_color("#161616", c->bg);

	char *text = path ? read_file(path) : strdup(default_config);
	if (!text) {
		fprintf(stderr, "bixis: can't read %s: %s\n", path, strerror(errno));
		return false;
	}
	parse(c, text, path ? path : "<built-in>");
	free(text);
	return true;
}

void config_free(struct config *c)
{
	free(c->start);
	for (int i = 0; i < c->nautostart; i++)
		free(c->autostart[i]);
	free(c->autostart);
	free(c->kb_layout);
	free(c->kb_variant);
	free(c->kb_options);
	for (int i = 0; i < c->nslots; i++) {
		free(c->slots[i].name);
		free(c->slots[i].exec);
		free(c->slots[i].app_id);
	}
	for (int i = 0; i < c->nbinds; i++)
		free(c->binds[i].arg);
	free(c->binds);
	memset(c, 0, sizeof *c);
}

const struct slot_cfg *config_slot(const struct config *c, const char *name)
{
	for (int i = 0; i < c->nslots; i++)
		if (!strcmp(c->slots[i].name, name))
			return &c->slots[i];
	return NULL;
}
