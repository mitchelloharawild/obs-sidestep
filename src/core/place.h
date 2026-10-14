/*
 * obs-sidestep — placement logic (no libobs dependency).
 *
 * Candidate generation (anchors, captured position, automatic grid search),
 * selection, and the hysteresis state machine (confirm checks, cooldown,
 * return home, hide/show), plus optional image preference: with a strategy
 * that detects background images, positions on an image are ranked first
 * whenever the content changes.
 *
 * All geometry is in canvas pixels. A "position" is the top-left corner of
 * the camera's bounding box. Time is in nanoseconds (os_gettime_ns()).
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum place_anchor {
	PLACE_ANCHOR_NONE = 0,
	PLACE_ANCHOR_TOP_LEFT,
	PLACE_ANCHOR_TOP_CENTER,
	PLACE_ANCHOR_TOP_RIGHT,
	PLACE_ANCHOR_MIDDLE_LEFT,
	PLACE_ANCHOR_MIDDLE_CENTER,
	PLACE_ANCHOR_MIDDLE_RIGHT,
	PLACE_ANCHOR_BOTTOM_LEFT,
	PLACE_ANCHOR_BOTTOM_CENTER,
	PLACE_ANCHOR_BOTTOM_RIGHT,
	PLACE_ANCHOR_CAPTURED,
	PLACE_ANCHOR_COUNT,
};

enum place_mode {
	PLACE_MODE_PREFERRED = 0,
	PLACE_MODE_PREFERRED_THEN_ANYWHERE = 1, /* default */
	PLACE_MODE_ANYWHERE = 2,
};

enum place_fallback {
	PLACE_FALLBACK_HIDE = 0,       /* default */
	PLACE_FALLBACK_LEAST_BUSY = 1, /* least-busy (auto modes) / stay put (preferred) */
};

#define PLACE_MAX_CHOICES 4

struct place_config {
	enum place_mode mode;
	enum place_anchor choices[PLACE_MAX_CHOICES]; /* priority order; NONE = unused */

	bool has_captured;
	double captured_x, captured_y; /* captured camera top-left */

	double margin;      /* px: anchor inset from canvas edge AND score-rect inflation */
	bool return_home;   /* return to higher-priority choice when clear */
	enum place_fallback fallback;
	double t_suitable;  /* rect suitable iff score <= t_suitable */
	int confirm_checks; /* 1..5 */
	uint64_t return_delay_ns; /* default 3 s */
	uint64_t cooldown_ns;     /* default 1 s, min time between actions */
	double lambda;            /* default 0.01, distance penalty in auto search */
	int grid_divisions;       /* default 48: auto-search stride = canvas_w / 48 */

	/* Image preference (strategies with prefers_images). A position is
	 * image-suitable if it is suitable AND image_frac(camera rect, not
	 * inflated) >= t_image. */
	bool prefer_images; /* default false */
	double t_image;     /* default 0.9 */
};

void place_config_default(struct place_config *c);

/* Score a canvas rect (already inflated by the margin). Returns busy
 * fraction in [0, 1]. Implemented by the caller (maps canvas -> analysis). */
typedef double (*place_score_fn)(void *ctx, double x, double y, double w,
				 double h);

struct place_input {
	uint64_t now_ns;
	double canvas_w, canvas_h;
	double cam_w, cam_h; /* camera bounding-box size */
	double cam_x, cam_y; /* camera's current (or animation-target) top-left */
	bool cam_visible;    /* camera's current visibility */
	place_score_fn score;
	void *score_ctx;

	/* Optional: fraction [0, 1] of a canvas rect covered by a detected
	 * background image (NULL = no image information). Called with
	 * image_ctx, or score_ctx when image_ctx is NULL. */
	place_score_fn image_frac;
	void *image_ctx;

	/* The content changed substantially since the last decision (first
	 * check, first settled check after a transition, after a reset).
	 * With prefer_images this triggers an image re-rank. */
	bool new_content;
};

enum place_action {
	PLACE_ACTION_NONE = 0, /* stay as is */
	PLACE_ACTION_MOVE,     /* move (visible) to x, y */
	PLACE_ACTION_HIDE,     /* hide; position unchanged */
	PLACE_ACTION_SHOW,     /* move to x, y (instantly) and show */
};

/* Report for the debug overlay: rects evaluated this check (anchors /
 * captured / current / auto pick — not every grid cell). Rects are the
 * camera box at that position, NOT inflated. */
#define PLACE_MAX_REPORT 16
struct place_report_item {
	double x, y, w, h;
	double score;
	bool suitable;
	double image_frac; /* 0 without image information */
	bool on_image;     /* image-suitable (prefer_images only) */
};

struct place_decision {
	enum place_action action;
	double x, y;
	struct place_report_item report[PLACE_MAX_REPORT];
	int n_report;
	int chosen; /* index into report of the selected position, or -1 */
};

struct place_state {
	bool initialised;
	int unsuitable_count; /* consecutive settled checks current pos unsuitable */
	int show_count;       /* consecutive checks something suitable while hidden */
	/* Per-priority time since which choices[i] has been continuously
	 * suitable (0 = not currently suitable). */
	uint64_t suitable_since_ns[PLACE_MAX_CHOICES];
	/* Same, for image-suitable (prefer_images only). */
	uint64_t image_since_ns[PLACE_MAX_CHOICES];
	uint64_t last_action_ns;
	bool has_acted;
	/* Image re-rank requested by new_content, not yet acted on (e.g.
	 * blocked by the cooldown). Superseded by the next new_content. */
	bool rerank_pending;
};

void place_state_reset(struct place_state *s);

/*
 * Top-left of an anchor position for the given camera size, inset by the
 * margin (CAPTURED returns the captured position). Returns false for NONE,
 * or CAPTURED without has_captured.
 */
bool place_anchor_pos(enum place_anchor a, const struct place_config *c,
		      double canvas_w, double canvas_h, double cam_w,
		      double cam_h, double *x, double *y);

/*
 * Run one settled check. Call only when content is settled (the caller
 * implements the transition guard). Rules:
 *  - Stay put while the current position is suitable.
 *  - Current unsuitable for confirm_checks consecutive checks: move to
 *    the highest-priority suitable choice (preferred modes); else, in auto
 *    modes, grid-search min(score + lambda*dist/diag) among suitable rects.
 *  - Return home (generalised): if a choice with higher priority than the
 *    current position (current not a choice = lowest priority) has been
 *    continuously suitable for return_delay, move to the highest such one.
 *  - Nothing suitable: HIDE (fallback hide), or move to the least-busy
 *    candidate (fallback least-busy, auto modes) / stay (preferred mode).
 *  - While hidden: SHOW at the selected position once something is
 *    suitable for confirm_checks consecutive checks.
 *  - Cooldown: no MOVE/HIDE/SHOW within cooldown_ns of the previous one.
 * With prefer_images (and in->image_frac):
 *  - new_content requests a re-rank: move (or show) to the highest-priority
 *    image-suitable choice (ANYWHERE: the best image-suitable grid rect),
 *    unless the camera already sits on an image-suitable position of equal
 *    or higher priority. No confirm checks; the request stays pending
 *    through the cooldown and is dropped if there's no image-suitable spot.
 *  - Selections (moves, shows, auto search) rank image-suitable positions
 *    before merely suitable ones.
 *  - While on an image-suitable position, return home only goes to
 *    higher-priority choices that are image-suitable themselves.
 * Positions are clamped so the camera stays within the margins.
 */
struct place_decision place_evaluate(struct place_state *s,
				     const struct place_config *c,
				     const struct place_input *in);

/* Cubic ease-in-out, t in [0,1] (clamped). */
double place_ease_in_out(double t);

#ifdef __cplusplus
}
#endif
