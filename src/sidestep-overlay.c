/*
 * obs-sidestep — debug overlay: busy mask tint, candidate outlines and a
 * tiny built-in 3x5 bitmap font for score labels / status text.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "sidestep-overlay.h"

#include <util/bmem.h>
#include <util/threading.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

struct sidestep_overlay {
	pthread_mutex_t mutex;

	/* mask as RGBA (red, alpha = tint), rebuilt by set_mask */
	uint8_t *mask_rgba;
	uint32_t mask_w, mask_h;
	bool mask_dirty;
	bool has_mask;

	struct sidestep_overlay_rect rects[SIDESTEP_OVERLAY_MAX_RECTS];
	int n_rects;
	bool unsettled;
	bool paused;

	/* graphics thread only */
	gs_texture_t *tex;
	uint32_t tex_w, tex_h;
};

struct sidestep_overlay *sidestep_overlay_create(void)
{
	struct sidestep_overlay *o = bzalloc(sizeof(*o));
	if (pthread_mutex_init(&o->mutex, NULL) != 0) {
		bfree(o);
		return NULL;
	}
	return o;
}

void sidestep_overlay_destroy(struct sidestep_overlay *o)
{
	if (!o)
		return;
	obs_enter_graphics();
	gs_texture_destroy(o->tex);
	obs_leave_graphics();
	pthread_mutex_destroy(&o->mutex);
	bfree(o->mask_rgba);
	bfree(o);
}

#define MASK_ALPHA 110

void sidestep_overlay_set_mask(struct sidestep_overlay *o, const uint8_t *mask, uint32_t w, uint32_t h)
{
	if (!o || !mask || !w || !h)
		return;

	pthread_mutex_lock(&o->mutex);
	if (o->mask_w != w || o->mask_h != h) {
		bfree(o->mask_rgba);
		o->mask_rgba = bmalloc((size_t)w * h * 4);
		o->mask_w = w;
		o->mask_h = h;
	}
	uint8_t *p = o->mask_rgba;
	for (size_t i = 0, n = (size_t)w * h; i < n; i++, p += 4) {
		p[0] = 255;
		p[1] = 0;
		p[2] = 0;
		p[3] = mask[i] ? MASK_ALPHA : 0;
	}
	o->mask_dirty = true;
	o->has_mask = true;
	pthread_mutex_unlock(&o->mutex);
}

void sidestep_overlay_set_rects(struct sidestep_overlay *o, const struct sidestep_overlay_rect *rects, int n)
{
	if (!o)
		return;
	if (n < 0)
		n = 0;
	if (n > SIDESTEP_OVERLAY_MAX_RECTS)
		n = SIDESTEP_OVERLAY_MAX_RECTS;

	pthread_mutex_lock(&o->mutex);
	if (n)
		memcpy(o->rects, rects, sizeof(*rects) * (size_t)n);
	o->n_rects = n;
	pthread_mutex_unlock(&o->mutex);
}

void sidestep_overlay_set_status(struct sidestep_overlay *o, bool unsettled, bool paused)
{
	if (!o)
		return;
	pthread_mutex_lock(&o->mutex);
	o->unsettled = unsettled;
	o->paused = paused;
	pthread_mutex_unlock(&o->mutex);
}

void sidestep_overlay_clear(struct sidestep_overlay *o)
{
	if (!o)
		return;
	pthread_mutex_lock(&o->mutex);
	o->has_mask = false;
	o->mask_dirty = false;
	o->n_rects = 0;
	o->unsettled = false;
	o->paused = false;
	pthread_mutex_unlock(&o->mutex);
}

/* ------------------------------------------------------------------------- */
/* Drawing primitives                                                        */

struct draw_ctx {
	gs_eparam_t *color;
	float cx, cy; /* clip bounds */
};

static void fill_rect(struct draw_ctx *d, float x, float y, float w, float h, uint32_t argb)
{
	float x0 = fmaxf(x, 0.0f), y0 = fmaxf(y, 0.0f);
	float x1 = fminf(x + w, d->cx), y1 = fminf(y + h, d->cy);
	if (x1 <= x0 || y1 <= y0)
		return;

	struct vec4 c;
	vec4_from_rgba(&c, ((argb >> 16) & 0xff) | (argb & 0xff00) | ((argb & 0xff) << 16) | (argb & 0xff000000));
	gs_effect_set_vec4(d->color, &c);

	gs_matrix_push();
	gs_matrix_translate3f(x0, y0, 0.0f);
	gs_matrix_scale3f(x1 - x0, y1 - y0, 1.0f);
	gs_draw_sprite(NULL, 0, 1, 1);
	gs_matrix_pop();
}

static void outline_rect(struct draw_ctx *d, float x, float y, float w, float h, float t, uint32_t argb)
{
	fill_rect(d, x, y, w, t, argb);
	fill_rect(d, x, y + h - t, w, t, argb);
	fill_rect(d, x, y + t, t, h - 2.0f * t, argb);
	fill_rect(d, x + w - t, y + t, t, h - 2.0f * t, argb);
}

/* 3x5 glyphs, one row per entry, bit 2 = left column. */
struct glyph {
	char ch;
	uint8_t rows[5];
};

static const struct glyph font[] = {
	{'0', {7, 5, 5, 5, 7}}, {'1', {2, 6, 2, 2, 7}}, {'2', {7, 1, 7, 4, 7}}, {'3', {7, 1, 7, 1, 7}},
	{'4', {5, 5, 7, 1, 1}}, {'5', {7, 4, 7, 1, 7}}, {'6', {7, 4, 7, 5, 7}}, {'7', {7, 1, 1, 1, 1}},
	{'8', {7, 5, 7, 5, 7}}, {'9', {7, 5, 7, 1, 7}}, {'.', {0, 0, 0, 0, 2}}, {'%', {5, 1, 2, 4, 5}},
	{'A', {2, 5, 7, 5, 5}}, {'D', {6, 5, 5, 5, 6}}, {'E', {7, 4, 6, 4, 7}}, {'L', {4, 4, 4, 4, 7}},
	{'N', {6, 5, 5, 5, 5}}, {'P', {6, 5, 6, 4, 4}}, {'S', {3, 4, 2, 1, 6}}, {'T', {7, 2, 2, 2, 2}},
	{'U', {5, 5, 5, 5, 7}},
};

static const uint8_t *find_glyph(char ch)
{
	for (size_t i = 0; i < sizeof(font) / sizeof(font[0]); i++) {
		if (font[i].ch == ch)
			return font[i].rows;
	}
	return NULL; /* space / unknown: blank */
}

static float text_width(const char *s, float px)
{
	size_t n = strlen(s);
	return n ? (float)(n * 4 - 1) * px : 0.0f;
}

/* Text with a dark backing box; (x, y) is the box's top-left. */
static void draw_text(struct draw_ctx *d, float x, float y, float px, const char *s, uint32_t argb)
{
	fill_rect(d, x, y, text_width(s, px) + 2.0f * px, 7.0f * px, 0xB0000000);

	float gx = x + px;
	for (; *s; s++, gx += 4.0f * px) {
		const uint8_t *rows = find_glyph(*s);
		if (!rows)
			continue;
		for (int r = 0; r < 5; r++) {
			/* merge horizontal runs into single quads */
			int c = 0;
			while (c < 3) {
				if (!(rows[r] & (4 >> c))) {
					c++;
					continue;
				}
				int start = c;
				while (c < 3 && (rows[r] & (4 >> c)))
					c++;
				fill_rect(d, gx + (float)start * px, y + (float)(r + 1) * px, (float)(c - start) * px, px,
					  argb);
			}
		}
	}
}

/* ------------------------------------------------------------------------- */

static void draw_mask(struct sidestep_overlay *o, uint32_t cx, uint32_t cy)
{
	if (!o->tex)
		return;

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), o->tex);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(o->tex, 0, cx, cy);
}

void sidestep_overlay_render(struct sidestep_overlay *o, uint32_t cx, uint32_t cy)
{
	if (!o || !cx || !cy)
		return;

	struct sidestep_overlay_rect rects[SIDESTEP_OVERLAY_MAX_RECTS];
	int n;
	bool unsettled, paused, has_mask;

	pthread_mutex_lock(&o->mutex);
	if (o->mask_dirty) {
		if (!o->tex || o->tex_w != o->mask_w || o->tex_h != o->mask_h) {
			gs_texture_destroy(o->tex);
			o->tex = gs_texture_create(o->mask_w, o->mask_h, GS_RGBA, 1, NULL, GS_DYNAMIC);
			o->tex_w = o->tex ? o->mask_w : 0;
			o->tex_h = o->tex ? o->mask_h : 0;
		}
		if (o->tex)
			gs_texture_set_image(o->tex, o->mask_rgba, o->mask_w * 4, false);
		o->mask_dirty = false;
	}
	has_mask = o->has_mask;
	n = o->n_rects;
	memcpy(rects, o->rects, sizeof(rects[0]) * (size_t)n);
	unsettled = o->unsettled;
	paused = o->paused;
	pthread_mutex_unlock(&o->mutex);

	const bool previous_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	if (has_mask)
		draw_mask(o, cx, cy);

	/* Line width and font pixel scale relative to a 1080p-ish source. */
	const float t = fmaxf(2.0f, (float)cx / 640.0f);
	const float px = fmaxf(2.0f, roundf((float)cx / 480.0f));

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	struct draw_ctx d = {
		.color = gs_effect_get_param_by_name(solid, "color"),
		.cx = (float)cx,
		.cy = (float)cy,
	};

	while (gs_effect_loop(solid, "Solid")) {
		char label[32];

		for (int i = 0; i < n; i++) {
			const struct sidestep_overlay_rect *r = &rects[i];
			const uint32_t col = r->suitable ? 0xFF20E040 : 0xFFFF3030;

			if (r->chosen) {
				fill_rect(&d, r->x, r->y, r->w, r->h, 0x30000000 | (col & 0x00FFFFFF));
				outline_rect(&d, r->x, r->y, r->w, r->h, 2.0f * t, 0xFFFFE020);
			}
			outline_rect(&d, r->x, r->y, r->w, r->h, t, col);

			snprintf(label, sizeof(label), "%.1f%%", (double)r->score * 100.0);
			draw_text(&d, r->x + 2.0f * t, r->y + 2.0f * t, px, label, col);
		}

		float sy = 2.0f * px;
		if (paused) {
			draw_text(&d, 2.0f * px, sy, 2.0f * px, "PAUSED", 0xFFFFE020);
			sy += 16.0f * px;
		}
		if (unsettled)
			draw_text(&d, 2.0f * px, sy, 2.0f * px, "UNSETTLED", 0xFFFFA020);
	}

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous_srgb);
}
