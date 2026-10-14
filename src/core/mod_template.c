/*
 * obs-sidestep — "Ignore template elements" modifier (works after any
 * strategy).
 *
 * Corporate templates put a logo, footer or slide number in a corner on
 * every slide, often exactly where the camera should go. Content that stays
 * identical across several slide changes is "template" and may be covered.
 *
 *  1. Slide changes. A capture whose global change exceeds T_TRIGGER marks
 *     a change as pending; the next capture with global change <=
 *     T_SETTLE (nothing moving) is a settled slide. At each settled slide
 *     every tile (~1/80 of the frame width, 4x4 px at 320x180) is compared
 *     with a stored reference luma of the previous settled slide (per-tile
 *     SAD <= T_SAD per pixel -> unchanged). The change only counts as a
 *     slide change if at least SLIDE_CHANGE_PCT % of the tiles with
 *     content (in either slide) changed: returning to the same slide after
 *     a pop-up, alt-tab or flash, and slide builds that add one bullet, do
 *     not count — otherwise the unchanged slide text would be learnt as
 *     template. The reference is refreshed at every settled slide either
 *     way, so the next comparison is against the latest state.
 *  2. Per-tile counter, updated on counted slide changes only: +1 if the
 *     tile has content (cost > 0 or edge > t_edge somewhere in the tile)
 *     and is unchanged, otherwise 0. A tile is template once its counter
 *     reaches MIN_PERSIST and at least MIN_CHANGES slide changes have
 *     been seen.
 *  3. Template tiles are re-verified against the reference on every
 *     capture (a slide change too subtle to trigger may have put content
 *     there) and their non-zero cost becomes 1 (of 255). Not 0: a rect
 *     fully covered by template edges then scores 1/255 ~ 0.4 %, below
 *     the default threshold (1 %) so covering a logo is effectively free,
 *     while automatic placement still prefers truly empty space over it.
 *     The image map is left alone.
 *  4. Cap. Persistent large regions (an agenda sidebar, a static screen
 *     share whose content only scrolls in one pane) are a documented
 *     trade-off: they are coverable too. But when template tiles make up
 *     more than CAP_CONTENT_PCT % of the content tiles, or more than
 *     CAP_FRAME_PCT % of all tiles, at the last settled slide, it is
 *     probably not a template (a static sidebar, a deck that hardly
 *     changes) and nothing is discounted until a later slide is back under
 *     the cap. A sparse slide (just a section title next to the logo) may
 *     hit the cap too; it has plenty of free space anyway.
 *
 * Triggering at 5 % rather than the filter's 25 % guard: a change between
 * two text-only slides on a plain background often alters less than 25 %
 * of the pixels; the content-based confirmation in step 1 rejects the
 * false triggers a lower threshold lets through.
 *
 * Cost: one pass over the frame on settled-slide frames only; otherwise a
 * scan of the per-tile counters plus a SAD over the (few) template tiles.
 * Memory: one reference luma buffer and 2 bytes per tile.
 *
 * SPDX-License-Identifier: MIT
 */
#include "strategy.h"

#include <stdlib.h>
#include <string.h>

#define T_TRIGGER 0.05      /* global change that marks a pending change */
#define T_SETTLE 0.01       /* global change of a settled capture */
#define T_SAD 6             /* mean |dY| per pixel for an unchanged tile */
#define SLIDE_CHANGE_PCT 50 /* % of content tiles that must change */
#define MIN_CHANGES 3       /* slide changes seen before anything counts */
#define MIN_PERSIST 3       /* slide changes a tile must survive */
#define CAP_CONTENT_PCT 40  /* template tiles <= % of content tiles */
#define CAP_FRAME_PCT 25    /* template tiles <= % of all tiles */
#define TEMPLATE_COST 1     /* cost of template content (of 255) */

struct template_state {
	uint32_t tile, gw, gh;
	uint8_t *ref;         /* w*h luma of the last settled slide */
	uint8_t *count;       /* per tile, slide changes survived (sat. 255) */
	uint8_t *ref_content; /* per tile, content at the last settled slide */
	bool has_ref;
	bool pending;       /* a change happened since the last settled capture */
	bool capped;        /* too much template at the last settled slide */
	uint32_t n_changes; /* counted slide changes since reset */
};

static void template_destroy(void *state)
{
	struct template_state *s = state;
	free(s->ref);
	free(s->count);
	free(s->ref_content);
}

static bool template_init(struct busy_analyzer *b, void *state)
{
	struct template_state *s = state;

	/* Same tiling as the Background images strategy: 2 / 4 / 6 px at
	 * 160 / 320 / 480 wide. */
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

	s->ref = malloc((size_t)b->w * b->h);
	s->count = calloc(nt, 1);
	s->ref_content = calloc(nt, 1);
	return s->ref && s->count && s->ref_content;
}

static void template_reset(struct busy_analyzer *b, void *state)
{
	struct template_state *s = state;
	(void)b;
	memset(s->count, 0, (size_t)s->gw * s->gh);
	memset(s->ref_content, 0, (size_t)s->gw * s->gh);
	s->has_ref = false;
	s->pending = false;
	s->capped = false;
	s->n_changes = 0;
}

static inline int iabs(int v)
{
	return v < 0 ? -v : v;
}

/* Pixel bounds of tile (tx, ty). */
static inline void tile_bounds(const struct template_state *s, const struct busy_analyzer *b, uint32_t tx, uint32_t ty,
			       uint32_t *x0, uint32_t *x1, uint32_t *y0, uint32_t *y1)
{
	*x0 = tx * s->tile;
	*y0 = ty * s->tile;
	*x1 = *x0 + s->tile < b->w ? *x0 + s->tile : b->w;
	*y1 = *y0 + s->tile < b->h ? *y0 + s->tile : b->h;
}

/* Is the tile's current luma within T_SAD of the reference? The tile is
 * compared with one extra column and row: its edge pixels depend on them,
 * and a tile whose only content is the edge of text just below it must
 * not look unchanged while that text changes. */
static bool tile_unchanged(const struct template_state *s, const struct busy_analyzer *b, uint32_t tx, uint32_t ty)
{
	uint32_t x0, x1, y0, y1;
	tile_bounds(s, b, tx, ty, &x0, &x1, &y0, &y1);
	x1 += x1 < b->w;
	y1 += y1 < b->h;
	const uint32_t limit = (x1 - x0) * (y1 - y0) * T_SAD;
	uint32_t sad = 0;
	for (uint32_t y = y0; y < y1; y++) {
		const uint8_t *cur = b->luma + (size_t)y * b->w;
		const uint8_t *ref = s->ref + (size_t)y * b->w;
		for (uint32_t x = x0; x < x1; x++)
			sad += (uint32_t)iabs(cur[x] - ref[x]);
		if (sad > limit)
			return false;
	}
	return true;
}

static bool tile_has_content(const struct template_state *s, const struct busy_analyzer *b, int t_edge, uint32_t tx,
			     uint32_t ty)
{
	uint32_t x0, x1, y0, y1;
	tile_bounds(s, b, tx, ty, &x0, &x1, &y0, &y1);
	for (uint32_t y = y0; y < y1; y++) {
		const size_t row = (size_t)y * b->w;
		for (uint32_t x = x0; x < x1; x++) {
			if (b->cost[row + x] || b->edge[row + x] > t_edge)
				return true;
		}
	}
	return false;
}

/* A settled capture after a change: compare with the reference, update
 * the counters if it was a real slide change, store the new reference. */
static void settle(struct template_state *s, struct busy_analyzer *b, const struct busy_params *p)
{
	const uint32_t gw = s->gw, gh = s->gh;
	const size_t nt = (size_t)gw * gh;

	if (s->has_ref) {
		/* Classify tiles. ref_content (bit 0 = content in the
		 * reference) is reused in place for the new flags: bit 0 =
		 * content now, bit 1 = unchanged. */
		uint32_t n_content = 0, n_changed = 0;
		uint8_t *flags = s->ref_content; /* reused after reading */
		for (uint32_t ty = 0; ty < gh; ty++) {
			for (uint32_t tx = 0; tx < gw; tx++) {
				const size_t t = (size_t)ty * gw + tx;
				const bool was = flags[t] & 1;
				const bool now = tile_has_content(s, b, p->t_edge, tx, ty);
				const bool same = tile_unchanged(s, b, tx, ty);
				if (was || now) {
					n_content++;
					n_changed += !same;
				}
				flags[t] = (uint8_t)(now | (same << 1));
			}
		}

		const bool slide_change = n_content &&
					  (uint64_t)n_changed * 100 >= (uint64_t)n_content * SLIDE_CHANGE_PCT;
		uint32_t n_template = 0, n_now = 0;
		for (size_t t = 0; t < nt; t++) {
			const bool now = flags[t] & 1, same = flags[t] & 2;
			if (slide_change) {
				if (now && same)
					s->count[t] += s->count[t] < 255;
				else
					s->count[t] = 0;
			} else if (!same) {
				/* A build or edit: what changed is new. */
				s->count[t] = 0;
			}
			n_now += now;
			n_template += now && s->count[t] >= MIN_PERSIST;
			flags[t] = now; /* back to plain content flags */
		}
		s->n_changes += slide_change;
		s->capped = (uint64_t)n_template * 100 > (uint64_t)n_now * CAP_CONTENT_PCT ||
			    (uint64_t)n_template * 100 > (uint64_t)nt * CAP_FRAME_PCT;
	} else {
		for (uint32_t ty = 0; ty < gh; ty++)
			for (uint32_t tx = 0; tx < gw; tx++)
				s->ref_content[(size_t)ty * gw + tx] = tile_has_content(s, b, p->t_edge, tx, ty);
		s->has_ref = true;
	}
	memcpy(s->ref, b->luma, (size_t)b->w * b->h);
}

static void template_process(struct busy_analyzer *b, const struct busy_frame *fr, const struct busy_params *p,
			     void *state)
{
	struct template_state *s = state;
	(void)fr;

	const double g = b->global_change;
	if (g > T_TRIGGER) {
		s->pending = true;
	} else if (s->pending && g <= T_SETTLE) {
		s->pending = false;
		settle(s, b, p);
	}

	if (!s->has_ref || s->capped || s->n_changes < MIN_CHANGES)
		return;

	for (uint32_t ty = 0; ty < s->gh; ty++) {
		for (uint32_t tx = 0; tx < s->gw; tx++) {
			if (s->count[(size_t)ty * s->gw + tx] < MIN_PERSIST || !tile_unchanged(s, b, tx, ty))
				continue;
			uint32_t x0, x1, y0, y1;
			tile_bounds(s, b, tx, ty, &x0, &x1, &y0, &y1);
			for (uint32_t y = y0; y < y1; y++) {
				uint8_t *crow = b->cost + (size_t)y * b->w;
				for (uint32_t x = x0; x < x1; x++) {
					if (crow[x] > TEMPLATE_COST)
						crow[x] = TEMPLATE_COST;
				}
			}
		}
	}
}

static bool template_enabled(const struct busy_params *p)
{
	return p->ignore_template;
}

const struct busy_modifier_ops busy_mod_template = {
	.name = "template",
	.state_size = sizeof(struct template_state),
	.init = template_init,
	.destroy = template_destroy,
	.enabled = template_enabled,
	.process = template_process,
	.reset = template_reset,
};
