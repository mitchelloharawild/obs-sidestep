/*
 * obs-sidestep — busyness analysis (no libobs dependency).
 *
 * Turns a small RGBA/BGRA frame into a cost map (0..255 per pixel, how bad
 * it is to cover that pixel) using a pluggable avoidance strategy (see
 * strategy.h), builds its summed-area table, and scores rectangles as the
 * mean cost of their area. Strategies that detect background images also
 * fill an image map, scored the same way.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum busy_pixfmt {
	BUSY_FMT_RGBA, /* bytes R, G, B, A */
	BUSY_FMT_BGRA, /* bytes B, G, R, A */
};

/* Strategy ids are persisted in settings — never renumber. */
enum busy_strategy_id {
	BUSY_STRATEGY_ANY = 0,      /* edge OR motion (default) */
	BUSY_STRATEGY_BG_IMAGE = 1, /* sit on side background images */
	BUSY_STRATEGY_SOLID = 2,    /* only sit on the slide background */
	BUSY_STRATEGY_ACTIVITY = 3, /* screen share: avoid recent activity */
};

struct busy_params {
	int t_edge;      /* edge threshold, 0..254 (default 24) */
	int t_motion;    /* motion threshold, 0..255 (default 24) */
	bool use_motion; /* include motion term in the cost (default true) */
	int strategy;    /* enum busy_strategy_id; unknown ids act as ANY */
	bool ignore_template; /* modifier: lower the cost of content that persists across slide changes */
	uint64_t now_ns;      /* capture time (ns, monotonic); 0 = unknown */
};

#define BUSY_DEFAULT_T_EDGE 24
#define BUSY_DEFAULT_T_MOTION 24

void busy_params_default(struct busy_params *p);

struct busy_strategy_ops;

struct busy_analyzer {
	uint32_t w, h;

	uint8_t *luma;      /* w*h, current capture */
	uint8_t *prev_luma; /* w*h, previous capture */
	bool has_prev;

	/* Shared per-pixel features of the last processed frame, computed
	 * before the strategy runs. */
	uint8_t *edge;   /* w*h, e = |dY/dx| + |dY/dy|, saturated at 255 */
	uint8_t *motion; /* w*h, 1 if |Y_t - Y_{t-1}| > t_motion (all 0 without
			    a previous capture) */

	uint8_t *cost; /* w*h, 0..255 cost map written by the strategy */
	uint32_t *sat; /* (w+1)*(h+1) summed-area table of cost, row 0/col 0 zero */

	/* Image map: 1 where the strategy found a background image the
	 * camera may sit on. Only valid when has_image (else all zero and
	 * image_sat is stale). */
	uint8_t *image;
	uint32_t *image_sat;
	bool has_image;

	/* Fraction [0,1] of pixels whose luma changed by more than t_motion
	 * versus the previous capture (computed regardless of use_motion).
	 * 1.0 when there is no previous capture. */
	double global_change;

	/* Active strategy (never NULL after busy_create) and its state. */
	const struct busy_strategy_ops *strategy;
	void *strategy_state;
	int strategy_req; /* id last requested (may be unknown) */
	void **modifier_state;
};

/* Returns NULL on allocation failure or zero / oversized size. The
 * analyzer starts with the ANY strategy. */
struct busy_analyzer *busy_create(uint32_t w, uint32_t h);
void busy_destroy(struct busy_analyzer *b);

/* Forget the previous frame (next process has no motion history) and
 * reset strategy / modifier state (heat maps etc.). */
void busy_reset_history(struct busy_analyzer *b);

/* Switch strategy by id (unknown -> ANY). Fresh, zeroed state; a no-op if
 * `id` is already the requested strategy. Returns false if the strategy's
 * state could not be allocated (the analyzer then falls back to ANY).
 * busy_process calls this itself when p->strategy changes. */
bool busy_set_strategy(struct busy_analyzer *b, int id);

/*
 * Analyse one frame of exactly b->w × b->h pixels. `linesize` is the row
 * stride in bytes (>= 4*w). Afterwards luma/edge/motion/cost/sat (and
 * image/image_sat when has_image) and global_change are updated and the
 * frame becomes the "previous" one for the next call.
 *
 * Edge: e = |Y(x+1,y)-Y(x,y)| + |Y(x,y+1)-Y(x,y)| (missing neighbours at the
 * right/bottom border contribute 0). Motion: |Y_t - Y_{t-1}| > t_motion.
 * The ANY strategy sets cost = 255 where e > t_edge or (use_motion and
 * motion), else 0.
 */
void busy_process(struct busy_analyzer *b, const uint8_t *pixels,
		  uint32_t linesize, enum busy_pixfmt fmt,
		  const struct busy_params *p);

/*
 * Score of a rectangle given in analysis-image pixel coordinates
 * (x, y = top-left; may extend outside the image). Returns
 * sum(cost in rect) / (255 * area(rect)), in [0, 1]. Parts outside the
 * image count as empty. Bounds are rounded to whole pixels (x0 = floor,
 * x1 = ceil); a rect with zero area returns 0.
 */
double busy_rect_score(const struct busy_analyzer *b, double x, double y,
		       double w, double h);

/* Fraction [0, 1] of a rectangle covered by detected background image,
 * same rounding as busy_rect_score. Parts outside the image count as not
 * image; 0 when the strategy doesn't detect images. */
double busy_rect_image_frac(const struct busy_analyzer *b, double x, double y,
			    double w, double h);

/* Map the 0..100 "Sensitivity" slider to T_suitable on a log scale:
 * T = 0.1 * 10^(-2 * s / 100), so 0 -> 0.10, 50 -> 0.01, 100 -> 0.001.
 * Input is clamped to [0, 100]. Default slider value is 50 (1 %). */
double busy_sensitivity_to_threshold(double sensitivity);

#ifdef __cplusplus
}
#endif
