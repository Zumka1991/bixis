/* bixis-term configuration. Edit, then rebuild: make && sudo make install */

/* Font: any fontconfig pattern, e.g. "JetBrains Mono" or "monospace" */
#define FONT          "monospace"
#define FONT_SIZE     11.0   /* points */

#define PADDING       6      /* pixels around the text grid */
#define SCROLLBACK    5000   /* lines per tab */
#define WHEEL_LINES   3      /* lines per mouse wheel notch */
#define DEFAULT_COLS  100    /* initial size when the compositor doesn't choose */
#define DEFAULT_ROWS  30
#define MAX_TABS      32

/* Colors: 0xRRGGBB */
#define COLOR_FG            0xd0d0d0
#define COLOR_BG            0x161616
#define COLOR_CURSOR        0xd0d0d0
#define COLOR_CURSOR_TEXT   0x161616
#define COLOR_SEL_FG        0xf0f0f0
#define COLOR_SEL_BG        0x3a4a6a
#define COLOR_TAB_BG        0x0e0e0e
#define COLOR_TAB_FG        0x7a7a7a
#define COLOR_TAB_ACTIVE_BG 0x2a2a2a
#define COLOR_TAB_ACTIVE_FG 0xeeeeee

static const uint32_t palette[16] = {
	0x1c1c1c, 0xd75f5f, 0x87af5f, 0xd7af5f, 0x5f87d7, 0xaf87d7, 0x5fafaf, 0xc6c6c6,
	0x585858, 0xff8787, 0xafd787, 0xffd787, 0x87afff, 0xd7afff, 0x87d7d7, 0xeeeeee,
};

/*
 * Key bindings. Keys are matched by their Latin symbol, so they work in any
 * layout (Ctrl+Shift+T is the same key in a Russian layout).
 * Modifiers: CTRL, SHIFT, ALT, SUPER.
 */
static const struct binding bindings[] = {
	{ CTRL | SHIFT, XKB_KEY_t,         ACT_TAB_NEW,       0 },
	{ CTRL | SHIFT, XKB_KEY_w,         ACT_TAB_CLOSE,     0 },
	{ CTRL,         XKB_KEY_Tab,       ACT_TAB_NEXT,      0 },
	{ CTRL | SHIFT, XKB_KEY_Tab,       ACT_TAB_PREV,      0 },
	{ CTRL,         XKB_KEY_Next,      ACT_TAB_NEXT,      0 },
	{ CTRL,         XKB_KEY_Prior,     ACT_TAB_PREV,      0 },
	{ CTRL | SHIFT, XKB_KEY_1,         ACT_TAB_GOTO,      0 },
	{ CTRL | SHIFT, XKB_KEY_2,         ACT_TAB_GOTO,      1 },
	{ CTRL | SHIFT, XKB_KEY_3,         ACT_TAB_GOTO,      2 },
	{ CTRL | SHIFT, XKB_KEY_4,         ACT_TAB_GOTO,      3 },
	{ CTRL | SHIFT, XKB_KEY_5,         ACT_TAB_GOTO,      4 },
	{ CTRL | SHIFT, XKB_KEY_6,         ACT_TAB_GOTO,      5 },
	{ CTRL | SHIFT, XKB_KEY_7,         ACT_TAB_GOTO,      6 },
	{ CTRL | SHIFT, XKB_KEY_8,         ACT_TAB_GOTO,      7 },
	{ CTRL | SHIFT, XKB_KEY_9,         ACT_TAB_GOTO,      8 },

	{ CTRL | SHIFT, XKB_KEY_c,         ACT_COPY,          0 },
	{ CTRL | SHIFT, XKB_KEY_v,         ACT_PASTE,         0 },
	{ SHIFT,        XKB_KEY_Insert,    ACT_PASTE,         0 },

	{ SHIFT,        XKB_KEY_Prior,     ACT_SCROLL_PAGE,  -1 },
	{ SHIFT,        XKB_KEY_Next,      ACT_SCROLL_PAGE,   1 },
	{ SHIFT,        XKB_KEY_Home,      ACT_SCROLL_TOP,    0 },
	{ SHIFT,        XKB_KEY_End,       ACT_SCROLL_BOTTOM, 0 },

	{ CTRL,         XKB_KEY_equal,     ACT_FONT_SIZE,     1 },
	{ CTRL,         XKB_KEY_minus,     ACT_FONT_SIZE,    -1 },
	{ CTRL,         XKB_KEY_0,         ACT_FONT_SIZE,     0 },
};
