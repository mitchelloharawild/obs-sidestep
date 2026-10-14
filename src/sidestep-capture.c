/*
 * obs-sidestep — GPU downscale + asynchronous stagesurface readback.
 *
 * Pass-through always uses obs_source_skip_video_filter(), so the output
 * path is identical whether or not a capture happens this frame. On capture
 * frames the filter input is rendered a second time, into a small
 * texrender at analysis resolution (the GPU's bilinear sampling does the
 * downscale). Filters further down the chain cache their texrender per
 * frame, so the second render is cheap and consistent.
 *
 * The small texture is copied into one of two stagesurfaces and mapped on
 * a later frame (never the frame it was staged), so the CPU never waits on
 * the GPU.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sidestep-capture.h"

#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>
#include <string.h>

#define NUM_STAGES 2

struct sidestep_capture {
	/* graphics thread only */
	gs_texrender_t *texrender;
	gs_stagesurf_t *stage[NUM_STAGES];
	uint32_t stage_w, stage_h;
	int next_stage;
	int pending;            /* stage index awaiting map, or -1 */
	uint64_t pending_frame; /* obs_get_video_frame_time() when staged */
	uint64_t pending_ts;    /* os_gettime_ns() at capture request */
	bool deferred_want;     /* capture requested but not yet possible */

	/* shared with the tick thread, guarded by mutex */
	pthread_mutex_t mutex;
	uint8_t *cpu;
	size_t cpu_size;
	uint32_t cpu_w, cpu_h;
	uint64_t cpu_ts;
	bool cpu_new;
};

struct sidestep_capture *sidestep_capture_create(void)
{
	struct sidestep_capture *c = bzalloc(sizeof(*c));
	if (pthread_mutex_init(&c->mutex, NULL) != 0) {
		bfree(c);
		return NULL;
	}
	c->pending = -1;
	return c;
}

static void destroy_stages(struct sidestep_capture *c)
{
	for (int i = 0; i < NUM_STAGES; i++) {
		gs_stagesurface_destroy(c->stage[i]);
		c->stage[i] = NULL;
	}
	c->stage_w = c->stage_h = 0;
	c->pending = -1;
}

void sidestep_capture_destroy(struct sidestep_capture *c)
{
	if (!c)
		return;

	obs_enter_graphics();
	gs_texrender_destroy(c->texrender);
	destroy_stages(c);
	obs_leave_graphics();

	pthread_mutex_destroy(&c->mutex);
	bfree(c->cpu);
	bfree(c);
}

/* Map the pending stagesurface and copy it to the CPU buffer. */
static void read_pending(struct sidestep_capture *c)
{
	gs_stagesurf_t *stage = c->stage[c->pending];
	uint8_t *data;
	uint32_t linesize;

	c->pending = -1;

	if (!gs_stagesurface_map(stage, &data, &linesize))
		return;

	const uint32_t w = c->stage_w;
	const uint32_t h = c->stage_h;
	const size_t row = (size_t)w * 4;
	const size_t size = row * h;

	pthread_mutex_lock(&c->mutex);
	if (c->cpu_size < size) {
		c->cpu = brealloc(c->cpu, size);
		c->cpu_size = size;
	}
	if (linesize == row) {
		memcpy(c->cpu, data, size);
	} else {
		for (uint32_t y = 0; y < h; y++)
			memcpy(c->cpu + row * y, data + (size_t)linesize * y, row);
	}
	c->cpu_w = w;
	c->cpu_h = h;
	c->cpu_ts = c->pending_ts;
	c->cpu_new = true;
	pthread_mutex_unlock(&c->mutex);

	gs_stagesurface_unmap(stage);
}

static bool ensure_gpu_objects(struct sidestep_capture *c, uint32_t aw, uint32_t ah)
{
	if (!c->texrender) {
		c->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		if (!c->texrender)
			return false;
	}

	if (c->stage_w != aw || c->stage_h != ah) {
		destroy_stages(c);
		for (int i = 0; i < NUM_STAGES; i++) {
			c->stage[i] = gs_stagesurface_create(aw, ah, GS_RGBA);
			if (!c->stage[i]) {
				destroy_stages(c);
				return false;
			}
		}
		c->stage_w = aw;
		c->stage_h = ah;
		c->next_stage = 0;
	}
	return true;
}

static void capture_input(struct sidestep_capture *c, obs_source_t *filter, uint32_t aw, uint32_t ah)
{
	obs_source_t *target = obs_filter_get_target(filter);
	if (!target)
		return;

	const uint32_t cx = obs_source_get_base_width(target);
	const uint32_t cy = obs_source_get_base_height(target);
	if (!cx || !cy)
		return;

	if (!ensure_gpu_objects(c, aw, ah))
		return;

	gs_texrender_reset(c->texrender);
	if (!gs_texrender_begin_with_color_space(c->texrender, aw, ah, GS_CS_SRGB))
		return;

	struct vec4 clear_color;
	vec4_zero(&clear_color);
	gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);

	/* Whole input in source pixels, squeezed into the aw x ah viewport:
	 * the GPU samples bilinearly. Same blend as OBS's filter texrender. */
	gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);

	const bool previous_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	obs_source_skip_video_filter(filter);

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous_srgb);
	gs_texrender_end(c->texrender);

	const int idx = c->next_stage;
	gs_stage_texture(c->stage[idx], gs_texrender_get_texture(c->texrender));
	c->next_stage = (idx + 1) % NUM_STAGES;
	c->pending = idx;
	c->pending_frame = obs_get_video_frame_time();
	c->pending_ts = os_gettime_ns();
}

void sidestep_capture_render(struct sidestep_capture *c, obs_source_t *filter, uint32_t aw, uint32_t ah,
			     bool want_capture)
{
	/* Pass-through: the input, unchanged, into the current target. */
	obs_source_skip_video_filter(filter);

	if (!c)
		return;

	/* The source may be rendered several times per frame (preview,
	 * program, projectors); only map once a new frame has started. */
	const uint64_t frame = obs_get_video_frame_time();
	if (c->pending >= 0 && c->pending_frame != frame)
		read_pending(c);

	if (want_capture)
		c->deferred_want = true;
	if (!c->deferred_want || !aw || !ah)
		return;
	if (c->pending >= 0) /* staged this frame, still unread */
		return;

	c->deferred_want = false;
	capture_input(c, filter, aw, ah);
}

bool sidestep_capture_take(struct sidestep_capture *c, uint8_t **buf, size_t *buf_size, uint32_t *w, uint32_t *h,
			   uint64_t *frame_ts)
{
	bool got = false;

	if (!c)
		return false;

	pthread_mutex_lock(&c->mutex);
	if (c->cpu_new) {
		const size_t size = (size_t)c->cpu_w * c->cpu_h * 4;
		if (*buf_size < size) {
			*buf = brealloc(*buf, size);
			*buf_size = size;
		}
		memcpy(*buf, c->cpu, size);
		*w = c->cpu_w;
		*h = c->cpu_h;
		*frame_ts = c->cpu_ts;
		c->cpu_new = false;
		got = true;
	}
	pthread_mutex_unlock(&c->mutex);

	return got;
}
