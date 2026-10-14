/*
 * obs-sidestep — "Screen share / live demo" strategy.
 *
 * In a screen share or live demo, static content (an idle window, a menu
 * bar, yesterday's terminal output) may be covered, but the area being
 * worked in — the line being typed, an output pane, where the cursor
 * dwells, a playing video — may not. Viewers look where things just
 * changed, so this strategy remembers recent motion in a heat map.
 *
 *  1. Tiles of ~1/80 of the frame width (4x4 px at 320x180), as in
 *     "Background images". Per tile, the fraction of moving pixels f
 *     (motion = luma change since the previous capture).
 *  2. heat = max(heat * decay(dt), 255 * min(1, MOTION_GAIN * f)), with a
 *     half-life of HALF_LIFE_S seconds of capture time (p->now_ns). When
 *     the timestamp is unknown (0) or not increasing, the heat decays by
 *     one FALLBACK_DT_S step per frame instead. Heat is kept in 8.8 fixed
 *     point so slow decay at short check intervals doesn't stall or round
 *     away.
 *  3. Cost of a pixel = max(heat of its tile dilated by one tile (3x3 max,
 *     so the camera doesn't sit right next to a cursor), EDGE_COST where
 *     the edge magnitude exceeds t_edge).
 *
 * Static edges and the suitability threshold. Placement compares a rect's
 * mean cost with T_suitable (Sensitivity, default 1 %). Text covers ~30–60 %
 * of the pixels of a text-heavy area at analysis resolution, so a weight
 * like 80 would make every static window unsuitable (30 % * 80 / 255 ≈
 * 9 %) and the mode would behave like "Any content". EDGE_COST is
 * therefore tiny (4 → 0.5–0.9 % for dense text): at the default
 * Sensitivity static text is coverable, but it still ranks below empty
 * space, so "Preferred, then anywhere" picks an empty area when there is
 * one. Raising Sensitivity makes static text matter again (at 75 %, i.e.
 * 0.3 %, rects with more than ~20 % text pixels are unsuitable), so the
 * slider doubles as "how much to avoid static content". Heat dominates
 * at any setting: one active tile, dilated to 3x3 tiles at 255, is ~4 %
 * of a camera-sized rect (80x45 px at 320x180); it takes ~2 half-lives
 * (~16 s) of quiet to fall under 1 %.
 *
 * Whole-frame changes (window switch, scrolling, slide change, the first
 * frame) would light up every tile and keep the camera from sitting
 * anywhere for ~20 s. Motion is ignored (heat only decays) on frames with
 * global change > GLOBAL_MAX, and on the frame right after one — it
 * compares against a capture taken mid-transition, so its motion is the
 * tail of that transition. Existing heat is kept: after a scroll the
 * active area is usually still where it was, and after a window switch
 * the stale heat fades within seconds. A full-screen video therefore
 * produces no heat (there is nowhere better to go anyway).
 *
 * "Treat motion as content" off: no new heat is added, so only static
 * edges (at EDGE_COST) count.
 *
 * SPDX-License-Identifier: MIT
 */
#include "strategy.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define HALF_LIFE_S 8.0    /* heat half-life, seconds of capture time */
#define FALLBACK_DT_S 1.0  /* decay step per frame without timestamps */
#define MAX_DT_S 120.0     /* longer gaps decay to zero anyway */
#define MOTION_GAIN 4      /* tile is fully hot at >= 1/4 of px moving */
#define EDGE_COST 4        /* cost of a static edge pixel (see above) */
#define GLOBAL_MAX 0.25    /* ignore motion above this global change */

struct activity_state {
	uint32_t tile, gw, gh;
	uint16_t *heat;  /* per tile, 8.8 fixed point (0..255 << 8) */
	uint16_t *n_mot; /* per tile, moving pixels this frame */
	uint8_t *hot;    /* per tile, heat >> 8, dilated 3x3 */
	uint8_t *row;    /* per tile row scratch for the dilation */
	uint64_t last_ns;
	bool skip_next;  /* last frame was a global change */
};

static void activity_destroy(void *state)
{
	struct activity_state *s = state;
	free(s->heat);
	free(s->n_mot);
	free(s->hot);
	free(s->row);
}

static bool activity_init(struct busy_analyzer *b, void *state)
{
	struct activity_state *s = state;

	/* Same tile scaling as "Background images": 2 / 4 / 6 px at
	 * 160 / 320 / 480. */
	uint32_t span = b->w;
	if ((uint64_t)b->h * 16 / 9 > span)
		span = (uint32_t)((uint64_t)b->h * 16 / 9);
	uint32_t tile = (span + 40) / 80;
	if (tile < 2)
		tile = 2;
	if (tile > 16)
		tile = 16;

	s->tile = tile;
	s->gw = (b->w + tile - 1) / tile;
	s->gh = (b->h + tile - 1) / tile;
	const size_t nt = (size_t)s->gw * s->gh;

	s->heat = calloc(nt, sizeof(uint16_t));
	s->n_mot = calloc(nt, sizeof(uint16_t));
	s->hot = calloc(nt, 1);
	s->row = calloc(s->gw, 1);
	return s->heat && s->n_mot && s->hot && s->row;
}

/* Decay multiplier (0.16 fixed point) for the time since the last frame.
 * Unknown (0) timestamps decay one fixed step; a timestamp that goes
 * backwards also decays one step and restarts the clock from it. */
static uint32_t decay_mul(struct activity_state *s, uint64_t now_ns)
{
	double dt = FALLBACK_DT_S;
	if (now_ns) {
		if (s->last_ns && now_ns >= s->last_ns)
			dt = (double)(now_ns - s->last_ns) * 1e-9;
		s->last_ns = now_ns;
	}
	if (dt > MAX_DT_S)
		return 0;
	return (uint32_t)(65536.0 * exp2(-dt / HALF_LIFE_S));
}

static void activity_process(struct busy_analyzer *b,
			     const struct busy_frame *fr,
			     const struct busy_params *p, void *state)
{
	(void)fr;
	struct activity_state *s = state;
	const uint32_t w = b->w, h = b->h, tile = s->tile;
	const uint32_t gw = s->gw, gh = s->gh;
	const size_t nt = (size_t)gw * gh;

	const uint32_t mul = decay_mul(s, p->now_ns);
	const bool global = b->global_change > GLOBAL_MAX; /* 1.0 on the first frame */
	const bool use = p->use_motion && !global && !s->skip_next;
	s->skip_next = global;

	/* 1. Moving pixels per tile. */
	if (use) {
		memset(s->n_mot, 0, nt * sizeof(uint16_t));
		for (uint32_t y = 0; y < h; y++) {
			const uint8_t *mrow = b->motion + (size_t)y * w;
			uint16_t *trow = s->n_mot + (size_t)(y / tile) * gw;
			for (uint32_t tx = 0, x0 = 0; tx < gw; tx++, x0 += tile) {
				const uint32_t x1 = x0 + tile < w ? x0 + tile : w;
				unsigned n = 0;
				for (uint32_t x = x0; x < x1; x++)
					n += mrow[x];
				trow[tx] += (uint16_t)n;
			}
		}
	}

	/* 2. Decay, then raise to this frame's motion. Edge tiles are
	 * smaller, so normalise by the tile's actual pixel count. */
	for (uint32_t ty = 0; ty < gh; ty++) {
		const uint32_t th = ty * tile + tile <= h ? tile : h - ty * tile;
		for (uint32_t tx = 0; tx < gw; tx++) {
			const size_t t = (size_t)ty * gw + tx;
			uint32_t v = ((uint32_t)s->heat[t] * mul) >> 16;
			if (use && s->n_mot[t]) {
				const uint32_t tw = tx * tile + tile <= w
							    ? tile
							    : w - tx * tile;
				uint32_t m = (uint32_t)s->n_mot[t] * MOTION_GAIN *
					     (255u << 8) / (tw * th);
				if (m > (255u << 8))
					m = 255u << 8;
				if (m > v)
					v = m;
			}
			s->heat[t] = (uint16_t)v;
		}
	}

	/* 3. 3x3 max dilation of the 8-bit heat: horizontal into s->row,
	 * then vertical into s->hot. */
	for (uint32_t ty = 0; ty < gh; ty++) {
		const uint16_t *hrow = s->heat + (size_t)ty * gw;
		uint8_t *orow = s->hot + (size_t)ty * gw;
		for (uint32_t tx = 0; tx < gw; tx++) {
			uint16_t m = hrow[tx];
			if (tx > 0 && hrow[tx - 1] > m)
				m = hrow[tx - 1];
			if (tx + 1 < gw && hrow[tx + 1] > m)
				m = hrow[tx + 1];
			orow[tx] = (uint8_t)(m >> 8);
		}
	}
	uint8_t *prev = s->row; /* unmodified copy of the row above */
	for (uint32_t ty = 0; ty < gh; ty++) {
		uint8_t *cur = s->hot + (size_t)ty * gw;
		const uint8_t *below = ty + 1 < gh ? cur + gw : NULL;
		for (uint32_t tx = 0; tx < gw; tx++) {
			uint8_t c = cur[tx], m = c;
			if (ty > 0 && prev[tx] > m)
				m = prev[tx];
			if (below && below[tx] > m)
				m = below[tx];
			prev[tx] = c;
			cur[tx] = m;
		}
	}

	/* 4. Cost = max(dilated heat, low-weight static edges). */
	const int t_edge = p->t_edge;
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *erow = b->edge + (size_t)y * w;
		const uint8_t *hrow = s->hot + (size_t)(y / tile) * gw;
		uint8_t *crow = b->cost + (size_t)y * w;
		for (uint32_t tx = 0, x0 = 0; tx < gw; tx++, x0 += tile) {
			const uint32_t x1 = x0 + tile < w ? x0 + tile : w;
			const uint8_t c = hrow[tx];
			if (c >= EDGE_COST) {
				memset(crow + x0, c, x1 - x0);
				continue;
			}
			for (uint32_t x = x0; x < x1; x++)
				crow[x] = erow[x] > t_edge ? EDGE_COST : c;
		}
	}
}

static void activity_reset(struct busy_analyzer *b, void *state)
{
	struct activity_state *s = state;
	const size_t nt = (size_t)s->gw * s->gh;
	(void)b;
	memset(s->heat, 0, nt * sizeof(uint16_t));
	memset(s->hot, 0, nt);
	s->last_ns = 0;
	s->skip_next = false;
}

const struct busy_strategy_ops busy_strat_activity = {
	.id = BUSY_STRATEGY_ACTIVITY,
	.name_key = "Strategy.Activity",
	.desc_key = "Strategy.Activity.Description",
	.state_size = sizeof(struct activity_state),
	.init = activity_init,
	.destroy = activity_destroy,
	.process = activity_process,
	.reset = activity_reset,
	.prefers_images = false,
};
