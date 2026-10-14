/*
 * obs-sidestep — avoidance strategy registry (no libobs dependency).
 *
 * A strategy decides what counts as content: it turns the shared per-pixel
 * features of a frame (luma, edge magnitude, motion, RGB) into the cost map
 * the placement logic scores, and optionally an image map of background
 * images the camera may sit on. busy_process() runs:
 *
 *   luma -> edge / motion / global_change      (shared, busy.c)
 *        -> strategy->process: cost (+ image)   (one strat_*.c per strategy)
 *        -> modifiers, in registry order       (post-process hook, optional)
 *        -> summed-area tables
 *
 * Adding a strategy: write src/core/strat_<name>.c defining a
 * `const struct busy_strategy_ops busy_strat_<name>`, declare it below, add
 * it to the table in strategy.c with a new, never-reused id (also add the
 * id to enum busy_strategy_id), add its name_key / desc_key strings to
 * data/locale/en-US.ini, and list the file in CMakeLists.txt.
 *
 * Adding a modifier (adjusts the cost map after any strategy, e.g. "ignore
 * persistent template elements"): define a `struct busy_modifier_ops`,
 * declare it below and add it to the modifier table in strategy.c. Gate it
 * on a new busy_params field via `enabled` so it is opt-in.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "busy.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Read-only access to the frame being processed. Byte offsets ri/gi/bi
 * locate R, G and B inside each 4-byte pixel. */
struct busy_frame {
	const uint8_t *pixels;
	uint32_t linesize;
	enum busy_pixfmt fmt;
	int ri, gi, bi;
};

static inline const uint8_t *busy_frame_px(const struct busy_frame *fr,
					   uint32_t x, uint32_t y)
{
	return fr->pixels + (size_t)y * fr->linesize + 4 * (size_t)x;
}

struct busy_strategy_ops {
	int id;               /* persisted in settings — never renumber */
	const char *name_key; /* locale key for the GUI, e.g. "Strategy.Any" */
	const char *desc_key; /* locale key for the long description */
	size_t state_size;    /* private state, zeroed on create / reset */

	/* Optional: allocate buffers sized by b->w × b->h into the (zeroed)
	 * state. Returning false makes the analyzer fall back to ANY. */
	bool (*init)(struct busy_analyzer *b, void *state);
	/* Optional: free what init allocated (state itself is freed by the
	 * caller). Must cope with a zeroed or partially initialised state. */
	void (*destroy)(void *state);

	/*
	 * Required. Write every pixel of b->cost (0..255). May read b->luma,
	 * b->prev_luma, b->edge, b->motion, b->has_prev and the frame's RGB.
	 * Strategies that detect images write every pixel of b->image (0/1)
	 * and set b->has_image = true (it is false on entry).
	 */
	void (*process)(struct busy_analyzer *b, const struct busy_frame *fr,
			const struct busy_params *p, void *state);

	/* Optional: forget history (busy_reset_history). Without it the
	 * state is zeroed after destroy + init. */
	void (*reset)(struct busy_analyzer *b, void *state);

	/* Placement should rank image-covered positions first. */
	bool prefers_images;
};

struct busy_modifier_ops {
	const char *name; /* for logs / debugging only */
	size_t state_size;
	bool (*init)(struct busy_analyzer *b, void *state);  /* optional */
	void (*destroy)(void *state);                        /* optional */
	/* Optional: NULL = always enabled. */
	bool (*enabled)(const struct busy_params *p);
	/* Required. Adjust b->cost (and b->image if b->has_image) in place. */
	void (*process)(struct busy_analyzer *b, const struct busy_frame *fr,
			const struct busy_params *p, void *state);
	void (*reset)(struct busy_analyzer *b, void *state); /* optional */
};

/* Strategy registry, in GUI order. Index-based iteration for building the
 * dropdown; lookup by persisted id. busy_strategy_find returns the ANY
 * strategy for unknown ids (never NULL). */
size_t busy_strategy_count(void);
const struct busy_strategy_ops *busy_strategy_get(size_t index);
const struct busy_strategy_ops *busy_strategy_find(int id);

/* Modifier registry, applied in order after the strategy. */
size_t busy_modifier_count(void);
const struct busy_modifier_ops *busy_modifier_get(size_t index);

/* Built-in strategies (strat_*.c). */
extern const struct busy_strategy_ops busy_strat_any;
extern const struct busy_strategy_ops busy_strat_bgimage;
extern const struct busy_strategy_ops busy_strat_solid;
extern const struct busy_strategy_ops busy_strat_activity;

/* Built-in modifiers (mod_*.c). */
extern const struct busy_modifier_ops busy_mod_template;

/* Shared helper: cost = 255 where e > t_edge or (use_motion and motion)
 * — the ANY rule — for pixels [i0, i1) of the flattened maps. */
void busy_cost_edge_motion(struct busy_analyzer *b,
			   const struct busy_params *p, size_t i0, size_t i1);

#ifdef __cplusplus
}
#endif
