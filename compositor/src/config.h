#ifndef BIXIS_CONFIG_H
#define BIXIS_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <xkbcommon/xkbcommon.h>

#define MAX_SLOTS 16

enum action {
	ACT_SLOT,    /* arg: slot name */
	ACT_SPAWN,   /* arg: shell command */
	ACT_CLOSE,
	ACT_CYCLE,
	ACT_RELOAD,
	ACT_EXIT,
};

struct slot_cfg {
	char *name;
	char *exec;
	char *app_id;
};

struct bind {
	uint32_t mods;       /* WLR_MODIFIER_* */
	xkb_keysym_t sym;    /* lowercase */
	enum action act;
	char *arg;
};

struct config {
	uint32_t mod;
	char *start;         /* slot shown at startup */

	char *kb_layout, *kb_variant, *kb_options;
	int repeat_rate, repeat_delay;

	float scale;         /* 0 = auto: 200% on 4K and dense screens */
	int mode_w, mode_h;  /* 0 = monitor's preferred */
	int mode_refresh;    /* mHz, 0 = highest available */
	float bg[4];

	struct slot_cfg slots[MAX_SLOTS];
	int nslots;
	struct bind *binds;
	int nbinds;
};

/* path may be NULL: built-in defaults. Errors are reported, bad lines skipped. */
bool config_load(struct config *c, const char *path);
void config_free(struct config *c);
const struct slot_cfg *config_slot(const struct config *c, const char *name);

#endif
