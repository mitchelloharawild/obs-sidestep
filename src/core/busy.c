/*
 * obs-sidestep — busyness analysis (no libobs dependency).
 *
 * SPDX-License-Identifier: MIT
 */
#include "busy.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void busy_params_default(struct busy_params *p)
{
	if (!p)
		return;
	p->t_edge = BUSY_DEFAULT_T_EDGE;
	p->t_motion = BUSY_DEFAULT_T_MOTION;
	p->use_motion = true;
}

struct busy_analyzer *busy_create(uint32_t w, uint32_t h)
{
	if (!w || !h)
		return NULL;

	/* guard against size overflow in (w+1)*(h+1)*4 */
	if ((uint64_t)(w + 1ull) * (h + 1ull) > (SIZE_MAX / sizeof(uint32_t)))
		return NULL;

	struct busy_analyzer *b = calloc(1, sizeof(*b));
	if (!b)
		return NULL;

	size_t n = (size_t)w * h;
	size_t sat_n = ((size_t)w + 1) * ((size_t)h + 1);

	b->w = w;
	b->h = h;
	b->luma = malloc(n);
	b->prev_luma = malloc(n);
	b->mask = calloc(n, 1);
	b->sat = calloc(sat_n, sizeof(uint32_t));
	b->has_prev = false;
	b->global_change = 1.0;

	if (!b->luma || !b->prev_luma || !b->mask || !b->sat) {
		busy_destroy(b);
		return NULL;
	}
	return b;
}

void busy_destroy(struct busy_analyzer *b)
{
	if (!b)
		return;
	free(b->luma);
	free(b->prev_luma);
	free(b->mask);
	free(b->sat);
	free(b);
}

void busy_reset_history(struct busy_analyzer *b)
{
	if (b)
		b->has_prev = false;
}

static inline int iabs(int v)
{
	return v < 0 ? -v : v;
}

void busy_process(struct busy_analyzer *b, const uint8_t *pixels,
		  uint32_t linesize, enum busy_pixfmt fmt,
		  const struct busy_params *p)
{
	if (!b || !pixels)
		return;

	struct busy_params def;
	if (!p) {
		busy_params_default(&def);
		p = &def;
	}

	const uint32_t w = b->w, h = b->h;

	/* Previous capture becomes prev_luma; swap buffers instead of copying. */
	uint8_t *tmp = b->prev_luma;
	b->prev_luma = b->luma;
	b->luma = tmp;

	/* 1. Luma */
	const int ri = fmt == BUSY_FMT_BGRA ? 2 : 0;
	const int bi = fmt == BUSY_FMT_BGRA ? 0 : 2;
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *src = pixels + (size_t)y * linesize;
		uint8_t *dst = b->luma + (size_t)y * w;
		for (uint32_t x = 0; x < w; x++) {
			const uint8_t *px = src + 4 * x;
			dst[x] = (uint8_t)((77 * px[ri] + 150 * px[1] +
					    29 * px[bi]) >> 8);
		}
	}

	/* 2-4. Edge OR motion -> mask; count global change. */
	const bool has_prev = b->has_prev;
	const bool motion = has_prev && p->use_motion;
	const int t_edge = p->t_edge;
	const int t_motion = p->t_motion;
	uint64_t changed = 0;

	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *row = b->luma + (size_t)y * w;
		const uint8_t *below = y + 1 < h ? row + w : NULL;
		const uint8_t *prow = b->prev_luma + (size_t)y * w;
		uint8_t *mrow = b->mask + (size_t)y * w;

		for (uint32_t x = 0; x < w; x++) {
			int c = row[x];
			int e = 0;
			if (x + 1 < w)
				e += iabs(row[x + 1] - c);
			if (below)
				e += iabs(below[x] - c);

			bool busy = e > t_edge;
			if (has_prev) {
				bool m = iabs(c - prow[x]) > t_motion;
				changed += m;
				if (motion)
					busy = busy || m;
			}
			mrow[x] = busy ? 1 : 0;
		}
	}

	b->global_change = has_prev ? (double)changed / ((double)w * h) : 1.0;
	b->has_prev = true;

	/* Summed-area table: S(x+1,y+1) = sum of mask[0..y][0..x]. */
	const size_t sw = (size_t)w + 1;
	uint32_t *sat = b->sat;
	memset(sat, 0, sw * sizeof(uint32_t));
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *mrow = b->mask + (size_t)y * w;
		const uint32_t *up = sat + (size_t)y * sw;
		uint32_t *cur = sat + (size_t)(y + 1) * sw;
		uint32_t run = 0;
		cur[0] = 0;
		for (uint32_t x = 0; x < w; x++) {
			run += mrow[x];
			cur[x + 1] = up[x + 1] + run;
		}
	}
}

/* Clamp a double coordinate into an int64 range safe for arithmetic. */
static inline int64_t clamp_coord(double v)
{
	const double lim = 1e12;
	if (v != v) /* NaN */
		return 0;
	if (v < -lim)
		v = -lim;
	if (v > lim)
		v = lim;
	return (int64_t)v;
}

static inline int64_t clamp_i64(int64_t v, int64_t lo, int64_t hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

double busy_rect_score(const struct busy_analyzer *b, double x, double y,
		       double w, double h)
{
	if (!b || !(w > 0.0) || !(h > 0.0))
		return 0.0;

	int64_t x0 = clamp_coord(floor(x));
	int64_t y0 = clamp_coord(floor(y));
	int64_t x1 = clamp_coord(ceil(x + w));
	int64_t y1 = clamp_coord(ceil(y + h));

	if (x1 <= x0 || y1 <= y0)
		return 0.0;

	double area = (double)(x1 - x0) * (double)(y1 - y0);

	/* Clip to the image; outside pixels count as empty. */
	int64_t cx0 = clamp_i64(x0, 0, b->w), cx1 = clamp_i64(x1, 0, b->w);
	int64_t cy0 = clamp_i64(y0, 0, b->h), cy1 = clamp_i64(y1, 0, b->h);
	if (cx1 <= cx0 || cy1 <= cy0)
		return 0.0;

	const size_t sw = (size_t)b->w + 1;
	const uint32_t *s = b->sat;
	uint32_t sum = s[(size_t)cy1 * sw + (size_t)cx1] -
		       s[(size_t)cy0 * sw + (size_t)cx1] -
		       s[(size_t)cy1 * sw + (size_t)cx0] +
		       s[(size_t)cy0 * sw + (size_t)cx0];

	return (double)sum / area;
}

double busy_sensitivity_to_threshold(double sensitivity)
{
	if (sensitivity != sensitivity)
		sensitivity = 50.0;
	if (sensitivity < 0.0)
		sensitivity = 0.0;
	if (sensitivity > 100.0)
		sensitivity = 100.0;
	return 0.1 * pow(10.0, -2.0 * sensitivity / 100.0);
}
