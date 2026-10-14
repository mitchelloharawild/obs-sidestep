/*
 * obs-sidestep — "Solid backgrounds" strategy: only sit on the slide
 * background.
 *
 * A pixel is content (cost 255) if it is an edge or moving (the "Any
 * content" rule) or if it isn't part of the slide's background. Unlike
 * edge-only scoring, the interior of a large flat shape, a coloured callout
 * panel, a code block or a solid chart bar counts as content.
 *
 *  1. Background colour: mode of a 12-bit (4 bits/channel) RGB histogram
 *     over a 2x-subsampled frame, refined to the exact (8 bits/channel)
 *     most frequent colour inside that bin. The exact mode rather than the
 *     bin's mean (as "Background images" uses), so a light-grey code block
 *     sharing white's bin (#f0f0f0 vs #ffffff) can't pull the background
 *     colour towards itself.
 *  2. Two-tone templates (dark title bar on a white body, coloured side
 *     band): the largest bin at least two steps away from the first in
 *     some channel is a second background colour if it holds >=
 *     SECOND_PCT of the pixels, >= SECOND_BORDER_PCT of the frame's
 *     outermost ring of pixels, and >= SECOND_TIGHT_PCT of its bin sits
 *     on one exact colour (+-1 in the low nibbles). Template bands run to
 *     the frame edge and are perfectly flat; a coloured panel inside the
 *     slide doesn't touch the edge (stays content), and the dark backdrop
 *     of a photo isn't one flat colour.
 *  3. Background region: pixels within T_BG (L1 RGB) of a background
 *     colour, grown through non-edge neighbours whose colour differs by
 *     <= T_STEP. Gradients and vignettes that join the background colour
 *     smoothly become background (their far side would otherwise all
 *     "deviate"), while any crisp boundary — a shape, panel or photo
 *     edge, even one too faint to be an edge — stops the growth.
 *  4. Dominance: the first colour must explain (within T_BG) at least
 *     SEED_PCT of the pixels and the grown background region must cover
 *     at least DOMINANT_PCT. Otherwise (full-bleed photo, steep or noisy
 *     gradient, busy texture) local contrast replaces the global colour:
 *     cost = edge | motion | |Y - boxblur_R(Y)| > T_LC, from a luma SAT,
 *     R = h / 12 (15 px at 320x180, about a third of the camera's short
 *     side at its default size). The local mean follows gradients, so they
 *     are never content; photos are busy almost everywhere. Flat shapes
 *     wider than ~2R read as background again in this mode, the usual
 *     local-contrast trade-off.
 *
 * Everything is per frame (no history), so a slide change is picked up
 * immediately.
 *
 * SPDX-License-Identifier: MIT
 */
#include "strategy.h"

#include <stdlib.h>
#include <string.h>

#define T_BG 24              /* L1 RGB distance to count as background */
#define SEED_PCT 10          /* 1st bg colour must explain >= this % of px */
#define DOMINANT_PCT 25      /* background region must cover >= this % */
#define SECOND_PCT 10        /* 2nd bg colour: >= this % of pixels, */
#define SECOND_BORDER_PCT 25 /* >= this % of the frame's outermost ring, */
#define SECOND_TIGHT_PCT 60  /* >= this % of its bin on one exact colour */
#define T_STEP 6             /* max L1 RGB step when growing background */
#define T_LC 12              /* local contrast threshold (luma) */
#define LC_RADIUS_DIV 12     /* local contrast box radius = h / this */

struct solid_state {
	uint32_t *luma_sat; /* (w+1)*(h+1), local contrast fallback */
	uint8_t *lab;       /* w*h: 1 background, 2 may grow, 0 neither */
	uint32_t *rgb;      /* w*h, packed 0x00BBGGRR copy of the frame */
	uint32_t *queue;    /* w*h, region growing */
	uint32_t hist[4096];
	uint32_t sub[2][4096]; /* low nibbles inside the mode bin(s) */
};

static void solid_destroy(void *state)
{
	struct solid_state *s = state;
	free(s->luma_sat);
	free(s->lab);
	free(s->rgb);
	free(s->queue);
}

static bool solid_init(struct busy_analyzer *b, void *state)
{
	struct solid_state *s = state;
	const size_t n = (size_t)b->w * b->h;
	s->luma_sat = calloc(((size_t)b->w + 1) * ((size_t)b->h + 1),
			     sizeof(uint32_t));
	s->lab = malloc(n);
	s->rgb = malloc(n * sizeof(uint32_t));
	s->queue = malloc(n * sizeof(uint32_t));
	return s->luma_sat && s->lab && s->rgb && s->queue;
}

static inline int iabs(int v)
{
	return v < 0 ? -v : v;
}

static inline uint32_t bin12(const uint8_t *px, int ri, int gi, int bi)
{
	return ((uint32_t)(px[ri] >> 4) << 8) | ((uint32_t)(px[gi] >> 4) << 4) |
	       (uint32_t)(px[bi] >> 4);
}

static inline uint32_t low12(const uint8_t *px, int ri, int gi, int bi)
{
	return ((uint32_t)(px[ri] & 15) << 8) | ((uint32_t)(px[gi] & 15) << 4) |
	       (uint32_t)(px[bi] & 15);
}

static uint32_t argmax(const uint32_t *h, uint32_t n)
{
	uint32_t m = 0;
	for (uint32_t i = 1; i < n; i++) {
		if (h[i] > h[m])
			m = i;
	}
	return m;
}

/* Bins at least two steps apart in some channel (not one colour split
 * across a bin boundary). */
static bool bins_apart(uint32_t a, uint32_t c)
{
	for (int sh = 0; sh <= 8; sh += 4) {
		if (iabs((int)((a >> sh) & 15) - (int)((c >> sh) & 15)) >= 2)
			return true;
	}
	return false;
}

/* Share (sum) of a low-nibble histogram within +-1 per channel of l. */
static uint32_t tight_count(const uint32_t *sub, uint32_t l)
{
	const int r0 = (int)(l >> 8), g0 = (int)((l >> 4) & 15),
		  b0 = (int)(l & 15);
	uint32_t n = 0;
	for (int r = r0 - 1; r <= r0 + 1; r++) {
		for (int g = g0 - 1; g <= g0 + 1; g++) {
			for (int bl = b0 - 1; bl <= b0 + 1; bl++) {
				if (r < 0 || r > 15 || g < 0 || g > 15 ||
				    bl < 0 || bl > 15)
					continue;
				n += sub[(r << 8) | (g << 4) | bl];
			}
		}
	}
	return n;
}

/* Background colour(s) of the frame (bg[0] is the most frequent);
 * returns how many (1 or 2). */
static int estimate_bg(struct solid_state *s, const struct busy_analyzer *b,
		       const struct busy_frame *fr, int bg[2][3])
{
	const uint32_t w = b->w, h = b->h;
	const int ri = fr->ri, gi = fr->gi, bi = fr->bi;

	/* Coarse histogram. */
	memset(s->hist, 0, sizeof(s->hist));
	uint32_t n = 0;
	for (uint32_t y = 0; y < h; y += 2) {
		const uint8_t *src = fr->pixels + (size_t)y * fr->linesize;
		for (uint32_t x = 0; x < w; x += 2, n++)
			s->hist[bin12(src + 4 * x, ri, gi, bi)]++;
	}
	const uint32_t m1 = argmax(s->hist, 4096);

	/* Second candidate: big, far enough from the first, on the border. */
	uint32_t m2 = m1;
	for (uint32_t i = 0; i < 4096; i++) {
		if (bins_apart(i, m1) && (m2 == m1 || s->hist[i] > s->hist[m2]))
			m2 = i;
	}
	bool two = m2 != m1 &&
		   (uint64_t)s->hist[m2] * 100 >= (uint64_t)n * SECOND_PCT;
	if (two) {
		uint32_t ring = 0, hit = 0;
		for (uint32_t x = 0; x < w; x++) {
			hit += bin12(busy_frame_px(fr, x, 0), ri, gi, bi) == m2;
			hit += bin12(busy_frame_px(fr, x, h - 1), ri, gi, bi) == m2;
			ring += 2;
		}
		for (uint32_t y = 1; y + 1 < h; y++) {
			hit += bin12(busy_frame_px(fr, 0, y), ri, gi, bi) == m2;
			hit += bin12(busy_frame_px(fr, w - 1, y), ri, gi, bi) == m2;
			ring += 2;
		}
		two = (uint64_t)hit * 100 >= (uint64_t)ring * SECOND_BORDER_PCT;
	}

	/* Exact colour: mode of the low nibbles inside each bin. */
	memset(s->sub, 0, sizeof(s->sub));
	for (uint32_t y = 0; y < h; y += 2) {
		const uint8_t *src = fr->pixels + (size_t)y * fr->linesize;
		for (uint32_t x = 0; x < w; x += 2) {
			const uint8_t *px = src + 4 * x;
			const uint32_t q = bin12(px, ri, gi, bi);
			if (q == m1)
				s->sub[0][low12(px, ri, gi, bi)]++;
			else if (two && q == m2)
				s->sub[1][low12(px, ri, gi, bi)]++;
		}
	}
	uint32_t l[2] = {argmax(s->sub[0], 4096), 0};
	if (two) {
		l[1] = argmax(s->sub[1], 4096);
		two = (uint64_t)tight_count(s->sub[1], l[1]) * 100 >=
		      (uint64_t)s->hist[m2] * SECOND_TIGHT_PCT;
	}

	const int nbg = two ? 2 : 1;
	for (int k = 0; k < nbg; k++) {
		const uint32_t q = k ? m2 : m1;
		for (int c = 0; c < 3; c++) {
			const int sh = 8 - 4 * c; /* R, G, B nibbles */
			bg[k][c] = (int)((((q >> sh) & 15) << 4) |
					 ((l[k] >> sh) & 15));
		}
	}
	return nbg;
}

/* L1 RGB step between two packed 0x00BBGGRR colours. */
static inline int px_step(uint32_t a, uint32_t c)
{
	return iabs((int)(a & 255) - (int)(c & 255)) +
	       iabs((int)((a >> 8) & 255) - (int)((c >> 8) & 255)) +
	       iabs((int)(a >> 16) - (int)(c >> 16));
}

/*
 * Label the background region (step 3) and write the cost map. Returns
 * false, leaving the cost map alone, when the first colour isn't dominant.
 */
static bool solid_cost(struct busy_analyzer *b, struct solid_state *s,
		       const struct busy_frame *fr, const struct busy_params *p,
		       const int bg[2][3], int nbg)
{
	const uint32_t w = b->w, h = b->h;
	const int ri = fr->ri, gi = fr->gi, bi = fr->bi;
	const int t_edge = p->t_edge;
	const uint8_t *edge = b->edge;
	uint8_t *lab = s->lab;
	uint32_t *rgb = s->rgb;
	uint32_t *queue = s->queue;

	/* Per-channel distance tables; distance to bg[0] in the low and to
	 * bg[1] in the high 16 bits, so one sum gives both. */
	uint32_t tab[3][256];
	for (int c = 0; c < 3; c++) {
		for (int v = 0; v < 256; v++) {
			uint32_t d0 = (uint32_t)iabs(v - bg[0][c]);
			uint32_t d1 = nbg > 1 ? (uint32_t)iabs(v - bg[1][c]) : 255;
			tab[c][v] = d0 | d1 << 16;
		}
	}

	/* Label, pack, count pixels explained by the first colour and all
	 * background pixels. */
	uint64_t explained = 0, inside = 0;
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *src = fr->pixels + (size_t)y * fr->linesize;
		const size_t row = (size_t)y * w;
		for (uint32_t x = 0; x < w; x++) {
			const uint8_t *px = src + 4 * x;
			const uint8_t r = px[ri], g = px[gi], bl = px[bi];
			const uint32_t t = tab[0][r] + tab[1][g] + tab[2][bl];
			const uint32_t d0 = t & 0xffff, d1 = t >> 16;
			const bool in0 = d0 <= T_BG;
			const bool in = in0 || d1 <= T_BG;
			explained += in0;
			inside += in;
			lab[row + x] = in ? 1
				       : edge[row + x] > t_edge ? 0
								: 2;
			rgb[row + x] = (uint32_t)r | (uint32_t)g << 8 |
				       (uint32_t)bl << 16;
		}
	}
	if (explained * 100 < (uint64_t)w * h * SEED_PCT)
		return false;

	/* Grow: seed with growable pixels next to background, then flood.
	 * lab 2 -> 1 when joined. */
	uint32_t top = 0;
	for (uint32_t y = 0; y < h; y++) {
		const uint32_t row = y * w;
		for (uint32_t x = 0; x < w; x++) {
			const uint32_t i = row + x;
			if (lab[i] != 2)
				continue;
			const uint32_t c = rgb[i];
			if ((x > 0 && lab[i - 1] == 1 &&
			     px_step(c, rgb[i - 1]) <= T_STEP) ||
			    (x + 1 < w && lab[i + 1] == 1 &&
			     px_step(c, rgb[i + 1]) <= T_STEP) ||
			    (y > 0 && lab[i - w] == 1 &&
			     px_step(c, rgb[i - w]) <= T_STEP) ||
			    (y + 1 < h && lab[i + w] == 1 &&
			     px_step(c, rgb[i + w]) <= T_STEP))
				queue[top++] = i;
		}
	}
	/* Mark after the scan so seeding only joins the original region. */
	for (uint32_t k = 0; k < top; k++)
		lab[queue[k]] = 1;

	const uint32_t n = w * h;
	for (uint32_t head = 0; head < top; head++) {
		const uint32_t i = queue[head];
		const uint32_t x = i % w;
		const uint32_t c = rgb[i];
		const uint32_t nb[4] = {x > 0 ? i - 1 : i, x + 1 < w ? i + 1 : i,
					i >= w ? i - w : i,
					i + w < n ? i + w : i};
		for (int k = 0; k < 4; k++) {
			const uint32_t j = nb[k];
			if (lab[j] != 2 || px_step(c, rgb[j]) > T_STEP)
				continue;
			lab[j] = 1;
			queue[top++] = j;
		}
	}

	if ((inside + top) * 100 < (uint64_t)n * DOMINANT_PCT)
		return false;

	const bool use_motion = p->use_motion;
	const uint8_t *motion = b->motion;
	uint8_t *cost = b->cost;
	for (uint32_t i = 0; i < n; i++) {
		bool c = lab[i] != 1 || edge[i] > t_edge ||
			 (use_motion && motion[i]);
		cost[i] = c ? 255 : 0;
	}
	return true;
}

/* Fallback (step 4): edge | motion | local luma contrast. */
static void local_contrast(struct busy_analyzer *b, struct solid_state *s,
			   const struct busy_params *p)
{
	const uint32_t w = b->w, h = b->h;
	const size_t sw = (size_t)w + 1;
	uint32_t *sat = s->luma_sat;

	memset(sat, 0, sw * sizeof(uint32_t));
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *row = b->luma + (size_t)y * w;
		const uint32_t *up = sat + (size_t)y * sw;
		uint32_t *cur = sat + (size_t)(y + 1) * sw;
		uint32_t run = 0;
		cur[0] = 0;
		for (uint32_t x = 0; x < w; x++) {
			run += row[x];
			cur[x + 1] = up[x + 1] + run;
		}
	}

	uint32_t r = h / LC_RADIUS_DIV;
	if (r < 1)
		r = 1;
	const int t_edge = p->t_edge;
	const bool use_motion = p->use_motion;
	for (uint32_t y = 0; y < h; y++) {
		const uint32_t y0 = y > r ? y - r : 0;
		const uint32_t y1 = y + r + 1 < h ? y + r + 1 : h;
		const uint32_t *top = sat + (size_t)y0 * sw;
		const uint32_t *bot = sat + (size_t)y1 * sw;
		const size_t row = (size_t)y * w;
		const uint8_t *lrow = b->luma + row;
		const uint8_t *erow = b->edge + row;
		const uint8_t *mrow = b->motion + row;
		uint8_t *crow = b->cost + row;
		for (uint32_t x = 0; x < w; x++) {
			if (erow[x] > t_edge || (use_motion && mrow[x])) {
				crow[x] = 255;
				continue;
			}
			const uint32_t x0 = x > r ? x - r : 0;
			const uint32_t x1 = x + r + 1 < w ? x + r + 1 : w;
			const uint32_t sum = bot[x1] - bot[x0] - top[x1] + top[x0];
			const int64_t area = (int64_t)(x1 - x0) * (y1 - y0);
			/* |Y - mean| > T  <=>  |Y * area - sum| > T * area */
			int64_t diff = (int64_t)lrow[x] * area - (int64_t)sum;
			if (diff < 0)
				diff = -diff;
			crow[x] = diff > (int64_t)T_LC * area ? 255 : 0;
		}
	}
}

static void solid_process(struct busy_analyzer *b, const struct busy_frame *fr,
			  const struct busy_params *p, void *state)
{
	struct solid_state *s = state;
	int bg[2][3];
	const int nbg = estimate_bg(s, b, fr, bg);
	if (!solid_cost(b, s, fr, p, bg, nbg))
		local_contrast(b, s, p);
}

/* Nothing carries over between frames; keep the buffers. */
static void solid_reset(struct busy_analyzer *b, void *state)
{
	(void)b;
	(void)state;
}

const struct busy_strategy_ops busy_strat_solid = {
	.id = BUSY_STRATEGY_SOLID,
	.name_key = "Strategy.Solid",
	.desc_key = "Strategy.Solid.Description",
	.state_size = sizeof(struct solid_state),
	.init = solid_init,
	.destroy = solid_destroy,
	.process = solid_process,
	.reset = solid_reset,
	.prefers_images = false,
};
