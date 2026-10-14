/*
 * obs-sidestep — debug overlay (private).
 *
 * Holds a copy of the busy mask and the last check's candidate rects, and
 * draws them over the filter output. Setters may be called from any
 * thread; render only from the graphics thread.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <obs-module.h>

#define SIDESTEP_OVERLAY_MAX_RECTS 16

/* A rect in the filter input's own pixel space. */
struct sidestep_overlay_rect {
	float x, y, w, h;
	float score; /* busy fraction [0, 1] */
	bool suitable;
	bool chosen;
};

struct sidestep_overlay;

struct sidestep_overlay *sidestep_overlay_create(void);
/* Frees GPU objects (enters the graphics context itself). */
void sidestep_overlay_destroy(struct sidestep_overlay *o);

/* Copy a w x h 0/1 mask (analysis resolution). */
void sidestep_overlay_set_mask(struct sidestep_overlay *o, const uint8_t *mask, uint32_t w, uint32_t h);
void sidestep_overlay_set_rects(struct sidestep_overlay *o, const struct sidestep_overlay_rect *rects, int n);
void sidestep_overlay_set_status(struct sidestep_overlay *o, bool unsettled, bool paused);
void sidestep_overlay_clear(struct sidestep_overlay *o);

/* Draw over the current target, in a cx x cy pixel space. */
void sidestep_overlay_render(struct sidestep_overlay *o, uint32_t cx, uint32_t cy);
