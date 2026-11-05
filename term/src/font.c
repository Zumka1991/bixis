/* fontconfig lookup, FreeType rasterization, glyph cache, fallback fonts. */
#include "font.h"
#include "util.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H

struct face {
	FT_Face ft;
	bool fake_bold, fake_italic;
};

struct entry {
	uint32_t key;
	bool used;
	struct glyph g;
};

struct font_impl {
	FT_Library lib;
	FcPattern *base;
	double px;
	struct face style[4];
	struct face *fb;          /* fallback faces loaded so far */
	int nfb;
	FcFontSet *fbset;         /* candidates, sorted by fontconfig */
	struct entry *cache;
	size_t cap, count;
};

static bool face_load(struct font_impl *fi, const char *file, int index, struct face *out)
{
	if (FT_New_Face(fi->lib, file, index, &out->ft))
		return false;
	if (!FT_IS_SCALABLE(out->ft) || FT_Set_Pixel_Sizes(out->ft, 0, (FT_UInt)lround(fi->px))) {
		FT_Done_Face(out->ft);
		out->ft = NULL;
		return false;
	}
	return true;
}

static bool style_load(struct font_impl *fi, int style, struct face *out)
{
	FcPattern *p = FcPatternDuplicate(fi->base);
	FcPatternAddInteger(p, FC_WEIGHT, style & STYLE_BOLD ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
	FcPatternAddInteger(p, FC_SLANT, style & STYLE_ITALIC ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
	FcPatternAddDouble(p, FC_PIXEL_SIZE, fi->px);
	FcConfigSubstitute(NULL, p, FcMatchPattern);
	FcDefaultSubstitute(p);

	FcResult res;
	FcPattern *m = FcFontMatch(NULL, p, &res);
	FcPatternDestroy(p);
	if (!m)
		return false;

	FcChar8 *file;
	int index = 0, weight = FC_WEIGHT_REGULAR, slant = FC_SLANT_ROMAN;
	bool ok = FcPatternGetString(m, FC_FILE, 0, &file) == FcResultMatch;
	FcPatternGetInteger(m, FC_INDEX, 0, &index);
	FcPatternGetInteger(m, FC_WEIGHT, 0, &weight);
	FcPatternGetInteger(m, FC_SLANT, 0, &slant);
	if (ok)
		ok = face_load(fi, (const char *)file, index, out);
	FcPatternDestroy(m);

	out->fake_bold = (style & STYLE_BOLD) && weight < FC_WEIGHT_DEMIBOLD;
	out->fake_italic = (style & STYLE_ITALIC) && slant == FC_SLANT_ROMAN;
	return ok;
}

static struct face *fallback_for(struct font_impl *fi, uint32_t cp)
{
	for (int i = 0; i < fi->nfb; i++)
		if (FT_Get_Char_Index(fi->fb[i].ft, cp))
			return &fi->fb[i];

	if (!fi->fbset) {
		FcPattern *p = FcPatternDuplicate(fi->base);
		FcConfigSubstitute(NULL, p, FcMatchPattern);
		FcDefaultSubstitute(p);
		FcResult res;
		fi->fbset = FcFontSort(NULL, p, FcTrue, NULL, &res);
		FcPatternDestroy(p);
		if (!fi->fbset)
			return NULL;
	}

	for (int i = 0; i < fi->fbset->nfont; i++) {
		FcPattern *cand = fi->fbset->fonts[i];
		FcCharSet *cs;
		FcChar8 *file;
		int index = 0;

		if (FcPatternGetCharSet(cand, FC_CHARSET, 0, &cs) != FcResultMatch ||
		    !FcCharSetHasChar(cs, cp) ||
		    FcPatternGetString(cand, FC_FILE, 0, &file) != FcResultMatch)
			continue;
		FcPatternGetInteger(cand, FC_INDEX, 0, &index);

		struct face f = { 0 };
		if (!face_load(fi, (const char *)file, index, &f))
			continue;
		if (!FT_Get_Char_Index(f.ft, cp)) {
			FT_Done_Face(f.ft);
			continue;
		}
		fi->fb = xrealloc(fi->fb, (fi->nfb + 1) * sizeof *fi->fb);
		fi->fb[fi->nfb] = f;
		return &fi->fb[fi->nfb++];
	}
	return NULL;
}

static bool rasterize(struct face *f, uint32_t cp, struct glyph *g)
{
	FT_UInt gi;
	if (!f->ft || !(gi = FT_Get_Char_Index(f->ft, cp)))
		return false;
	if (FT_Load_Glyph(f->ft, gi, FT_LOAD_TARGET_LIGHT))
		return false;

	FT_GlyphSlot s = f->ft->glyph;
	if (f->fake_bold)
		FT_GlyphSlot_Embolden(s);
	if (f->fake_italic)
		FT_GlyphSlot_Oblique(s);
	if (FT_Render_Glyph(s, FT_RENDER_MODE_LIGHT))
		return false;

	FT_Bitmap *bm = &s->bitmap;
	if (bm->pixel_mode != FT_PIXEL_MODE_GRAY && bm->pixel_mode != FT_PIXEL_MODE_MONO)
		return false;

	g->left = s->bitmap_left;
	g->top = s->bitmap_top;
	g->w = bm->width;
	g->h = bm->rows;
	g->bmp = NULL;
	if (!g->w || !g->h)
		return true;

	g->bmp = xmalloc((size_t)g->w * g->h);
	for (int y = 0; y < g->h; y++) {
		const uint8_t *src = bm->buffer + y * bm->pitch;
		uint8_t *dst = g->bmp + y * g->w;
		if (bm->pixel_mode == FT_PIXEL_MODE_GRAY)
			memcpy(dst, src, g->w);
		else
			for (int x = 0; x < g->w; x++)
				dst[x] = src[x >> 3] & (0x80 >> (x & 7)) ? 255 : 0;
	}
	return true;
}

static size_t slot(const struct font_impl *fi, uint32_t key)
{
	size_t i = (key * 2654435761u) & (fi->cap - 1);
	while (fi->cache[i].used && fi->cache[i].key != key)
		i = (i + 1) & (fi->cap - 1);
	return i;
}

static void cache_grow(struct font_impl *fi)
{
	struct entry *old = fi->cache;
	size_t oldcap = fi->cap;

	fi->cap = oldcap ? oldcap * 2 : 1024;
	fi->cache = xcalloc(fi->cap, sizeof *fi->cache);
	for (size_t i = 0; i < oldcap; i++)
		if (old[i].used)
			fi->cache[slot(fi, old[i].key)] = old[i];
	free(old);
}

const struct glyph *font_glyph(struct font *f, uint32_t cp, int style)
{
	struct font_impl *fi = f->impl;
	uint32_t key = cp << 2 | (style & 3);

	if ((fi->count + 1) * 2 > fi->cap)
		cache_grow(fi);
	struct entry *e = &fi->cache[slot(fi, key)];
	if (e->used)
		return &e->g;

	struct glyph g = { 0 };
	if (!rasterize(&fi->style[style & 3], cp, &g) && !rasterize(&fi->style[0], cp, &g)) {
		struct face *fb = fallback_for(fi, cp);
		if (!fb || !rasterize(fb, cp, &g))
			rasterize(&fi->style[0], 0xfffd, &g);
	}
	e->key = key;
	e->used = true;
	e->g = g;
	fi->count++;
	return &e->g;
}

struct font *font_open(const char *family, double size, int scale)
{
	struct font_impl *fi = xcalloc(1, sizeof *fi);
	struct font *f = xcalloc(1, sizeof *f);
	f->impl = fi;

	if (!FcInit() || FT_Init_FreeType(&fi->lib)) {
		fputs("bixis-term: can't init fontconfig/freetype\n", stderr);
		exit(1);
	}
	fi->base = FcNameParse((const FcChar8 *)family);
	fi->px = size * 96.0 / 72.0 * scale;

	if (!style_load(fi, 0, &fi->style[0])) {
		fprintf(stderr, "bixis-term: can't load font '%s'\n", family);
		exit(1);
	}
	for (int s = 1; s < 4; s++)
		if (!style_load(fi, s, &fi->style[s])) {
			fi->style[s] = fi->style[0];
			fi->style[s].ft = NULL; /* rasterize() falls back to regular */
		}

	FT_Face ft = fi->style[0].ft;
	FT_Size_Metrics *m = &ft->size->metrics;
	int ascent = (m->ascender + 63) >> 6;
	int descent = (-m->descender + 63) >> 6;
	int height = MAX(ascent + descent, (int)((m->height + 32) >> 6));

	int advance = 0;
	if (!FT_Load_Char(ft, 'M', FT_LOAD_TARGET_LIGHT))
		advance = (ft->glyph->advance.x + 32) >> 6;
	if (advance <= 0)
		advance = (m->max_advance + 32) >> 6;

	f->cw = MAX(advance, 1);
	f->ch = MAX(height, 1);
	f->baseline = ascent + (height - ascent - descent) / 2;
	f->ul_thick = MAX(1, (int)(FT_MulFix(ft->underline_thickness, m->y_scale) >> 6));
	f->ul_pos = f->baseline - (int)(FT_MulFix(ft->underline_position, m->y_scale) >> 6);
	f->ul_pos = CLAMP(f->ul_pos, 0, f->ch - f->ul_thick);
	return f;
}

void font_close(struct font *f)
{
	struct font_impl *fi = f->impl;

	for (size_t i = 0; i < fi->cap; i++)
		if (fi->cache[i].used)
			free(fi->cache[i].g.bmp);
	free(fi->cache);
	for (int s = 0; s < 4; s++)
		if (fi->style[s].ft && (s == 0 || fi->style[s].ft != fi->style[0].ft))
			FT_Done_Face(fi->style[s].ft);
	for (int i = 0; i < fi->nfb; i++)
		FT_Done_Face(fi->fb[i].ft);
	free(fi->fb);
	if (fi->fbset)
		FcFontSetDestroy(fi->fbset);
	FcPatternDestroy(fi->base);
	FT_Done_FreeType(fi->lib);
	free(fi);
	free(f);
}
