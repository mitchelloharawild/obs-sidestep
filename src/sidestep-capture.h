/*
 * obs-sidestep — GPU downscale + asynchronous stagesurface readback.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#pragma once

#include <obs-module.h>

struct sidestep_capture;

/* Plain allocation; GPU objects are created lazily inside render. */
struct sidestep_capture *sidestep_capture_create(void);
/* Frees GPU objects (enters the graphics context itself). */
void sidestep_capture_destroy(struct sidestep_capture *c);

/*
 * Call from the filter's video_render (graphics thread). Always renders the
 * filter's input to the output unchanged (pass-through). If a previous
 * capture is pending readback, maps it (one frame late, no stall) and copies
 * it to the CPU buffer. If `want_capture`, renders the input downscaled
 * (bilinear) to aw × ah RGBA and stages it for readback on a later frame.
 */
void sidestep_capture_render(struct sidestep_capture *c, obs_source_t *filter,
			     uint32_t aw, uint32_t ah, bool want_capture);

/*
 * Thread-safe. If a new CPU frame is available since the last call, copies
 * it (tightly packed RGBA, w*4 stride) into *buf (realloc'd as needed,
 * caller frees with bfree), sets *w, *h, *frame_ts (os_gettime_ns at
 * capture request) and returns true. Otherwise returns false.
 */
bool sidestep_capture_take(struct sidestep_capture *c, uint8_t **buf,
			   size_t *buf_size, uint32_t *w, uint32_t *h,
			   uint64_t *frame_ts);
