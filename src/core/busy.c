/*
 * obs-sidestep — busyness analysis (no libobs dependency).
 *
 * SPDX-License-Identifier: MIT
 */
#include "busy.h"
#include "strategy.h"

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
	p->strategy = BUSY_STRATEGY_ANY;
	p->ignore_template = false;
	p->now_ns = 0;
}

/* ------------------------------------------------------------------------ */
/* Strategy / modifier state                                                 */

static void strategy_free(struct busy_analyzer *b)
{
	if (b->strategy && b->strategy->destroy && b->strategy_state)
		b->strategy->destroy(b->strategy_state);
	free(b->strategy_state);
	b->strategy_state = NULL;
}

/* Allocate zeroed state for `s` and init it; false on failure. */
static bool strategy_alloc(struct busy_analyzer *b,
			   const struct busy_strategy_ops *s)
{
	b->strategy = s;
	if (s->state_size) {
		b->strategy_state = calloc(1, s->state_size);
		if (!b->strategy_state)
			return false;
	}
	if (s->init && !s->init(b, b->strategy_state)) {
		/* init may have allocated part of its buffers */
		strategy_free(b);
		return false;
	}
	return true;
}

bool busy_set_strategy(struct busy_analyzer *b, int id)
{
	if (!b)
		return false;
	if (b->strategy && b->strategy_req == id)
		return true;

	strategy_free(b);
	b->strategy_req = id;
	if (strategy_alloc(b, busy_strategy_find(id)))
		return true;

	/* ANY has no state, so this cannot fail. */
	strategy_alloc(b, &busy_strat_any);
	return false;
}

static void modifiers_free(struct busy_analyzer *b)
{
	if (!b->modifier_state)
		return;
	for (size_t i = 0, n = busy_modifier_count(); i < n; i++) {
		const struct busy_modifier_ops *m = busy_modifier_get(i);
		if (m->destroy && b->modifier_state[i])
			m->destroy(b->modifier_state[i]);
		free(b->modifier_state[i]);
	}
	free(b->modifier_state);
	b->modifier_state = NULL;
}

static bool modifiers_alloc(struct busy_analyzer *b)
{
	size_t n = busy_modifier_count();
	b->modifier_state = calloc(n ? n : 1, sizeof(void *));
	if (!b->modifier_state)
		return false;
	for (size_t i = 0; i < n; i++) {
		const struct busy_modifier_ops *m = busy_modifier_get(i);
		if (m->state_size) {
			b->modifier_state[i] = calloc(1, m->state_size);
			if (!b->modifier_state[i])
				return false;
		}
		if (m->init && !m->init(b, b->modifier_state[i]))
			return false;
	}
	return true;
}

/* ------------------------------------------------------------------------ */

struct busy_analyzer *busy_create(uint32_t w, uint32_t h)
{
	if (!w || !h)
		return NULL;

	/* Guard against size overflow in (w+1)*(h+1)*4 and keep the cost
	 * SAT (up to 255 per pixel) within uint32. */
	if ((uint64_t)(w + 1ull) * (h + 1ull) > (SIZE_MAX / sizeof(uint32_t)))
		return NULL;
	if ((uint64_t)w * h * 255u > UINT32_MAX)
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
	b->edge = calloc(n, 1);
	b->motion = calloc(n, 1);
	b->cost = calloc(n, 1);
	b->sat = calloc(sat_n, sizeof(uint32_t));
	b->image = calloc(n, 1);
	b->image_sat = calloc(sat_n, sizeof(uint32_t));
	b->has_prev = false;
	b->global_change = 1.0;

	if (!b->luma || !b->prev_luma || !b->edge || !b->motion || !b->cost ||
	    !b->sat || !b->image || !b->image_sat || !modifiers_alloc(b)) {
		busy_destroy(b);
		return NULL;
	}
	b->strategy_req = BUSY_STRATEGY_ANY;
	strategy_alloc(b, &busy_strat_any);
	return b;
}

void busy_destroy(struct busy_analyzer *b)
{
	if (!b)
		return;
	strategy_free(b);
	modifiers_free(b);
	free(b->luma);
	free(b->prev_luma);
	free(b->edge);
	free(b->motion);
	free(b->cost);
	free(b->sat);
	free(b->image);
	free(b->image_sat);
	free(b);
}

void busy_reset_history(struct busy_analyzer *b)
{
	if (!b)
		return;
	b->has_prev = false;

	const struct busy_strategy_ops *s = b->strategy;
	if (s && s->reset) {
		s->reset(b, b->strategy_state);
	} else if (s && s->state_size) {
		int id = b->strategy_req;
		strategy_free(b);
		b->strategy = NULL; /* force re-creation */
		busy_set_strategy(b, id);
	}

	for (size_t i = 0, n = busy_modifier_count(); i < n; i++) {
		const struct busy_modifier_ops *m = busy_modifier_get(i);
		if (m->reset)
			m->reset(b, b->modifier_state[i]);
	}
}

static inline int iabs(int v)
{
	return v < 0 ? -v : v;
}

/* S(x+1,y+1) = sum of map[0..y][0..x]. */
static void build_sat(const uint8_t *map, uint32_t *sat, uint32_t w,
		      uint32_t h)
{
	const size_t sw = (size_t)w + 1;
	memset(sat, 0, sw * sizeof(uint32_t));
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *mrow = map + (size_t)y * w;
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
	if (p->strategy != b->strategy_req || !b->strategy)
		busy_set_strategy(b, p->strategy);

	const uint32_t w = b->w, h = b->h;
	const struct busy_frame fr = {
		.pixels = pixels,
		.linesize = linesize,
		.fmt = fmt,
		.ri = fmt == BUSY_FMT_BGRA ? 2 : 0,
		.gi = 1,
		.bi = fmt == BUSY_FMT_BGRA ? 0 : 2,
	};

	/* Previous capture becomes prev_luma; swap buffers instead of copying. */
	uint8_t *tmp = b->prev_luma;
	b->prev_luma = b->luma;
	b->luma = tmp;

	/* 1. Luma */
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *src = pixels + (size_t)y * linesize;
		uint8_t *dst = b->luma + (size_t)y * w;
		for (uint32_t x = 0; x < w; x++) {
			const uint8_t *px = src + 4 * x;
			dst[x] = (uint8_t)((77 * px[fr.ri] + 150 * px[1] +
					    29 * px[fr.bi]) >> 8);
		}
	}

	/* 2. Edge magnitude and motion; count global change. */
	const bool has_prev = b->has_prev;
	const int t_motion = p->t_motion;
	uint64_t changed = 0;

	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *row = b->luma + (size_t)y * w;
		const uint8_t *below = y + 1 < h ? row + w : NULL;
		const uint8_t *prow = b->prev_luma + (size_t)y * w;
		uint8_t *erow = b->edge + (size_t)y * w;
		uint8_t *mrow = b->motion + (size_t)y * w;

		for (uint32_t x = 0; x < w; x++) {
			int c = row[x];
			int e = 0;
			if (x + 1 < w)
				e += iabs(row[x + 1] - c);
			if (below)
				e += iabs(below[x] - c);
			erow[x] = (uint8_t)(e > 255 ? 255 : e);
		}
		if (has_prev) {
			for (uint32_t x = 0; x < w; x++) {
				uint8_t m = iabs(row[x] - prow[x]) > t_motion;
				mrow[x] = m;
				changed += m;
			}
		} else {
			memset(mrow, 0, w);
		}
	}

	b->global_change = has_prev ? (double)changed / ((double)w * h) : 1.0;
	b->has_prev = true;

	/* 3. Strategy -> cost (+ image), then modifiers. */
	b->has_image = false;
	b->strategy->process(b, &fr, p, b->strategy_state);

	for (size_t i = 0, n = busy_modifier_count(); i < n; i++) {
		const struct busy_modifier_ops *m = busy_modifier_get(i);
		if (!m->enabled || m->enabled(p))
			m->process(b, &fr, p, b->modifier_state[i]);
	}

	/* 4. Summed-area tables. */
	build_sat(b->cost, b->sat, w, h);
	if (b->has_image)
		build_sat(b->image, b->image_sat, w, h);
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

/* Sum of a SAT over a rect (clipped to the image) and the rect's full
 * rounded area. Returns false for empty rects. */
static bool rect_sum(const struct busy_analyzer *b, const uint32_t *s,
		     double x, double y, double w, double h, uint32_t *sum,
		     double *area)
{
	*sum = 0;
	*area = 0.0;
	if (!b || !(w > 0.0) || !(h > 0.0))
		return false;

	int64_t x0 = clamp_coord(floor(x));
	int64_t y0 = clamp_coord(floor(y));
	int64_t x1 = clamp_coord(ceil(x + w));
	int64_t y1 = clamp_coord(ceil(y + h));

	if (x1 <= x0 || y1 <= y0)
		return false;

	*area = (double)(x1 - x0) * (double)(y1 - y0);

	/* Clip to the image; outside pixels count as empty. */
	int64_t cx0 = clamp_i64(x0, 0, b->w), cx1 = clamp_i64(x1, 0, b->w);
	int64_t cy0 = clamp_i64(y0, 0, b->h), cy1 = clamp_i64(y1, 0, b->h);
	if (cx1 <= cx0 || cy1 <= cy0)
		return true;

	const size_t sw = (size_t)b->w + 1;
	*sum = s[(size_t)cy1 * sw + (size_t)cx1] -
	       s[(size_t)cy0 * sw + (size_t)cx1] -
	       s[(size_t)cy1 * sw + (size_t)cx0] +
	       s[(size_t)cy0 * sw + (size_t)cx0];
	return true;
}

double busy_rect_score(const struct busy_analyzer *b, double x, double y,
		       double w, double h)
{
	uint32_t sum;
	double area;
	if (!rect_sum(b, b ? b->sat : NULL, x, y, w, h, &sum, &area))
		return 0.0;
	/* sum and 255*area are exact, so this equals count/area exactly
	 * for a 0/255 cost map. */
	return (double)sum / (255.0 * area);
}

double busy_rect_image_frac(const struct busy_analyzer *b, double x, double y,
			    double w, double h)
{
	uint32_t sum;
	double area;
	if (!b || !b->has_image)
		return 0.0;
	if (!rect_sum(b, b->image_sat, x, y, w, h, &sum, &area))
		return 0.0;
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
