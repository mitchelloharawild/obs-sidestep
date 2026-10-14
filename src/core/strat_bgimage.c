/*
 * obs-sidestep — "Background images" strategy.
 *
 * For slide decks with photos filling one side (e.g. Quarto/reveal.js
 * `.image-left` / `.image-right`): the camera may sit on the photo, and on
 * the solid-colour background it avoids content exactly like "Any
 * content".
 *
 *  1. Background colour: mode of a 12-bit (4 bits/channel) RGB histogram
 *     over a 2x-subsampled frame, refined to the mean colour of the
 *     pixels in that bin. Without a dominant colour (full-bleed photo,
 *     gradient) the whole frame is scored like "Any content".
 *  2. Tiles of ~1/80 of the frame width (4x4 px at 320x180). Per tile:
 *     pixels within T_BG of the background (L1 RGB) and "exactly flat"
 *     pixels (edge magnitude <= T_FLAT), plus moving pixels.
 *  3. SURFACE tiles are mostly background; the rest are smoothed with a
 *     3x3 vote (fills flat patches inside photos such as sky or bokeh,
 *     drops isolated tiles) and grouped into 4-connected regions.
 *  4. A region is an IMAGE if it is large, touches the frame border (a
 *     background image, not an inline figure), is mostly continuous-tone
 *     (photos have few exactly-flat pixels, flat SVG shapes and coloured
 *     panels have many) and is mostly static (an embedded video is
 *     content, not a place to sit).
 *  5. Cost: SURFACE pixels use the "Any content" rule (edge or motion);
 *     IMAGE tiles cost 0 (moving pixels inside still cost 255) and are
 *     marked in the image map; in other regions (FILL: flat shapes,
 *     panels, code blocks, small pictures, video) every non-background
 *     pixel costs 255, so a filled shape's interior counts as content and
 *     its footprint stays pixel-accurate rather than tile-sized.
 *
 * Known edge case: an animated full-frame background (e.g. a particle
 * iframe on a title slide) is either mostly background colour with moving
 * specks (SURFACE + motion -> content where it moves) or fails the static
 * test, so it is never treated as an image to sit on.
 *
 * SPDX-License-Identifier: MIT
 */
#include "strategy.h"

#include <stdlib.h>
#include <string.h>

/* Tuned against tests/fixtures/user2026 (see tests/eval_fixtures.c). */
#define T_BG 24             /* L1 RGB distance to count as background */
#define T_FLAT 0            /* edge magnitude for an "exactly flat" pixel */
#define DOMINANT_PCT 20     /* bg bin must hold >= this % of pixels */
#define SURFACE_PCT 50      /* tile is SURFACE if >= this % is background */
#define SMOOTH_PASSES 2     /* 3x3 majority passes over non-surface tiles */
#define REGION_MIN_PCT 4    /* image region >= this % of all tiles */
#define REGION_FLAT_PCT 35  /* image region: <= this % exactly-flat pixels */
#define TILE_MOTION_PCT 25  /* tile is "moving" if > this % of px moved */
#define REGION_MOTION_PCT 25 /* image region: <= this % moving tiles */

enum tile_class {
	TILE_SURFACE = 0,
	TILE_OTHER = 1, /* not background; FILL unless its region is IMAGE */
	TILE_IMAGE = 2,
};

struct bgimage_state {
	uint32_t tile, gw, gh;
	uint16_t *n_px;   /* per tile */
	uint16_t *n_bg;
	uint16_t *n_flat;
	uint16_t *n_mot;
	uint8_t *cls;     /* per tile, enum tile_class */
	uint8_t *tmp;     /* smoothing scratch */
	uint32_t *stack;  /* flood-fill stack / region member list */
	uint32_t hist[4096];
};

static void bgimage_destroy(void *state)
{
	struct bgimage_state *s = state;
	free(s->n_px);
	free(s->n_bg);
	free(s->n_flat);
	free(s->n_mot);
	free(s->cls);
	free(s->tmp);
	free(s->stack);
}

static bool bgimage_init(struct busy_analyzer *b, void *state)
{
	struct bgimage_state *s = state;

	/* Tile size scales with the analysis resolution so a tile covers a
	 * similar part of the canvas: 2 / 4 / 6 px at 160 / 320 / 480. */
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

	s->n_px = calloc(nt, sizeof(uint16_t));
	s->n_bg = calloc(nt, sizeof(uint16_t));
	s->n_flat = calloc(nt, sizeof(uint16_t));
	s->n_mot = calloc(nt, sizeof(uint16_t));
	s->cls = calloc(nt, 1);
	s->tmp = calloc(nt, 1);
	s->stack = calloc(nt, sizeof(uint32_t));
	if (!s->n_px || !s->n_bg || !s->n_flat || !s->n_mot ||
	    !s->cls || !s->tmp || !s->stack)
		return false;

	return true;
}

static inline int iabs(int v)
{
	return v < 0 ? -v : v;
}

/* L1 RGB distance of a pixel to the background colour. */
static inline int bg_dist(const struct busy_frame *fr, const uint8_t *px,
			  const int bg[3])
{
	return iabs(px[fr->ri] - bg[0]) + iabs(px[fr->gi] - bg[1]) +
	       iabs(px[fr->bi] - bg[2]);
}

/* Dominant colour; returns false if no colour is dominant. */
static bool estimate_bg(struct bgimage_state *s, const struct busy_analyzer *b,
			const struct busy_frame *fr, int bg[3])
{
	memset(s->hist, 0, sizeof(s->hist));
	uint32_t n = 0;
	for (uint32_t y = 0; y < b->h; y += 2) {
		for (uint32_t x = 0; x < b->w; x += 2) {
			const uint8_t *px = busy_frame_px(fr, x, y);
			s->hist[((px[fr->ri] >> 4) << 8) |
				((px[fr->gi] >> 4) << 4) | (px[fr->bi] >> 4)]++;
			n++;
		}
	}
	uint32_t mode = 0;
	for (uint32_t i = 1; i < 4096; i++) {
		if (s->hist[i] > s->hist[mode])
			mode = i;
	}
	if ((uint64_t)s->hist[mode] * 100 < (uint64_t)n * DOMINANT_PCT)
		return false;

	/* Mean colour of the pixels in the mode bin. */
	uint64_t sum[3] = {0, 0, 0}, cnt = 0;
	for (uint32_t y = 0; y < b->h; y += 2) {
		for (uint32_t x = 0; x < b->w; x += 2) {
			const uint8_t *px = busy_frame_px(fr, x, y);
			uint32_t q = ((px[fr->ri] >> 4) << 8) |
				     ((px[fr->gi] >> 4) << 4) | (px[fr->bi] >> 4);
			if (q != mode)
				continue;
			sum[0] += px[fr->ri];
			sum[1] += px[fr->gi];
			sum[2] += px[fr->bi];
			cnt++;
		}
	}
	for (int c = 0; c < 3; c++)
		bg[c] = (int)((sum[c] + cnt / 2) / cnt);
	return true;
}

/* One 3x3 vote over the surface / non-surface split, with hysteresis: a
 * tile becomes non-surface with a majority (>= 5 of 9) and surface with
 * <= 2 of 9 votes, otherwise keeps its class. Fills holes, drops isolated
 * tiles, but keeps the corners and thin strokes of real shapes. Cells
 * outside the grid don't vote. */
static void smooth(struct bgimage_state *s)
{
	const int gw = (int)s->gw, gh = (int)s->gh;
	for (int y = 0; y < gh; y++) {
		for (int x = 0; x < gw; x++) {
			int votes = 0, total = 0;
			for (int dy = -1; dy <= 1; dy++) {
				int yy = y + dy;
				if (yy < 0 || yy >= gh)
					continue;
				for (int dx = -1; dx <= 1; dx++) {
					int xx = x + dx;
					if (xx < 0 || xx >= gw)
						continue;
					votes += s->cls[yy * gw + xx] != TILE_SURFACE;
					total++;
				}
			}
			uint8_t c = s->cls[y * gw + x];
			if (2 * votes > total)
				c = TILE_OTHER;
			else if (3 * votes < total)
				c = TILE_SURFACE;
			s->tmp[y * gw + x] = c;
		}
	}
	memcpy(s->cls, s->tmp, (size_t)gw * gh);
}

/* Group TILE_OTHER cells into 4-connected regions and promote the regions
 * that look like background images to TILE_IMAGE. */
static void find_images(struct bgimage_state *s)
{
	const uint32_t gw = s->gw, gh = s->gh, nt = gw * gh;
	const uint64_t min_tiles = ((uint64_t)nt * REGION_MIN_PCT + 99) / 100;
	uint8_t *seen = s->tmp;
	memset(seen, 0, nt);

	for (uint32_t start = 0; start < nt; start++) {
		if (s->cls[start] != TILE_OTHER || seen[start])
			continue;

		/* Flood fill; the stack doubles as the member list. */
		uint32_t top = 0, n = 0;
		uint64_t px = 0, flat = 0;
		uint32_t moving = 0;
		bool border = false;
		s->stack[top++] = start;
		seen[start] = 1;
		while (n < top) {
			uint32_t t = s->stack[n++];
			uint32_t x = t % gw, y = t / gw;
			px += s->n_px[t];
			flat += s->n_flat[t];
			moving += (uint32_t)s->n_mot[t] * 100 >
				  (uint32_t)s->n_px[t] * TILE_MOTION_PCT;
			if (x == 0 || y == 0 || x == gw - 1 || y == gh - 1)
				border = true;
			const uint32_t nb[4] = {x > 0 ? t - 1 : t,
						x + 1 < gw ? t + 1 : t,
						y > 0 ? t - gw : t,
						y + 1 < gh ? t + gw : t};
			for (int i = 0; i < 4; i++) {
				uint32_t u = nb[i];
				if (s->cls[u] == TILE_OTHER && !seen[u]) {
					seen[u] = 1;
					s->stack[top++] = u;
				}
			}
		}

		bool image = border && n >= min_tiles &&
			     flat * 100 <= px * REGION_FLAT_PCT &&
			     (uint64_t)moving * 100 <= (uint64_t)n * REGION_MOTION_PCT;
		if (image) {
			for (uint32_t i = 0; i < n; i++)
				s->cls[s->stack[i]] = TILE_IMAGE;
		}
	}
}

static void bgimage_process(struct busy_analyzer *b, const struct busy_frame *fr,
			    const struct busy_params *p, void *state)
{
	struct bgimage_state *s = state;
	const uint32_t w = b->w, h = b->h, tile = s->tile, gw = s->gw;
	const size_t nt = (size_t)gw * s->gh;

	/* Surface pixels (and the whole frame without a dominant colour)
	 * use the "Any content" rule. */
	busy_cost_edge_motion(b, p, 0, (size_t)w * h);

	int bg[3];
	if (!estimate_bg(s, b, fr, bg))
		return;

	/* Per-tile counts. */
	memset(s->n_px, 0, nt * sizeof(uint16_t));
	memset(s->n_bg, 0, nt * sizeof(uint16_t));
	memset(s->n_flat, 0, nt * sizeof(uint16_t));
	memset(s->n_mot, 0, nt * sizeof(uint16_t));
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *src = fr->pixels + (size_t)y * fr->linesize;
		const uint8_t *erow = b->edge + (size_t)y * w;
		const uint8_t *mrow = b->motion + (size_t)y * w;
		const size_t trow = (size_t)(y / tile) * gw;
		for (uint32_t tx = 0; tx < gw; tx++) {
			const uint32_t x0 = tx * tile;
			const uint32_t x1 = x0 + tile < w ? x0 + tile : w;
			unsigned n_bg = 0, n_flat = 0, n_mot = 0;
			for (uint32_t x = x0; x < x1; x++) {
				n_bg += bg_dist(fr, src + 4 * x, bg) <= T_BG;
				n_flat += erow[x] <= T_FLAT;
				n_mot += mrow[x];
			}
			const size_t t = trow + tx;
			s->n_px[t] += (uint16_t)(x1 - x0);
			s->n_bg[t] += (uint16_t)n_bg;
			s->n_flat[t] += (uint16_t)n_flat;
			s->n_mot[t] += (uint16_t)n_mot;
		}
	}

	for (size_t t = 0; t < nt; t++) {
		bool surface = (uint32_t)s->n_bg[t] * 100 >=
			       (uint32_t)s->n_px[t] * SURFACE_PCT;
		s->cls[t] = surface ? TILE_SURFACE : TILE_OTHER;
	}
	for (int i = 0; i < SMOOTH_PASSES; i++)
		smooth(s);
	find_images(s);

	/* Write cost / image per tile. */
	memset(b->image, 0, (size_t)w * h);
	bool any_image = false;
	for (uint32_t ty = 0; ty < s->gh; ty++) {
		const uint32_t y0 = ty * tile;
		const uint32_t y1 = y0 + tile < h ? y0 + tile : h;
		for (uint32_t tx = 0; tx < gw; tx++) {
			const uint8_t c = s->cls[ty * gw + tx];
			if (c == TILE_SURFACE)
				continue;
			const uint32_t x0 = tx * tile;
			const uint32_t x1 = x0 + tile < w ? x0 + tile : w;
			for (uint32_t y = y0; y < y1; y++) {
				const size_t row = (size_t)y * w;
				uint8_t *crow = b->cost + row;
				uint8_t *irow = b->image + row;
				const uint8_t *mrow = b->motion + row;
				const uint8_t *src =
					fr->pixels + (size_t)y * fr->linesize;
				for (uint32_t x = x0; x < x1; x++) {
					if (c == TILE_OTHER) {
						if (bg_dist(fr, src + 4 * x, bg) >
						    T_BG)
							crow[x] = 255;
					} else if (p->use_motion && mrow[x]) {
						crow[x] = 255;
					} else {
						crow[x] = 0;
						irow[x] = 1;
					}
				}
			}
			any_image = any_image || c == TILE_IMAGE;
		}
	}
	b->has_image = any_image;
}

/* Nothing carries over between frames; keep the buffers. */
static void bgimage_reset(struct busy_analyzer *b, void *state)
{
	(void)b;
	(void)state;
}

const struct busy_strategy_ops busy_strat_bgimage = {
	.id = BUSY_STRATEGY_BG_IMAGE,
	.name_key = "Strategy.BgImage",
	.desc_key = "Strategy.BgImage.Description",
	.state_size = sizeof(struct bgimage_state),
	.init = bgimage_init,
	.destroy = bgimage_destroy,
	.process = bgimage_process,
	.reset = bgimage_reset,
	.prefers_images = true,
};
