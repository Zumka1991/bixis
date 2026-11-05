#ifndef BIXIS_UTIL_H
#define BIXIS_UTIL_H

#include <stdio.h>
#include <stdlib.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(x, lo, hi) MIN(MAX(x, lo), hi)
#define LEN(a) (sizeof(a) / sizeof((a)[0]))

static inline void *oom(void *p)
{
	if (!p) {
		fputs("bixis-term: out of memory\n", stderr);
		abort();
	}
	return p;
}

#define xmalloc(n)     oom(malloc(n))
#define xcalloc(n, s)  oom(calloc(n, s))
#define xrealloc(p, n) oom(realloc(p, n))

static inline int utf8_encode(char *out, unsigned cp)
{
	if (cp < 0x80) {
		out[0] = cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = 0xc0 | cp >> 6;
		out[1] = 0x80 | (cp & 0x3f);
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = 0xe0 | cp >> 12;
		out[1] = 0x80 | (cp >> 6 & 0x3f);
		out[2] = 0x80 | (cp & 0x3f);
		return 3;
	}
	out[0] = 0xf0 | cp >> 18;
	out[1] = 0x80 | (cp >> 12 & 0x3f);
	out[2] = 0x80 | (cp >> 6 & 0x3f);
	out[3] = 0x80 | (cp & 0x3f);
	return 4;
}

#endif
