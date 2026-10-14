/*
 * obs-sidestep — busyness analysis (no libobs dependency).
 *
 * Turns a small RGBA/BGRA frame into a binary busy mask
 * (edge OR motion), builds its summed-area table, and scores rectangles
 * as the fraction of their area that contains content.
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

struct busy_params {
	int t_edge;      /* edge threshold, 0..510 (default 24) */
	int t_motion;    /* motion threshold, 0..255 (default 24) */
	bool use_motion; /* include motion term in mask (default true) */
};

#define BUSY_DEFAULT_T_EDGE 24
#define BUSY_DEFAULT_T_MOTION 24

void busy_params_default(struct busy_params *p);

struct busy_analyzer {
	uint32_t w, h;

	uint8_t *luma;      /* w*h, current capture */
	uint8_t *prev_luma; /* w*h, previous capture */
	bool has_prev;

	uint8_t *mask; /* w*h, 0/1 busy mask of the last processed frame */
	uint32_t *sat; /* (w+1)*(h+1) summed-area table of mask, row 0/col 0 zero */

	/* Fraction [0,1] of pixels whose luma changed by more than t_motion
	 * versus the previous capture (computed regardless of use_motion).
	 * 1.0 when there is no previous capture. */
	double global_change;
};

/* Returns NULL on allocation failure or zero size. */
struct busy_analyzer *busy_create(uint32_t w, uint32_t h);
void busy_destroy(struct busy_analyzer *b);

/* Forget the previous frame (next process has no motion history). */
void busy_reset_history(struct busy_analyzer *b);

/*
 * Analyse one frame of exactly b->w × b->h pixels. `linesize` is the row
 * stride in bytes (>= 4*w). Afterwards luma/mask/sat/global_change are
 * updated and the frame becomes the "previous" one for the next call.
 *
 * Edge: e = |Y(x+1,y)-Y(x,y)| + |Y(x,y+1)-Y(x,y)| (missing neighbours at the
 * right/bottom border contribute 0); edge = e > t_edge.
 * Motion: |Y_t - Y_{t-1}| > t_motion (only when has_prev and use_motion).
 */
void busy_process(struct busy_analyzer *b, const uint8_t *pixels,
		  uint32_t linesize, enum busy_pixfmt fmt,
		  const struct busy_params *p);

/*
 * Score of a rectangle given in analysis-image pixel coordinates
 * (x, y = top-left; may extend outside the image). Returns
 * sum(mask in rect) / area(rect), in [0, 1]. Parts outside the image count
 * as empty. Bounds are rounded to whole pixels (x0 = floor, x1 = ceil);
 * a rect with zero area returns 0.
 */
double busy_rect_score(const struct busy_analyzer *b, double x, double y,
		       double w, double h);

/* Map the 0..100 "Sensitivity" slider to T_suitable on a log scale:
 * T = 0.1 * 10^(-2 * s / 100), so 0 -> 0.10, 50 -> 0.01, 100 -> 0.001.
 * Input is clamped to [0, 100]. Default slider value is 50 (1 %). */
double busy_sensitivity_to_threshold(double sensitivity);

#ifdef __cplusplus
}
#endif
