/*
 * obs-sidestep — the "Sidestep" video filter.
 *
 * Added to the content source. Periodically captures a downscaled copy of
 * the content (sidestep-capture), scores candidate camera positions
 * (core/busy, core/place) and moves / hides / shows the chosen camera scene
 * item in the chosen scene.
 *
 * Threads: video_tick, video_render and (deferred) update all run on the
 * graphics thread. Properties callbacks run on the UI thread, hotkeys on
 * the hotkey thread (they only set atomic request flags) and filter_remove
 * on whichever thread removes the filter; f->lock serialises tick, update
 * and filter_remove.
 *
 * SPDX-License-Identifier: MIT
 */
#include <obs-module.h>
#include <graphics/matrix4.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>

#include <math.h>
#include <string.h>

#include "busy.h"
#include "place.h"
#include "strategy.h"
#include "sidestep-capture.h"
#include "sidestep-overlay.h"

/* settings keys */
#define S_WARNING "warning"
#define S_GROUP_CAMERA "group_camera"
#define S_SCENE "scene"
#define S_CAMERA_ID "camera_id"
#define S_CAMERA_NAME "camera_name"
#define S_GROUP_PLACEMENT "group_placement"
#define S_MODE "mode"
#define S_CHOICE_FMT "choice%d"
#define S_CAPTURE_POS "capture_pos"
#define S_HAS_CAPTURED "has_captured"
#define S_CAPTURED_X "captured_x"
#define S_CAPTURED_Y "captured_y"
#define S_MARGIN "margin"
#define S_RETURN_HOME "return_home"
#define S_FALLBACK "fallback"
#define S_HELP_TRANSITION "help_transition"
#define S_GROUP_DETECTION "group_detection"
#define S_STRATEGY "strategy"
#define S_INTERVAL "interval"
#define S_SENSITIVITY "sensitivity"
#define S_USE_MOTION "use_motion"
#define S_IGNORE_TEMPLATE "ignore_template"
#define S_DEBUG "debug_overlay"
#define S_GROUP_ADVANCED "group_advanced"
#define S_CONFIRM "confirm_checks"
#define S_RETURN_DELAY "return_delay"
#define S_ANIM_MS "animation_ms"
#define S_ANALYSIS_RES "analysis_res"
#define S_IMAGE_COVERAGE "image_coverage"
#define S_RESTORE "restore_when_disabled"

#define T_(s) obs_module_text(s)

#define NS_PER_SEC 1000000000ULL
#define SETTLE_RECHECK_NS 400000000ULL /* fixed, not exposed */
#define UNSETTLED_G 0.25
#define CAPTURE_TIMEOUT_NS (2 * NS_PER_SEC)

enum sidestep_status {
	ST_OK = 0,
	ST_NO_SCENE,
	ST_SCENE_MISSING,
	ST_NO_CAMERA,
	ST_CAMERA_MISSING,
	ST_CONTENT_MISSING,
	ST_SAME_ITEM,
	ST_ROTATED,
};

static const char *status_text_key[] = {
	[ST_OK] = NULL,
	[ST_NO_SCENE] = "Warning.NoScene",
	[ST_SCENE_MISSING] = "Warning.SceneMissing",
	[ST_NO_CAMERA] = "Warning.NoCamera",
	[ST_CAMERA_MISSING] = "Warning.CameraMissing",
	[ST_CONTENT_MISSING] = "Warning.ContentMissing",
	[ST_SAME_ITEM] = "Warning.SameItem",
	[ST_ROTATED] = "Warning.Rotated",
};

/* Strong refs for the duration of one call; always targets_release(). */
struct targets {
	obs_source_t *scene_src;
	obs_scene_t *scene;
	obs_sceneitem_t *cam;
	obs_sceneitem_t *content;
};

struct anim {
	bool active;
	uint64_t start_ns, dur_ns;
	double fx, fy; /* box top-left at start */
	double tx, ty; /* box top-left target */
	double ox, oy; /* item pos minus box top-left, fixed for the move */
};

/* Canvas <-> filter-input pixel mapping for the content scene item. */
struct content_map {
	struct matrix4 inv;           /* canvas -> item texture (cropped) space */
	double off_x, off_y;          /* texture origin in parent source pixels */
	double vis_x0, vis_y0;        /* visible (uncropped) region, parent px */
	double vis_x1, vis_y1;
	double sx, sy;                /* parent px -> filter input px */
	struct matrix4 fwd;           /* item texture space -> canvas */
};

struct score_ctx {
	const struct content_map *map;
	const struct busy_analyzer *busy;
	double ax, ay; /* filter input px -> analysis px */
};

struct sidestep_filter {
	obs_source_t *context;
	pthread_mutex_t lock;

	/* settings (written by update) */
	char *scene_name;
	char *camera_name;
	int64_t camera_id;
	struct place_config pcfg;
	struct busy_params bparams;
	uint64_t interval_ns;
	uint64_t anim_ns;
	int analysis_w; /* 160 / 320 / 480, 16:9 box */
	bool debug;
	bool restore;

	/* runtime */
	obs_weak_source_t *scene_weak;
	struct place_state pstate;
	struct busy_analyzer *busy;
	struct sidestep_capture *capture;
	struct sidestep_overlay *overlay;
	uint8_t *frame;
	size_t frame_size;

	uint32_t aw, ah;   /* analysis size requested from render */
	bool want_capture; /* tick -> render, both on the graphics thread */
	bool awaiting;
	uint64_t await_since;
	uint64_t next_check_ns;
	uint64_t unsettled_since; /* 0 = settled */
	bool new_content;         /* next settled decision follows a change */
	struct anim anim;
	bool was_enabled;
	bool paused;
	enum sidestep_status status;

	/* hotkeys: set on the hotkey thread, consumed by tick */
	obs_hotkey_id hk_pause, hk_hide, hk_show;
	volatile bool req_pause_toggle;
	volatile bool req_hide;
	volatile bool req_show;
};

/* ------------------------------------------------------------------------- */
/* Scene item helpers                                                        */

static void targets_release(struct targets *t)
{
	obs_sceneitem_release(t->cam);
	obs_sceneitem_release(t->content);
	obs_source_release(t->scene_src);
	memset(t, 0, sizeof(*t));
}

static bool item_rotated(obs_sceneitem_t *item)
{
	double r = fmod(fabs((double)obs_sceneitem_get_rot(item)), 360.0);
	return r > 0.01 && r < 359.99;
}

struct find_source_data {
	obs_source_t *source;
	obs_sceneitem_t *item;
};

static bool find_source_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	struct find_source_data *d = param;
	UNUSED_PARAMETER(scene);
	if (obs_sceneitem_get_source(item) == d->source) {
		d->item = item;
		obs_sceneitem_addref(item);
		return false;
	}
	return true;
}

/* Find the camera (by id, then by source name) and the item showing
 * `parent` (top level only) in `scene`. Refs in *t; *found_id updated. */
static enum sidestep_status resolve_items(obs_scene_t *scene, int64_t cam_id, const char *cam_name,
					  obs_source_t *parent, int64_t *found_id, struct targets *t)
{
	obs_sceneitem_t *cam = cam_id ? obs_scene_find_sceneitem_by_id(scene, cam_id) : NULL;
	if (!cam && cam_name && *cam_name)
		cam = obs_scene_find_source(scene, cam_name);
	if (!cam)
		return (cam_id || (cam_name && *cam_name)) ? ST_CAMERA_MISSING : ST_NO_CAMERA;

	obs_sceneitem_addref(cam);
	t->cam = cam;
	if (found_id)
		*found_id = obs_sceneitem_get_id(cam);

	if (!parent)
		return ST_CONTENT_MISSING;

	struct find_source_data d = {.source = parent};
	obs_scene_enum_items(scene, find_source_cb, &d);
	t->content = d.item;
	if (!t->content)
		return ST_CONTENT_MISSING;
	if (t->content == t->cam)
		return ST_SAME_ITEM;
	if (item_rotated(t->cam) || item_rotated(t->content))
		return ST_ROTATED;
	return ST_OK;
}

/* Scene by name: fills t->scene_src / t->scene. */
static bool resolve_scene_by_name(const char *name, struct targets *t)
{
	if (!name || !*name)
		return false;
	obs_source_t *src = obs_get_source_by_name(name);
	if (src && (obs_source_removed(src) || !obs_scene_from_source(src))) {
		obs_source_release(src);
		src = NULL;
	}
	t->scene_src = src;
	t->scene = src ? obs_scene_from_source(src) : NULL;
	return t->scene != NULL;
}

/* Tick-side resolution (lock held). Uses the weak scene ref so renames
 * keep working; falls back to the stored name. */
static enum sidestep_status resolve_targets(struct sidestep_filter *f, struct targets *t)
{
	memset(t, 0, sizeof(*t));

	if (!f->scene_name || !*f->scene_name)
		return ST_NO_SCENE;

	obs_source_t *src = f->scene_weak ? obs_weak_source_get_source(f->scene_weak) : NULL;
	if (src && (obs_source_removed(src) || !obs_scene_from_source(src))) {
		obs_source_release(src);
		src = NULL;
	}
	if (src) {
		t->scene_src = src;
		t->scene = obs_scene_from_source(src);

		/* Keep the stored name in sync so a renamed scene is still
		 * found after a reload. */
		const char *name = obs_source_get_name(src);
		if (name && strcmp(name, f->scene_name) != 0) {
			obs_data_t *settings = obs_source_get_settings(f->context);
			bfree(f->scene_name);
			f->scene_name = bstrdup(name);
			obs_data_set_string(settings, S_SCENE, name);
			obs_data_release(settings);
		}
	} else {
		if (!resolve_scene_by_name(f->scene_name, t))
			return ST_SCENE_MISSING;
		obs_weak_source_release(f->scene_weak);
		f->scene_weak = obs_source_get_weak_source(t->scene_src);
	}

	int64_t id = f->camera_id;
	enum sidestep_status st =
		resolve_items(t->scene, f->camera_id, f->camera_name, obs_filter_get_parent(f->context), &id, t);

	/* Keep the stored identity in sync (found by name after a load, or
	 * the camera source was renamed). */
	if (t->cam) {
		const char *name = obs_source_get_name(obs_sceneitem_get_source(t->cam));
		bool name_changed = name && (!f->camera_name || strcmp(name, f->camera_name) != 0);
		if (id != f->camera_id || name_changed) {
			obs_data_t *settings = obs_source_get_settings(f->context);
			f->camera_id = id;
			obs_data_set_int(settings, S_CAMERA_ID, id);
			if (name_changed) {
				bfree(f->camera_name);
				f->camera_name = bstrdup(name);
				obs_data_set_string(settings, S_CAMERA_NAME, name);
			}
			obs_data_release(settings);
		}
	}
	return st;
}

/* Bounding box of an item on the canvas (top-left, size). */
static void item_box(obs_sceneitem_t *item, double *x, double *y, double *w, double *h)
{
	struct matrix4 m;
	struct vec3 a, b;

	obs_sceneitem_get_box_transform(item, &m);
	vec3_set(&a, 0.0f, 0.0f, 0.0f);
	vec3_set(&b, 1.0f, 1.0f, 0.0f);
	vec3_transform(&a, &a, &m);
	vec3_transform(&b, &b, &m);

	*x = fmin(a.x, b.x);
	*y = fmin(a.y, b.y);
	*w = fabs((double)b.x - a.x);
	*h = fabs((double)b.y - a.y);
}

/* Move an item so its bounding box's top-left lands at (x, y). Works in
 * deltas so alignment / bounds / flips don't matter. */
static void item_set_box_tl(obs_sceneitem_t *item, double x, double y)
{
	double bx, by, bw, bh;
	struct vec2 pos;

	item_box(item, &bx, &by, &bw, &bh);
	obs_sceneitem_get_pos(item, &pos);
	pos.x += (float)(x - bx);
	pos.y += (float)(y - by);
	obs_sceneitem_set_pos(item, &pos);
}

static void canvas_size(double *w, double *h)
{
	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi)) {
		*w = (double)ovi.base_width;
		*h = (double)ovi.base_height;
	} else {
		*w = 1920.0;
		*h = 1080.0;
	}
}

/* ------------------------------------------------------------------------- */
/* Content transform mapping                                                 */

/* Rounded half of an overflow split by bounds alignment (mirrors libobs's
 * crop-to-bounds calculation): source px cropped at the top/left. */
static double bounds_crop_tl(double overdraw, uint32_t align, uint32_t tl_flag, uint32_t br_flag, bool flipped)
{
	double tl;
	if (align & tl_flag)
		tl = 0.0;
	else if (align & br_flag)
		tl = overdraw;
	else
		tl = overdraw / 2.0;
	return round(flipped ? overdraw - tl : tl);
}

static bool content_map_build(struct content_map *m, obs_sceneitem_t *item, obs_source_t *parent,
			      obs_source_t *target)
{
	const double pw = (double)obs_source_get_width(parent);
	const double ph = (double)obs_source_get_height(parent);
	const double tw = (double)obs_source_get_base_width(target);
	const double th = (double)obs_source_get_base_height(target);
	if (pw < 1.0 || ph < 1.0 || tw < 1.0 || th < 1.0)
		return false;

	struct obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);
	obs_sceneitem_get_draw_transform(item, &m->fwd);
	if (!matrix4_inv(&m->inv, &m->fwd))
		return false;

	/* Extra crop applied by "crop to bounds" isn't exposed; derive it
	 * from the bounds box and the draw scale. */
	double bcl = 0.0, bct = 0.0, bcr = 0.0, bcb = 0.0;
	if (obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE && obs_sceneitem_get_bounds_crop(item)) {
		double bx, by, bw, bh;
		item_box(item, &bx, &by, &bw, &bh);
		const double scx = m->fwd.x.x, scy = m->fwd.y.y;
		const double cw = pw - crop.left - crop.right;
		const double ch = ph - crop.top - crop.bottom;
		const uint32_t align = obs_sceneitem_get_bounds_alignment(item);
		const double wd = fabs(cw * scx) - bw;
		const double hd = fabs(ch * scy) - bh;

		if (wd > 0.1 && fabs(scx) > 1e-9) {
			double over = wd / fabs(scx);
			bcl = bounds_crop_tl(over, align, OBS_ALIGN_LEFT, OBS_ALIGN_RIGHT, scx < 0.0);
			bcr = round(over - bcl);
		} else if (hd > 0.1 && fabs(scy) > 1e-9) {
			double over = hd / fabs(scy);
			bct = bounds_crop_tl(over, align, OBS_ALIGN_TOP, OBS_ALIGN_BOTTOM, scy < 0.0);
			bcb = round(over - bct);
		}
	}

	m->off_x = crop.left + bcl;
	m->off_y = crop.top + bct;
	m->vis_x0 = m->off_x;
	m->vis_y0 = m->off_y;
	m->vis_x1 = pw - crop.right - bcr;
	m->vis_y1 = ph - crop.bottom - bcb;
	m->sx = tw / pw;
	m->sy = th / ph;
	return m->vis_x1 > m->vis_x0 && m->vis_y1 > m->vis_y0;
}

/* Canvas rect -> filter input pixel rect (x0, y0, x1, y1). With `clip`,
 * limited to the part of the source that is actually visible. */
static bool map_canvas_rect(const struct content_map *m, double x, double y, double w, double h, bool clip,
			    double r[4])
{
	const double cxs[4] = {x, x + w, x, x + w};
	const double cys[4] = {y, y, y + h, y + h};
	double x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;

	for (int i = 0; i < 4; i++) {
		struct vec3 p;
		vec3_set(&p, (float)cxs[i], (float)cys[i], 0.0f);
		vec3_transform(&p, &p, &m->inv);
		x0 = fmin(x0, p.x);
		y0 = fmin(y0, p.y);
		x1 = fmax(x1, p.x);
		y1 = fmax(y1, p.y);
	}

	x0 += m->off_x;
	x1 += m->off_x;
	y0 += m->off_y;
	y1 += m->off_y;

	if (clip) {
		x0 = fmax(x0, m->vis_x0);
		y0 = fmax(y0, m->vis_y0);
		x1 = fmin(x1, m->vis_x1);
		y1 = fmin(y1, m->vis_y1);
		if (x1 <= x0 || y1 <= y0)
			return false;
	}

	r[0] = x0 * m->sx;
	r[1] = y0 * m->sy;
	r[2] = x1 * m->sx;
	r[3] = y1 * m->sy;
	return true;
}

static double rounded_area(double x0, double y0, double x1, double y1)
{
	return (ceil(x1) - floor(x0)) * (ceil(y1) - floor(y0));
}

typedef double (*analysis_rect_fn)(const struct busy_analyzer *b, double x, double y, double w, double h);

/* Mean of an analysis-space rect measure over a canvas rect. Canvas
 * outside the visible content counts as 0 (empty / not image). */
static double mapped_rect(const struct score_ctx *s, analysis_rect_fn fn, double x, double y, double w, double h)
{
	double full[4], vis[4];

	if (w <= 0.0 || h <= 0.0)
		return 0.0;
	if (!map_canvas_rect(s->map, x, y, w, h, false, full))
		return 0.0;
	if (!map_canvas_rect(s->map, x, y, w, h, true, vis))
		return 0.0;

	const double a_full = rounded_area(full[0] * s->ax, full[1] * s->ay, full[2] * s->ax, full[3] * s->ay);
	if (a_full <= 0.0)
		return 0.0;

	const double vx0 = vis[0] * s->ax, vy0 = vis[1] * s->ay;
	const double vx1 = vis[2] * s->ax, vy1 = vis[3] * s->ay;
	const double sc = fn(s->busy, vx0, vy0, vx1 - vx0, vy1 - vy0);
	const double v = sc * rounded_area(vx0, vy0, vx1, vy1) / a_full;
	return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

/* place_score_fn: busy fraction of a canvas rect. */
static double score_rect(void *param, double x, double y, double w, double h)
{
	return mapped_rect(param, busy_rect_score, x, y, w, h);
}

/* place_score_fn: background-image fraction of a canvas rect. */
static double image_rect(void *param, double x, double y, double w, double h)
{
	return mapped_rect(param, busy_rect_image_frac, x, y, w, h);
}

/* ------------------------------------------------------------------------- */
/* Actions                                                                   */

static void anim_start(struct sidestep_filter *f, obs_sceneitem_t *cam, double x, double y, uint64_t now)
{
	double bx, by, bw, bh;
	struct vec2 pos;

	if (!f->anim_ns) {
		f->anim.active = false;
		item_set_box_tl(cam, x, y);
		return;
	}

	/* Fix the pos/box offset once: re-deriving it per frame could drift
	 * if libobs defers the transform update (e.g. during a UI drag). */
	item_box(cam, &bx, &by, &bw, &bh);
	obs_sceneitem_get_pos(cam, &pos);
	f->anim.ox = pos.x - bx;
	f->anim.oy = pos.y - by;
	f->anim.active = true;
	f->anim.start_ns = now;
	f->anim.dur_ns = f->anim_ns;
	f->anim.fx = bx;
	f->anim.fy = by;
	f->anim.tx = x;
	f->anim.ty = y;
}

static void anim_step(struct sidestep_filter *f, obs_sceneitem_t *cam, uint64_t now)
{
	struct anim *a = &f->anim;
	double t = a->dur_ns ? (double)(now - a->start_ns) / (double)a->dur_ns : 1.0;
	if (t >= 1.0) {
		t = 1.0;
		a->active = false;
	}
	const double e = place_ease_in_out(t);
	struct vec2 pos;
	pos.x = (float)(a->fx + (a->tx - a->fx) * e + a->ox);
	pos.y = (float)(a->fy + (a->ty - a->fy) * e + a->oy);
	obs_sceneitem_set_pos(cam, &pos);
}

/* Move the camera to its 1st choice and show it (lock held). */
static void restore_camera(struct sidestep_filter *f)
{
	struct targets t;
	resolve_targets(f, &t);
	f->anim.active = false;

	if (t.cam) {
		double cw, ch, bx, by, bw, bh, x, y;
		canvas_size(&cw, &ch);
		item_box(t.cam, &bx, &by, &bw, &bh);
		if (place_anchor_pos(f->pcfg.choices[0], &f->pcfg, cw, ch, bw, bh, &x, &y))
			item_set_box_tl(t.cam, x, y);
		obs_sceneitem_set_visible(t.cam, true);
	}
	targets_release(&t);
}

static void reset_runtime(struct sidestep_filter *f)
{
	place_state_reset(&f->pstate);
	if (f->busy)
		busy_reset_history(f->busy);
	f->anim.active = false;
	f->awaiting = false;
	f->want_capture = false;
	f->next_check_ns = 0;
	f->unsettled_since = 0;
	f->new_content = true;
	sidestep_overlay_clear(f->overlay);
}

/* ------------------------------------------------------------------------- */
/* Checks                                                                    */

static void fit_analysis(uint32_t tw, uint32_t th, int box_w, uint32_t *aw, uint32_t *ah)
{
	const uint32_t bw = (uint32_t)box_w;
	const uint32_t bh = (uint32_t)box_w * 9 / 16;

	if (!tw || !th) {
		*aw = *ah = 0;
		return;
	}
	if ((uint64_t)tw * bh >= (uint64_t)th * bw) {
		*aw = bw;
		*ah = (uint32_t)llround((double)bw * th / tw);
	} else {
		*ah = bh;
		*aw = (uint32_t)llround((double)bh * tw / th);
	}
	if (*aw < 2)
		*aw = 2;
	if (*ah < 2)
		*ah = 2;
}

static void push_report(struct sidestep_filter *f, const struct content_map *map, const struct place_decision *d)
{
	struct sidestep_overlay_rect rects[SIDESTEP_OVERLAY_MAX_RECTS];
	int n = 0;

	for (int i = 0; i < d->n_report && n < SIDESTEP_OVERLAY_MAX_RECTS; i++) {
		const struct place_report_item *it = &d->report[i];
		double r[4];
		if (!map_canvas_rect(map, it->x, it->y, it->w, it->h, false, r))
			continue;
		rects[n].x = (float)r[0];
		rects[n].y = (float)r[1];
		rects[n].w = (float)(r[2] - r[0]);
		rects[n].h = (float)(r[3] - r[1]);
		rects[n].score = (float)it->score;
		rects[n].suitable = it->suitable;
		rects[n].on_image = it->on_image;
		rects[n].chosen = (i == d->chosen);
		n++;
	}
	sidestep_overlay_set_rects(f->overlay, rects, n);
}

/* Returns false if no decision could be made (nothing evaluated). */
static bool decide(struct sidestep_filter *f, struct targets *t, uint64_t now, bool new_content)
{
	obs_source_t *parent = obs_filter_get_parent(f->context);
	obs_source_t *target = obs_filter_get_target(f->context);
	struct content_map map;

	if (!parent || !target || !content_map_build(&map, t->content, parent, target))
		return false;

	struct score_ctx sc = {
		.map = &map,
		.busy = f->busy,
		.ax = (double)f->busy->w / (double)obs_source_get_base_width(target),
		.ay = (double)f->busy->h / (double)obs_source_get_base_height(target),
	};

	struct place_input in = {0};
	double bx, by;
	in.now_ns = now;
	canvas_size(&in.canvas_w, &in.canvas_h);
	item_box(t->cam, &bx, &by, &in.cam_w, &in.cam_h);
	in.cam_x = f->anim.active ? f->anim.tx : bx;
	in.cam_y = f->anim.active ? f->anim.ty : by;
	in.cam_visible = obs_sceneitem_visible(t->cam);
	in.score = score_rect;
	in.score_ctx = &sc;
	in.image_frac = image_rect;
	in.new_content = new_content;

	if (in.cam_w < 1.0 || in.cam_h < 1.0)
		return false;

	struct place_decision d = place_evaluate(&f->pstate, &f->pcfg, &in);

	switch (d.action) {
	case PLACE_ACTION_MOVE:
		anim_start(f, t->cam, d.x, d.y, now);
		break;
	case PLACE_ACTION_HIDE:
		obs_sceneitem_set_visible(t->cam, false);
		break;
	case PLACE_ACTION_SHOW:
		f->anim.active = false;
		item_set_box_tl(t->cam, d.x, d.y);
		obs_sceneitem_set_visible(t->cam, true);
		break;
	case PLACE_ACTION_NONE:
		break;
	}

	if (f->debug)
		push_report(f, &map, &d);
	return true;
}

/* A captured frame arrived: analyse, apply the transition guard, decide. */
static void process_frame(struct sidestep_filter *f, struct targets *t, uint32_t w, uint32_t h, uint64_t ts,
			  uint64_t now)
{
	if (f->busy && (f->busy->w != w || f->busy->h != h)) {
		busy_destroy(f->busy);
		f->busy = NULL;
	}
	if (!f->busy)
		f->busy = busy_create(w, h);
	if (!f->busy)
		return;

	f->bparams.now_ns = ts;
	busy_process(f->busy, f->frame, w * 4, BUSY_FMT_RGBA, &f->bparams);

	if (f->debug)
		sidestep_overlay_set_maps(f->overlay, f->busy->cost, f->busy->has_image ? f->busy->image : NULL, w,
					  h);

	const uint64_t recheck = f->interval_ns < SETTLE_RECHECK_NS ? f->interval_ns : SETTLE_RECHECK_NS;

	bool new_content = false;
	if (f->busy->global_change > UNSETTLED_G) {
		/* Mid-transition: recheck soon instead of deciding, but never
		 * stay undecided for longer than one regular interval. The
		 * first settled decision afterwards sees new content (a
		 * forced decision mid-animation doesn't). */
		f->new_content = true;
		if (!f->unsettled_since)
			f->unsettled_since = ts;
		f->next_check_ns = now + recheck;
		if (ts - f->unsettled_since < f->interval_ns) {
			sidestep_overlay_set_status(f->overlay, true, false);
			return;
		}
		f->unsettled_since = ts; /* forced decision; new window */
		sidestep_overlay_set_status(f->overlay, true, false);
	} else {
		f->unsettled_since = 0;
		new_content = f->new_content;
		f->new_content = false;
		f->next_check_ns = ts + f->interval_ns;
		if (f->next_check_ns < now)
			f->next_check_ns = now;
		sidestep_overlay_set_status(f->overlay, false, false);
	}

	if (!decide(f, t, now, new_content) && new_content)
		f->new_content = true; /* keep it for the next decision */
}

static void handle_hotkeys(struct sidestep_filter *f, obs_sceneitem_t *cam)
{
	if (os_atomic_exchange_bool(&f->req_pause_toggle, false)) {
		f->paused = !f->paused;
		if (!f->paused)
			reset_runtime(f);
	}
	if (os_atomic_exchange_bool(&f->req_hide, false)) {
		f->paused = true;
		f->anim.active = false;
		if (cam)
			obs_sceneitem_set_visible(cam, false);
	}
	if (os_atomic_exchange_bool(&f->req_show, false)) {
		if (cam)
			obs_sceneitem_set_visible(cam, true);
		f->paused = false;
		reset_runtime(f);
	}
}

static void sidestep_tick(void *data, float seconds)
{
	struct sidestep_filter *f = data;
	const uint64_t now = os_gettime_ns();
	struct targets t;
	UNUSED_PARAMETER(seconds);

	pthread_mutex_lock(&f->lock);

	if (!obs_source_enabled(f->context)) {
		if (f->was_enabled) {
			f->was_enabled = false;
			if (f->restore)
				restore_camera(f);
			reset_runtime(f);
		}
		pthread_mutex_unlock(&f->lock);
		return;
	}
	if (!f->was_enabled) {
		f->was_enabled = true;
		reset_runtime(f);
	}

	f->status = resolve_targets(f, &t);
	handle_hotkeys(f, t.cam);

	if (f->status != ST_OK) {
		f->anim.active = false;
		f->awaiting = false;
		goto done;
	}

	if (f->anim.active)
		anim_step(f, t.cam, now);

	if (f->paused) {
		if (f->debug)
			sidestep_overlay_set_status(f->overlay, false, true);
		goto done;
	}

	/* Pause when the scene is in neither program nor preview. */
	if (!obs_source_showing(t.scene_src) && !obs_source_active(t.scene_src)) {
		f->awaiting = false;
		f->want_capture = false;
		goto done;
	}

	obs_source_t *target = obs_filter_get_target(f->context);
	if (target)
		fit_analysis(obs_source_get_base_width(target), obs_source_get_base_height(target), f->analysis_w,
			     &f->aw, &f->ah);

	if (!f->awaiting && now >= f->next_check_ns && f->aw && f->ah) {
		f->want_capture = true;
		f->awaiting = true;
		f->await_since = now;
	} else if (f->awaiting && now - f->await_since > CAPTURE_TIMEOUT_NS) {
		/* content not rendered (hidden?) — retry later */
		f->awaiting = false;
		f->want_capture = false;
	}

	uint32_t w, h;
	uint64_t ts;
	if (sidestep_capture_take(f->capture, &f->frame, &f->frame_size, &w, &h, &ts)) {
		f->awaiting = false;
		process_frame(f, &t, w, h, ts, now);
	}

done:
	targets_release(&t);
	pthread_mutex_unlock(&f->lock);
}

/* ------------------------------------------------------------------------- */
/* Render                                                                    */

static void sidestep_render(void *data, gs_effect_t *effect)
{
	struct sidestep_filter *f = data;
	UNUSED_PARAMETER(effect);

	const bool want = f->want_capture;
	f->want_capture = false;

	sidestep_capture_render(f->capture, f->context, f->aw, f->ah, want);

	if (f->debug) {
		obs_source_t *target = obs_filter_get_target(f->context);
		if (target)
			sidestep_overlay_render(f->overlay, obs_source_get_base_width(target),
						obs_source_get_base_height(target));
	}
}

static enum gs_color_space sidestep_color_space(void *data, size_t count, const enum gs_color_space *preferred_spaces)
{
	struct sidestep_filter *f = data;
	obs_source_t *target = obs_filter_get_target(f->context);
	if (target)
		return obs_source_get_color_space(target, count, preferred_spaces);
	return count ? preferred_spaces[0] : GS_CS_SRGB;
}

/* ------------------------------------------------------------------------- */
/* Settings                                                                  */

static void sidestep_update(void *data, obs_data_t *settings)
{
	struct sidestep_filter *f = data;
	char key[16];

	pthread_mutex_lock(&f->lock);

	const char *scene = obs_data_get_string(settings, S_SCENE);
	if (!f->scene_name || strcmp(scene, f->scene_name) != 0) {
		bfree(f->scene_name);
		f->scene_name = bstrdup(scene);
		obs_weak_source_release(f->scene_weak);
		f->scene_weak = NULL; /* resolved lazily by name */
	}
	bfree(f->camera_name);
	f->camera_name = bstrdup(obs_data_get_string(settings, S_CAMERA_NAME));
	f->camera_id = obs_data_get_int(settings, S_CAMERA_ID);

	struct place_config *c = &f->pcfg;
	place_config_default(c);
	c->mode = (enum place_mode)obs_data_get_int(settings, S_MODE);
	for (int i = 0; i < PLACE_MAX_CHOICES; i++) {
		snprintf(key, sizeof(key), S_CHOICE_FMT, i + 1);
		long long a = obs_data_get_int(settings, key);
		c->choices[i] = (a > PLACE_ANCHOR_NONE && a < PLACE_ANCHOR_COUNT) ? (enum place_anchor)a
										  : PLACE_ANCHOR_NONE;
	}
	c->has_captured = obs_data_get_bool(settings, S_HAS_CAPTURED);
	c->captured_x = obs_data_get_double(settings, S_CAPTURED_X);
	c->captured_y = obs_data_get_double(settings, S_CAPTURED_Y);
	c->margin = (double)obs_data_get_int(settings, S_MARGIN);
	c->return_home = obs_data_get_bool(settings, S_RETURN_HOME);
	c->fallback = (enum place_fallback)obs_data_get_int(settings, S_FALLBACK);
	c->t_suitable = busy_sensitivity_to_threshold((double)obs_data_get_int(settings, S_SENSITIVITY));
	c->confirm_checks = (int)obs_data_get_int(settings, S_CONFIRM);
	if (c->confirm_checks < 1)
		c->confirm_checks = 1;
	if (c->confirm_checks > 5)
		c->confirm_checks = 5;
	c->return_delay_ns = (uint64_t)(fmax(obs_data_get_double(settings, S_RETURN_DELAY), 0.0) * NS_PER_SEC);

	/* Unknown ids (e.g. from a newer version) act as "Any content". */
	const struct busy_strategy_ops *strat = busy_strategy_find((int)obs_data_get_int(settings, S_STRATEGY));
	c->prefer_images = strat->prefers_images;
	c->t_image = fmin(fmax((double)obs_data_get_int(settings, S_IMAGE_COVERAGE), 1.0), 100.0) / 100.0;

	busy_params_default(&f->bparams);
	f->bparams.use_motion = obs_data_get_bool(settings, S_USE_MOTION);
	f->bparams.strategy = strat->id;
	f->bparams.ignore_template = obs_data_get_bool(settings, S_IGNORE_TEMPLATE);
	if (f->busy)
		busy_set_strategy(f->busy, strat->id); /* fresh state on change */

	double interval = obs_data_get_double(settings, S_INTERVAL);
	if (interval < 0.1)
		interval = 0.1;
	f->interval_ns = (uint64_t)(interval * NS_PER_SEC);
	f->anim_ns = (uint64_t)obs_data_get_int(settings, S_ANIM_MS) * 1000000ULL;

	int res = (int)obs_data_get_int(settings, S_ANALYSIS_RES);
	if (res != 160 && res != 320 && res != 480)
		res = 320;
	f->analysis_w = res;

	const bool debug = obs_data_get_bool(settings, S_DEBUG);
	if (!debug && f->debug)
		sidestep_overlay_clear(f->overlay);
	f->debug = debug;
	f->restore = obs_data_get_bool(settings, S_RESTORE);

	place_state_reset(&f->pstate);
	f->next_check_ns = 0;
	f->unsettled_since = 0;
	f->new_content = true;

	pthread_mutex_unlock(&f->lock);
}

static void sidestep_defaults(obs_data_t *settings)
{
	char key[16];
	const long long choices[PLACE_MAX_CHOICES] = {PLACE_ANCHOR_BOTTOM_RIGHT, PLACE_ANCHOR_TOP_RIGHT,
						      PLACE_ANCHOR_BOTTOM_LEFT, PLACE_ANCHOR_NONE};

	obs_data_set_default_string(settings, S_SCENE, "");
	obs_data_set_default_int(settings, S_CAMERA_ID, 0);
	obs_data_set_default_string(settings, S_CAMERA_NAME, "");
	obs_data_set_default_int(settings, S_MODE, PLACE_MODE_PREFERRED_THEN_ANYWHERE);
	for (int i = 0; i < PLACE_MAX_CHOICES; i++) {
		snprintf(key, sizeof(key), S_CHOICE_FMT, i + 1);
		obs_data_set_default_int(settings, key, choices[i]);
	}
	obs_data_set_default_bool(settings, S_HAS_CAPTURED, false);
	obs_data_set_default_int(settings, S_MARGIN, 24);
	obs_data_set_default_bool(settings, S_RETURN_HOME, true);
	obs_data_set_default_int(settings, S_FALLBACK, PLACE_FALLBACK_HIDE);
	obs_data_set_default_int(settings, S_STRATEGY, BUSY_STRATEGY_ANY);
	obs_data_set_default_double(settings, S_INTERVAL, 1.0);
	obs_data_set_default_int(settings, S_SENSITIVITY, 50);
	obs_data_set_default_bool(settings, S_USE_MOTION, true);
	obs_data_set_default_bool(settings, S_IGNORE_TEMPLATE, false);
	obs_data_set_default_bool(settings, S_DEBUG, false);
	obs_data_set_default_int(settings, S_CONFIRM, 1);
	obs_data_set_default_double(settings, S_RETURN_DELAY, 3.0);
	obs_data_set_default_int(settings, S_ANIM_MS, 300);
	obs_data_set_default_int(settings, S_ANALYSIS_RES, 320);
	obs_data_set_default_int(settings, S_IMAGE_COVERAGE, 90);
	obs_data_set_default_bool(settings, S_RESTORE, true);
}

/* ------------------------------------------------------------------------- */
/* Properties                                                                */

static obs_properties_t *props_root(obs_properties_t *props)
{
	obs_properties_t *parent;
	while ((parent = obs_properties_get_parent(props)) != NULL)
		props = parent;
	return props;
}

/* UI-thread status check from the settings being edited. */
static enum sidestep_status settings_status(struct sidestep_filter *f, obs_data_t *settings)
{
	struct targets t = {0};
	enum sidestep_status st;
	const char *scene = obs_data_get_string(settings, S_SCENE);

	if (!*scene)
		return ST_NO_SCENE;
	if (!resolve_scene_by_name(scene, &t)) {
		targets_release(&t);
		return ST_SCENE_MISSING;
	}
	st = resolve_items(t.scene, obs_data_get_int(settings, S_CAMERA_ID),
			   obs_data_get_string(settings, S_CAMERA_NAME), obs_filter_get_parent(f->context), NULL, &t);
	targets_release(&t);
	return st;
}

static void refresh_warning(struct sidestep_filter *f, obs_properties_t *props, obs_data_t *settings)
{
	obs_property_t *w = obs_properties_get(props_root(props), S_WARNING);
	if (!w)
		return;
	enum sidestep_status st = settings_status(f, settings);
	if (st == ST_OK) {
		obs_property_set_visible(w, false);
	} else {
		obs_property_set_description(w, T_(status_text_key[st]));
		obs_property_set_visible(w, true);
	}
}

struct camera_list_data {
	obs_property_t *list;
	obs_source_t *exclude;
};

static bool add_camera_cb(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	struct camera_list_data *d = param;
	obs_source_t *src = obs_sceneitem_get_source(item);
	UNUSED_PARAMETER(scene);
	if (src && src != d->exclude)
		obs_property_list_add_int(d->list, obs_source_get_name(src), obs_sceneitem_get_id(item));
	return true;
}

static bool scene_modified(void *priv, obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	struct sidestep_filter *f = priv;
	obs_property_t *cam = obs_properties_get(props_root(props), S_CAMERA_ID);
	UNUSED_PARAMETER(p);

	if (cam) {
		struct targets t = {0};
		obs_property_list_clear(cam);
		obs_property_list_add_int(cam, T_("Camera.Select"), 0);
		if (resolve_scene_by_name(obs_data_get_string(settings, S_SCENE), &t)) {
			struct camera_list_data d = {.list = cam, .exclude = obs_filter_get_parent(f->context)};
			obs_scene_enum_items(t.scene, add_camera_cb, &d);
		}
		targets_release(&t);
	}
	refresh_warning(f, props, settings);
	return true;
}

static bool camera_modified(void *priv, obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	refresh_warning(priv, props, settings);
	return true;
}

static bool mode_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	obs_properties_t *root = props_root(props);
	const bool choices = obs_data_get_int(settings, S_MODE) != PLACE_MODE_ANYWHERE;
	char key[16];
	UNUSED_PARAMETER(p);

	for (int i = 0; i < PLACE_MAX_CHOICES; i++) {
		snprintf(key, sizeof(key), S_CHOICE_FMT, i + 1);
		obs_property_set_visible(obs_properties_get(root, key), choices);
	}
	obs_property_set_visible(obs_properties_get(root, S_CAPTURE_POS), choices);
	obs_property_set_visible(obs_properties_get(root, S_RETURN_HOME), choices);
	return true;
}

/* Show the selected strategy's description and its relevant options. */
static bool strategy_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	const struct busy_strategy_ops *s = busy_strategy_find((int)obs_data_get_int(settings, S_STRATEGY));
	obs_property_set_long_description(p, T_(s->desc_key));
	obs_property_set_visible(obs_properties_get(props_root(props), S_IMAGE_COVERAGE), s->prefers_images);
	return true;
}

static const char *anchor_text_key[PLACE_ANCHOR_COUNT] = {
	[PLACE_ANCHOR_NONE] = "Anchor.None",
	[PLACE_ANCHOR_TOP_LEFT] = "Anchor.TopLeft",
	[PLACE_ANCHOR_TOP_CENTER] = "Anchor.TopCenter",
	[PLACE_ANCHOR_TOP_RIGHT] = "Anchor.TopRight",
	[PLACE_ANCHOR_MIDDLE_LEFT] = "Anchor.MiddleLeft",
	[PLACE_ANCHOR_MIDDLE_CENTER] = "Anchor.MiddleCenter",
	[PLACE_ANCHOR_MIDDLE_RIGHT] = "Anchor.MiddleRight",
	[PLACE_ANCHOR_BOTTOM_LEFT] = "Anchor.BottomLeft",
	[PLACE_ANCHOR_BOTTOM_CENTER] = "Anchor.BottomCenter",
	[PLACE_ANCHOR_BOTTOM_RIGHT] = "Anchor.BottomRight",
	[PLACE_ANCHOR_CAPTURED] = "Anchor.Captured",
};

static bool list_has_int(obs_property_t *p, long long v)
{
	size_t n = obs_property_list_item_count(p);
	for (size_t i = 0; i < n; i++) {
		if (obs_property_list_item_int(p, i) == v)
			return true;
	}
	return false;
}

/* Insert "Captured" just before "(none)" (the last entry). */
static void add_captured_choice(obs_property_t *p)
{
	if (!p || list_has_int(p, PLACE_ANCHOR_CAPTURED))
		return;
	size_t n = obs_property_list_item_count(p);
	obs_property_list_insert_int(p, n ? n - 1 : 0, T_(anchor_text_key[PLACE_ANCHOR_CAPTURED]),
				     PLACE_ANCHOR_CAPTURED);
}

static bool capture_pos_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	struct sidestep_filter *f = data;
	obs_data_t *settings = obs_source_get_settings(f->context);
	struct targets t = {0};
	bool captured = false;
	UNUSED_PARAMETER(p);

	if (resolve_scene_by_name(obs_data_get_string(settings, S_SCENE), &t)) {
		resolve_items(t.scene, obs_data_get_int(settings, S_CAMERA_ID),
			      obs_data_get_string(settings, S_CAMERA_NAME), NULL, NULL, &t);
		if (t.cam) {
			double x, y, w, h;
			item_box(t.cam, &x, &y, &w, &h);
			obs_data_set_bool(settings, S_HAS_CAPTURED, true);
			obs_data_set_double(settings, S_CAPTURED_X, x);
			obs_data_set_double(settings, S_CAPTURED_Y, y);
			captured = true;
		}
	}
	targets_release(&t);

	if (captured) {
		obs_properties_t *root = props_root(props);
		char key[16];
		for (int i = 0; i < PLACE_MAX_CHOICES; i++) {
			snprintf(key, sizeof(key), S_CHOICE_FMT, i + 1);
			add_captured_choice(obs_properties_get(root, key));
		}
		obs_source_update(f->context, NULL);
	}
	obs_data_release(settings);
	return captured;
}

static bool add_scene_cb(void *param, obs_source_t *src)
{
	const char *name = obs_source_get_name(src);
	if (name)
		obs_property_list_add_string(param, name, name);
	return true;
}

static obs_property_t *add_choice_list(obs_properties_t *props, int idx, bool has_captured)
{
	char key[16], label[32];
	snprintf(key, sizeof(key), S_CHOICE_FMT, idx);
	snprintf(label, sizeof(label), "Choice%d", idx);

	obs_property_t *p = obs_properties_add_list(props, key, T_(label), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	for (int a = PLACE_ANCHOR_TOP_LEFT; a <= PLACE_ANCHOR_BOTTOM_RIGHT; a++)
		obs_property_list_add_int(p, T_(anchor_text_key[a]), a);
	if (has_captured)
		obs_property_list_add_int(p, T_(anchor_text_key[PLACE_ANCHOR_CAPTURED]), PLACE_ANCHOR_CAPTURED);
	obs_property_list_add_int(p, T_(anchor_text_key[PLACE_ANCHOR_NONE]), PLACE_ANCHOR_NONE);
	return p;
}

static obs_properties_t *sidestep_properties(void *data)
{
	struct sidestep_filter *f = data;
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	obs_data_t *settings = obs_source_get_settings(f->context);
	const bool has_captured = obs_data_get_bool(settings, S_HAS_CAPTURED);
	obs_data_release(settings);

	p = obs_properties_add_text(props, S_WARNING, "", OBS_TEXT_INFO);
	obs_property_text_set_info_type(p, OBS_TEXT_INFO_WARNING);
	obs_property_set_visible(p, false);

	/* Camera */
	obs_properties_t *cam = obs_properties_create();
	p = obs_properties_add_list(cam, S_SCENE, T_("Scene"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, T_("Scene.Select"), "");
	obs_enum_scenes(add_scene_cb, p);
	obs_property_set_modified_callback2(p, scene_modified, f);

	p = obs_properties_add_list(cam, S_CAMERA_ID, T_("Camera"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_set_long_description(p, T_("Camera.Description"));
	obs_property_set_modified_callback2(p, camera_modified, f);
	obs_properties_add_group(props, S_GROUP_CAMERA, T_("Group.Camera"), OBS_GROUP_NORMAL, cam);

	/* Placement */
	obs_properties_t *place = obs_properties_create();
	p = obs_properties_add_list(place, S_MODE, T_("Mode"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_("Mode.Preferred"), PLACE_MODE_PREFERRED);
	obs_property_list_add_int(p, T_("Mode.PreferredThenAnywhere"), PLACE_MODE_PREFERRED_THEN_ANYWHERE);
	obs_property_list_add_int(p, T_("Mode.Anywhere"), PLACE_MODE_ANYWHERE);
	obs_property_set_modified_callback(p, mode_modified);

	for (int i = 1; i <= PLACE_MAX_CHOICES; i++)
		add_choice_list(place, i, has_captured);

	obs_properties_add_button2(place, S_CAPTURE_POS, T_("CapturePosition"), capture_pos_clicked, f);

	p = obs_properties_add_int(place, S_MARGIN, T_("Margin"), 0, 1000, 1);
	obs_property_int_set_suffix(p, " px");
	obs_properties_add_bool(place, S_RETURN_HOME, T_("ReturnHome"));

	p = obs_properties_add_list(place, S_FALLBACK, T_("Fallback"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, T_("Fallback.Hide"), PLACE_FALLBACK_HIDE);
	obs_property_list_add_int(p, T_("Fallback.LeastBusy"), PLACE_FALLBACK_LEAST_BUSY);

	obs_properties_add_text(place, S_HELP_TRANSITION, T_("Help.Transition"), OBS_TEXT_INFO);
	obs_properties_add_group(props, S_GROUP_PLACEMENT, T_("Group.Placement"), OBS_GROUP_NORMAL, place);

	/* Detection */
	obs_properties_t *det = obs_properties_create();
	p = obs_properties_add_list(det, S_STRATEGY, T_("Strategy"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	for (size_t i = 0; i < busy_strategy_count(); i++) {
		const struct busy_strategy_ops *s = busy_strategy_get(i);
		obs_property_list_add_int(p, T_(s->name_key), s->id);
	}
	obs_property_set_modified_callback(p, strategy_modified);
	p = obs_properties_add_float_slider(det, S_INTERVAL, T_("Interval"), 0.1, 10.0, 0.1);
	obs_property_float_set_suffix(p, " s");
	p = obs_properties_add_int_slider(det, S_SENSITIVITY, T_("Sensitivity"), 0, 100, 1);
	obs_property_set_long_description(p, T_("Sensitivity.Description"));
	obs_properties_add_bool(det, S_USE_MOTION, T_("UseMotion"));
	p = obs_properties_add_bool(det, S_IGNORE_TEMPLATE, T_("IgnoreTemplate"));
	obs_property_set_long_description(p, T_("IgnoreTemplate.Description"));
	p = obs_properties_add_bool(det, S_DEBUG, T_("DebugOverlay"));
	obs_property_set_long_description(p, T_("DebugOverlay.Description"));
	obs_properties_add_group(props, S_GROUP_DETECTION, T_("Group.Detection"), OBS_GROUP_NORMAL, det);

	/* Advanced */
	obs_properties_t *adv = obs_properties_create();
	p = obs_properties_add_int(adv, S_CONFIRM, T_("ConfirmChecks"), 1, 5, 1);
	obs_property_set_long_description(p, T_("ConfirmChecks.Description"));
	p = obs_properties_add_float(adv, S_RETURN_DELAY, T_("ReturnDelay"), 0.0, 60.0, 0.5);
	obs_property_float_set_suffix(p, " s");
	p = obs_properties_add_int(adv, S_ANIM_MS, T_("Animation"), 0, 5000, 50);
	obs_property_int_set_suffix(p, " ms");
	p = obs_properties_add_list(adv, S_ANALYSIS_RES, T_("AnalysisResolution"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, "160×90", 160);
	obs_property_list_add_int(p, "320×180", 320);
	obs_property_list_add_int(p, "480×270", 480);
	p = obs_properties_add_int_slider(adv, S_IMAGE_COVERAGE, T_("ImageCoverage"), 50, 100, 1);
	obs_property_int_set_suffix(p, " %");
	obs_property_set_long_description(p, T_("ImageCoverage.Description"));
	obs_properties_add_bool(adv, S_RESTORE, T_("RestoreWhenDisabled"));
	obs_properties_add_group(props, S_GROUP_ADVANCED, T_("Group.Advanced"), OBS_GROUP_NORMAL, adv);

	return props;
}

/* ------------------------------------------------------------------------- */
/* Hotkeys                                                                   */

static void hk_pause(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	struct sidestep_filter *f = data;
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		os_atomic_set_bool(&f->req_pause_toggle, true);
}

static void hk_hide(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	struct sidestep_filter *f = data;
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		os_atomic_set_bool(&f->req_hide, true);
}

static void hk_show(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	struct sidestep_filter *f = data;
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		os_atomic_set_bool(&f->req_show, true);
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */

static const char *sidestep_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return T_("Sidestep");
}

static void *sidestep_create(obs_data_t *settings, obs_source_t *source)
{
	struct sidestep_filter *f = bzalloc(sizeof(*f));
	f->context = source;
	if (pthread_mutex_init(&f->lock, NULL) != 0) {
		bfree(f);
		return NULL;
	}
	f->capture = sidestep_capture_create();
	f->overlay = sidestep_overlay_create();
	place_state_reset(&f->pstate);

	/* Filter hotkeys are listed under the parent source in Settings. */
	f->hk_pause = obs_hotkey_register_source(source, "sidestep.pause", T_("Hotkey.Pause"), hk_pause, f);
	f->hk_hide = obs_hotkey_register_source(source, "sidestep.hide", T_("Hotkey.Hide"), hk_hide, f);
	f->hk_show = obs_hotkey_register_source(source, "sidestep.show", T_("Hotkey.Show"), hk_show, f);

	sidestep_update(f, settings);
	return f;
}

static void sidestep_destroy(void *data)
{
	struct sidestep_filter *f = data;

	obs_hotkey_unregister(f->hk_pause);
	obs_hotkey_unregister(f->hk_hide);
	obs_hotkey_unregister(f->hk_show);

	sidestep_capture_destroy(f->capture);
	sidestep_overlay_destroy(f->overlay);
	busy_destroy(f->busy);
	obs_weak_source_release(f->scene_weak);
	bfree(f->frame);
	bfree(f->scene_name);
	bfree(f->camera_name);
	pthread_mutex_destroy(&f->lock);
	bfree(f);
}

static void sidestep_filter_remove(void *data, obs_source_t *parent)
{
	struct sidestep_filter *f = data;
	UNUSED_PARAMETER(parent);

	pthread_mutex_lock(&f->lock);
	if (f->restore && f->was_enabled)
		restore_camera(f);
	f->was_enabled = false;
	pthread_mutex_unlock(&f->lock);
}

struct obs_source_info sidestep_filter_info = {
	.id = "sidestep_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = sidestep_get_name,
	.create = sidestep_create,
	.destroy = sidestep_destroy,
	.update = sidestep_update,
	.get_defaults = sidestep_defaults,
	.get_properties = sidestep_properties,
	.video_tick = sidestep_tick,
	.video_render = sidestep_render,
	.video_get_color_space = sidestep_color_space,
	.filter_remove = sidestep_filter_remove,
};
