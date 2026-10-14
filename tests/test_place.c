/*
 * obs-sidestep — placement state machine tests.
 *
 * Uses a fake score function over a synthetic busy map in canvas space:
 * either a list of busy rectangles, or (inverted) a fully busy canvas with
 * clear "holes". Score = busy fraction of the (inflated) rect. Optional
 * "photo" rects make up the image map (image fraction of the camera rect).
 *
 * SPDX-License-Identifier: MIT
 */
#include "place.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond)                                                       \
	do {                                                              \
		checks++;                                                 \
		if (!(cond)) {                                            \
			failures++;                                       \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n",      \
				__FILE__, __LINE__, #cond);               \
		}                                                         \
	} while (0)

#define CHECK_NEAR(a, b) CHECK(fabs((double)(a) - (double)(b)) < 1e-6)

#define CANVAS_W 1920.0
#define CANVAS_H 1080.0
#define CAM_W 320.0
#define CAM_H 180.0
#define MARGIN 24.0

/* Anchor positions for the defaults above. */
#define LEFT_X 24.0
#define RIGHT_X 1576.0
#define TOP_Y 24.0
#define BOTTOM_Y 876.0

#define SEC 1000000000ULL

/* ---- fake busy map ----------------------------------------------------- */

struct rect {
	double x, y, w, h;
};

struct busy_map {
	struct rect r[16];
	int n;
	bool inverted; /* whole canvas busy except r[] (disjoint holes) */
	struct rect img[4]; /* disjoint background-image rects */
	int n_img;
};

static double overlap(double x, double y, double w, double h,
		      const struct rect *r)
{
	double x0 = fmax(x, r->x), x1 = fmin(x + w, r->x + r->w);
	double y0 = fmax(y, r->y), y1 = fmin(y + h, r->y + r->h);
	if (x1 <= x0 || y1 <= y0)
		return 0.0;
	return (x1 - x0) * (y1 - y0);
}

static double fake_score(void *ctx, double x, double y, double w, double h)
{
	const struct busy_map *m = ctx;
	double area = w * h, busy = 0.0;
	if (area <= 0.0)
		return 0.0;
	if (m->inverted) {
		struct rect canvas = {0, 0, CANVAS_W, CANVAS_H};
		busy = overlap(x, y, w, h, &canvas);
	}
	for (int i = 0; i < m->n; i++) {
		double o = overlap(x, y, w, h, &m->r[i]);
		busy += m->inverted ? -o : o;
	}
	busy /= area;
	return busy < 0.0 ? 0.0 : (busy > 1.0 ? 1.0 : busy);
}

static double fake_image(void *ctx, double x, double y, double w, double h)
{
	const struct busy_map *m = ctx;
	double area = w * h, cov = 0.0;
	if (area <= 0.0)
		return 0.0;
	for (int i = 0; i < m->n_img; i++)
		cov += overlap(x, y, w, h, &m->img[i]);
	return cov / area;
}

/* Photo filling the left or right half of the canvas. */
static void map_photo(struct busy_map *m, bool right)
{
	m->img[m->n_img++] = (struct rect){right ? CANVAS_W / 2 : 0, 0,
					   CANVAS_W / 2, CANVAS_H};
}

static void map_add(struct busy_map *m, double x, double y, double w, double h)
{
	m->r[m->n++] = (struct rect){x, y, w, h};
}

/* Busy content right inside the given anchor's camera box. */
static void map_busy_at(struct busy_map *m, double ax, double ay)
{
	map_add(m, ax + 10, ay + 10, CAM_W - 20, CAM_H - 20);
}

/* ---- simulated camera -------------------------------------------------- */

struct sim {
	struct place_config cfg;
	struct place_state st;
	struct busy_map map;
	double x, y;
	bool visible;
	struct place_decision last;
};

static void sim_init(struct sim *s, double x, double y)
{
	memset(s, 0, sizeof(*s));
	place_config_default(&s->cfg);
	place_state_reset(&s->st);
	s->x = x;
	s->y = y;
	s->visible = true;
}

/* Run one check at time t (seconds, offset so t=0 is not ns 0) and apply
 * the decision to the simulated camera. */
static enum place_action sim_step_nc(struct sim *s, double t, bool new_content)
{
	struct place_input in = {
		.now_ns = 100 * SEC + (uint64_t)(t * (double)SEC),
		.canvas_w = CANVAS_W,
		.canvas_h = CANVAS_H,
		.cam_w = CAM_W,
		.cam_h = CAM_H,
		.cam_x = s->x,
		.cam_y = s->y,
		.cam_visible = s->visible,
		.score = fake_score,
		.score_ctx = &s->map,
		.image_frac = fake_image,
		.new_content = new_content,
	};
	s->last = place_evaluate(&s->st, &s->cfg, &in);
	switch (s->last.action) {
	case PLACE_ACTION_MOVE:
		s->x = s->last.x;
		s->y = s->last.y;
		break;
	case PLACE_ACTION_SHOW:
		s->x = s->last.x;
		s->y = s->last.y;
		s->visible = true;
		break;
	case PLACE_ACTION_HIDE:
		s->visible = false;
		break;
	default:
		break;
	}
	return s->last.action;
}

static enum place_action sim_step(struct sim *s, double t)
{
	return sim_step_nc(s, t, false);
}

/* Simulated slide with a text column beside the photo (or none). */
static void sim_slide(struct sim *s, int photo /* 0 none, 1 left, 2 right */)
{
	s->map.n = 0;
	s->map.n_img = 0;
	s->map.inverted = false;
	if (photo)
		map_photo(&s->map, photo == 2);
	/* text near the top of the other column */
	map_add(&s->map, photo == 1 ? 1000 : 60, 80, 800, 500);
}

static void sim_init_images(struct sim *s, double x, double y)
{
	sim_init(s, x, y);
	s->cfg.prefer_images = true;
	s->cfg.choices[0] = PLACE_ANCHOR_BOTTOM_RIGHT;
	s->cfg.choices[1] = PLACE_ANCHOR_BOTTOM_LEFT;
	s->cfg.choices[2] = PLACE_ANCHOR_NONE;
}

static bool at(const struct sim *s, double x, double y)
{
	return fabs(s->x - x) < 1e-6 && fabs(s->y - y) < 1e-6;
}

static bool within_margins(double x, double y)
{
	return x >= MARGIN - 1e-6 && y >= MARGIN - 1e-6 &&
	       x + CAM_W <= CANVAS_W - MARGIN + 1e-6 &&
	       y + CAM_H <= CANVAS_H - MARGIN + 1e-6;
}

/* ---- tests ------------------------------------------------------------- */

static void test_anchors(void)
{
	struct place_config c;
	place_config_default(&c);
	double x, y;

	CHECK(place_anchor_pos(PLACE_ANCHOR_TOP_LEFT, &c, CANVAS_W, CANVAS_H,
			       CAM_W, CAM_H, &x, &y));
	CHECK_NEAR(x, LEFT_X);
	CHECK_NEAR(y, TOP_Y);
	CHECK(place_anchor_pos(PLACE_ANCHOR_TOP_CENTER, &c, CANVAS_W, CANVAS_H,
			       CAM_W, CAM_H, &x, &y));
	CHECK_NEAR(x, 800);
	CHECK_NEAR(y, TOP_Y);
	CHECK(place_anchor_pos(PLACE_ANCHOR_MIDDLE_RIGHT, &c, CANVAS_W,
			       CANVAS_H, CAM_W, CAM_H, &x, &y));
	CHECK_NEAR(x, RIGHT_X);
	CHECK_NEAR(y, 450);
	CHECK(place_anchor_pos(PLACE_ANCHOR_MIDDLE_CENTER, &c, CANVAS_W,
			       CANVAS_H, CAM_W, CAM_H, &x, &y));
	CHECK_NEAR(x, 800);
	CHECK_NEAR(y, 450);
	CHECK(place_anchor_pos(PLACE_ANCHOR_BOTTOM_RIGHT, &c, CANVAS_W,
			       CANVAS_H, CAM_W, CAM_H, &x, &y));
	CHECK_NEAR(x, RIGHT_X);
	CHECK_NEAR(y, BOTTOM_Y);
	CHECK(place_anchor_pos(PLACE_ANCHOR_BOTTOM_LEFT, &c, CANVAS_W,
			       CANVAS_H, CAM_W, CAM_H, &x, &y));
	CHECK_NEAR(x, LEFT_X);
	CHECK_NEAR(y, BOTTOM_Y);

	CHECK(!place_anchor_pos(PLACE_ANCHOR_NONE, &c, CANVAS_W, CANVAS_H,
				CAM_W, CAM_H, &x, &y));
	CHECK(!place_anchor_pos(PLACE_ANCHOR_CAPTURED, &c, CANVAS_W, CANVAS_H,
				CAM_W, CAM_H, &x, &y));
	c.has_captured = true;
	c.captured_x = 300;
	c.captured_y = 200;
	CHECK(place_anchor_pos(PLACE_ANCHOR_CAPTURED, &c, CANVAS_W, CANVAS_H,
			       CAM_W, CAM_H, &x, &y));
	CHECK_NEAR(x, 300);
	CHECK_NEAR(y, 200);
}

static void test_defaults(void)
{
	struct place_config c;
	place_config_default(&c);
	CHECK(c.mode == PLACE_MODE_PREFERRED_THEN_ANYWHERE);
	CHECK(c.choices[0] == PLACE_ANCHOR_BOTTOM_RIGHT);
	CHECK(c.choices[1] == PLACE_ANCHOR_TOP_RIGHT);
	CHECK(c.choices[2] == PLACE_ANCHOR_BOTTOM_LEFT);
	CHECK(c.choices[3] == PLACE_ANCHOR_NONE);
	CHECK_NEAR(c.margin, 24);
	CHECK(c.return_home);
	CHECK(c.fallback == PLACE_FALLBACK_HIDE);
	CHECK_NEAR(c.t_suitable, 0.01);
	CHECK(c.confirm_checks == 1);
	CHECK(c.return_delay_ns == 3 * SEC);
	CHECK(c.cooldown_ns == 1 * SEC);
	CHECK_NEAR(c.lambda, 0.01);
	CHECK(c.grid_divisions == 48);
	CHECK(!c.prefer_images);
	CHECK_NEAR(c.t_image, 0.9);
}

static void test_stays_put(void)
{
	struct sim s;
	sim_init(&s, RIGHT_X, BOTTOM_Y);
	for (int i = 0; i < 10; i++)
		CHECK(sim_step(&s, i) == PLACE_ACTION_NONE);
	CHECK(at(&s, RIGHT_X, BOTTOM_Y));
	CHECK(s.last.chosen >= 0 && s.last.chosen < s.last.n_report);
	CHECK(s.last.n_report == 3); /* three choices, current is BR */
	CHECK_NEAR(s.last.report[s.last.chosen].x, RIGHT_X);
	CHECK(s.last.report[s.last.chosen].suitable);
	CHECK_NEAR(s.last.report[0].w, CAM_W); /* not inflated */

	/* Elsewhere and clear (not a choice): stays put too, until return
	 * home kicks in after the delay. */
	sim_init(&s, 800, 450);
	CHECK(sim_step(&s, 0) == PLACE_ACTION_NONE);
	CHECK(at(&s, 800, 450));
}

static void test_priority(void)
{
	struct sim s;

	/* BR busy: move to TR. */
	sim_init(&s, RIGHT_X, BOTTOM_Y);
	map_busy_at(&s.map, RIGHT_X, BOTTOM_Y);
	CHECK(sim_step(&s, 0) == PLACE_ACTION_MOVE);
	CHECK(at(&s, RIGHT_X, TOP_Y));
	CHECK(s.last.chosen >= 0);
	CHECK_NEAR(s.last.report[s.last.chosen].y, TOP_Y);

	/* BR and TR busy: move to BL. */
	sim_init(&s, RIGHT_X, BOTTOM_Y);
	map_busy_at(&s.map, RIGHT_X, BOTTOM_Y);
	map_busy_at(&s.map, RIGHT_X, TOP_Y);
	CHECK(sim_step(&s, 0) == PLACE_ACTION_MOVE);
	CHECK(at(&s, LEFT_X, BOTTOM_Y));

	/* Content just outside the camera box but within the margin still
	 * counts (score rect is inflated). */
	sim_init(&s, RIGHT_X, BOTTOM_Y);
	map_add(&s.map, RIGHT_X - 20, BOTTOM_Y, 15, CAM_H);
	CHECK(sim_step(&s, 0) == PLACE_ACTION_MOVE);
	CHECK(at(&s, RIGHT_X, TOP_Y));
}

static void test_confirm_checks(void)
{
	struct sim s;
	sim_init(&s, RIGHT_X, BOTTOM_Y);
	s.cfg.confirm_checks = 3;
	map_busy_at(&s.map, RIGHT_X, BOTTOM_Y);
	CHECK(sim_step(&s, 0) == PLACE_ACTION_NONE);
	CHECK(sim_step(&s, 1) == PLACE_ACTION_NONE);
	CHECK(sim_step(&s, 2) == PLACE_ACTION_MOVE);
	CHECK(at(&s, RIGHT_X, TOP_Y));

	/* A good check in between resets the count. */
	sim_init(&s, RIGHT_X, BOTTOM_Y);
	s.cfg.confirm_checks = 3;
	map_busy_at(&s.map, RIGHT_X, BOTTOM_Y);
	CHECK(sim_step(&s, 0) == PLACE_ACTION_NONE);
	CHECK(sim_step(&s, 1) == PLACE_ACTION_NONE);
	s.map.n = 0;
	CHECK(sim_step(&s, 2) == PLACE_ACTION_NONE);
	map_busy_at(&s.map, RIGHT_X, BOTTOM_Y);
	CHECK(sim_step(&s, 3) == PLACE_ACTION_NONE);
	CHECK(sim_step(&s, 4) == PLACE_ACTION_NONE);
	CHECK(at(&s, RIGHT_X, BOTTOM_Y));
	CHECK(sim_step(&s, 5) == PLACE_ACTION_MOVE);
	CHECK(at(&s, RIGHT_X, TOP_Y));
}

static void test_cooldown(void)
{
	struct sim s;
	sim_init(&s, RIGHT_X, BOTTOM_Y);
	map_busy_at(&s.map, RIGHT_X, BOTTOM_Y);
	CHECK(sim_step(&s, 0) == PLACE_ACTION_MOVE); /* -> TR */
	CHECK(at(&s, RIGHT_X, TOP_Y));

	map_busy_at(&s.map, RIGHT_X, TOP_Y);
	CHECK(sim_step(&s, 0.3) == PLACE_ACTION_NONE);
	CHECK(sim_step(&s, 0.6) == PLACE_ACTION_NONE);
	CHECK(at(&s, RIGHT_X, TOP_Y));
	CHECK(sim_step(&s, 1.0) == PLACE_ACTION_MOVE); /* first after cooldown */
	CHECK(at(&s, LEFT_X, BOTTOM_Y));

	/* Counters accumulate during cooldown: confirm 2 is already met. */
	sim_init(&s, RIGHT_X, BOTTOM_Y);
	s.cfg.confirm_checks = 2;
	s.cfg.cooldown_ns = 2 * SEC;
	map_busy_at(&s.map, RIGHT_X, BOTTOM_Y);
	CHECK(sim_step(&s, 0) == PLACE_ACTION_NONE);
	CHECK(sim_step(&s, 0.5) == PLACE_ACTION_MOVE);
	map_busy_at(&s.map, RIGHT_X, TOP_Y);
	CHECK(sim_step(&s, 1.0) == PLACE_ACTION_NONE);
	CHECK(sim_step(&s, 1.5) == PLACE_ACTION_NONE);
	CHECK(sim_step(&s, 2.5) == PLACE_ACTION_MOVE);
	CHECK(at(&s, LEFT_X, BOTTOM_Y));
}

static void test_return_home(void)
{
	struct sim s;

	/* Home clear from t=1: return at t=4, not before. */
	sim_init(&s, RIGHT_X, BOTTOM_Y);
	map_busy_at(&s.map, RIGHT_X, BOTTOM_Y);
	CHECK(sim_step(&s, 0) == PLACE_ACTION_MOVE);
	CHECK(at(&s, RIGHT_X, TOP_Y));
	s.map.n = 0;
	CHECK(sim_step(&s, 1) == PLACE_ACTION_NONE);
	CHECK(sim_step(&s, 2) == PLACE_ACTION_NONE);
	CHECK(sim_step(&s, 3.9) == PLACE_ACTION_NONE);
	CHECK(sim_step(&s, 4) == PLACE_ACTION_MOVE);
	CHECK(at(&s, RIGHT_X, BOTTOM_Y));
	CHECK(sim_step(&s, 5) == PLACE_ACTION_NONE);

	/* Home busy again in between: the timer restarts. */
	sim_init(&s, RIGHT_X, BOTTOM_Y);
	map_busy_at(&s.map, RIGHT_X, BOTTOM_Y);
	CHECK(sim_step(&s, 0) == PLACE_ACTION_MOVE);
	s.map.n = 0;
	CHECK(sim_step(&s, 1) == PLACE_ACTION_NONE);
	map_busy_at(&s.map, RIGHT_X, BOTTOM_Y);
	CHECK(sim_step(&s, 2) == PLACE_ACTION_NONE);
	s.map.n = 0;
	CHECK(sim_step(&s, 3) == PLACE_ACTION_NONE);
	CHECK(sim_step(&s, 4) == PLACE_ACTION_NONE);
	CHECK(sim_step(&s, 5) == PLACE_ACTION_NONE);
	CHECK(at(&s, RIGHT_X, TOP_Y));
	CHECK(sim_step(&s, 6) == PLACE_ACTION_MOVE);
	CHECK(at(&s, RIGHT_X, BOTTOM_Y));

	/* Disabled: never returns. */
	sim_init(&s, RIGHT_X, BOTTOM_Y);
	s.cfg.return_home = false;
	map_busy_at(&s.map, RIGHT_X, BOTTOM_Y);
	CHECK(sim_step(&s, 0) == PLACE_ACTION_MOVE);
	s.map.n = 0;
	for (int i = 1; i < 10; i++)
		CHECK(sim_step(&s, i) == PLACE_ACTION_NONE);
	CHECK(at(&s, RIGHT_X, TOP_Y));

	/* Generalised: at BL (3rd) with BR busy, TR (2nd) clear long enough
	 * returns to TR, not BR. */
	sim_init(&s, LEFT_X, BOTTOM_Y);
	map_busy_at(&s.map, RIGHT_X, BOTTOM_Y);
	CHECK(sim_step(&s, 0) == PLACE_ACTION_NONE);
	CHECK(sim_step(&s, 2) == PLACE_ACTION_NONE);
	CHECK(sim_step(&s, 3) == PLACE_ACTION_MOVE);
	CHECK(at(&s, RIGHT_X, TOP_Y));
	/* ... and at TR, a 2nd choice, it won't move to BL (lower). */
	for (int i = 4; i < 10; i++)
		CHECK(sim_step(&s, i) == PLACE_ACTION_NONE);
	CHECK(at(&s, RIGHT_X, TOP_Y));

	/* Not at a choice (auto position): returns to home after delay. */
	sim_init(&s, 800, 450);
	CHECK(sim_step(&s, 0) == PLACE_ACTION_NONE);
	CHECK(sim_step(&s, 2) == PLACE_ACTION_NONE);
	CHECK(sim_step(&s, 3) == PLACE_ACTION_MOVE);
	CHECK(at(&s, RIGHT_X, BOTTOM_Y));
}

static void test_hide_show(void)
{
	struct sim s;
	sim_init(&s, RIGHT_X, BOTTOM_Y);
	s.cfg.confirm_checks = 2;
	map_add(&s.map, 0, 0, CANVAS_W, CANVAS_H);
	CHECK(sim_step(&s, 0) == PLACE_ACTION_NONE);
	CHECK(sim_step(&s, 1) == PLACE_ACTION_HIDE);
	CHECK(!s.visible);
	CHECK(s.last.chosen == -1);
	CHECK(at(&s, RIGHT_X, BOTTOM_Y)); /* position unchanged */
	CHECK(sim_step(&s, 2) == PLACE_ACTION_NONE);

	/* Only TR clears: needs two consecutive good checks. */
	s.map.n = 0;
	map_busy_at(&s.map, RIGHT_X, BOTTOM_Y);
	map_busy_at(&s.map, LEFT_X, BOTTOM_Y);
	map_add(&s.map, 0, 300, CANVAS_W, 300); /* keep the middle busy */
	CHECK(sim_step(&s, 3) == PLACE_ACTION_NONE);
	CHECK(!s.visible);
	s.map.n = 0;
	map_add(&s.map, 0, 0, CANVAS_W, CANVAS_H);
	CHECK(sim_step(&s, 4) == PLACE_ACTION_NONE); /* reset */
	s.map.n = 0;
	map_busy_at(&s.map, RIGHT_X, BOTTOM_Y);
	CHECK(sim_step(&s, 5) == PLACE_ACTION_NONE);
	CHECK(sim_step(&s, 6) == PLACE_ACTION_SHOW);
	CHECK(s.visible);
	CHECK(at(&s, RIGHT_X, TOP_Y));
	CHECK(s.last.chosen >= 0);

	/* Preferred-only mode hides too, and shows at home once clear. */
	sim_init(&s, RIGHT_X, BOTTOM_Y);
	s.cfg.mode = PLACE_MODE_PREFERRED;
	map_busy_at(&s.map, RIGHT_X, BOTTOM_Y);
	map_busy_at(&s.map, RIGHT_X, TOP_Y);
	map_busy_at(&s.map, LEFT_X, BOTTOM_Y);
	CHECK(sim_step(&s, 0) == PLACE_ACTION_HIDE);
	s.map.n = 0;
	CHECK(sim_step(&s, 0.5) == PLACE_ACTION_NONE); /* cooldown */
	CHECK(sim_step(&s, 1) == PLACE_ACTION_SHOW);
	CHECK(at(&s, RIGHT_X, BOTTOM_Y));

	/* First call with the camera already hidden counts as hidden. */
	sim_init(&s, RIGHT_X, BOTTOM_Y);
	s.visible = false;
	CHECK(sim_step(&s, 0) == PLACE_ACTION_SHOW);
	CHECK(s.visible);
}

static void test_least_busy(void)
{
	struct sim s;

	/* Busy everywhere left of x = 1700: no rect is ever suitable. The
	 * least-busy one hugs the right edge, nearest the current spot. */
	sim_init(&s, LEFT_X, BOTTOM_Y);
	s.cfg.fallback = PLACE_FALLBACK_LEAST_BUSY;
	s.cfg.choices[0] = PLACE_ANCHOR_BOTTOM_LEFT;
	s.cfg.choices[1] = PLACE_ANCHOR_NONE;
	s.cfg.choices[2] = PLACE_ANCHOR_NONE;
	map_add(&s.map, 0, 0, 1700, CANVAS_H);
	CHECK(sim_step(&s, 0) == PLACE_ACTION_MOVE);
	CHECK(at(&s, RIGHT_X, BOTTOM_Y));
	CHECK(s.visible);
	/* Already there: no MOVE to the current position. */
	CHECK(sim_step(&s, 2) == PLACE_ACTION_NONE);
	CHECK(sim_step(&s, 4) == PLACE_ACTION_NONE);

	/* Preferred mode: stay put. */
	sim_init(&s, LEFT_X, BOTTOM_Y);
	s.cfg.mode = PLACE_MODE_PREFERRED;
	s.cfg.fallback = PLACE_FALLBACK_LEAST_BUSY;
	map_add(&s.map, 0, 0, 1700, CANVAS_H);
	CHECK(sim_step(&s, 0) == PLACE_ACTION_NONE);
	CHECK(sim_step(&s, 1) == PLACE_ACTION_NONE);
	CHECK(at(&s, LEFT_X, BOTTOM_Y));
	CHECK(s.visible);
}

static void test_anywhere(void)
{
	struct sim s;

	/* Single clear hole in the middle of a busy canvas. */
	sim_init(&s, LEFT_X, TOP_Y);
	s.cfg.mode = PLACE_MODE_ANYWHERE;
	s.map.inverted = true;
	map_add(&s.map, 700, 350, 520, 380);
	CHECK(sim_step(&s, 0) == PLACE_ACTION_MOVE);
	CHECK(s.x - MARGIN >= 700 && s.x + CAM_W + MARGIN <= 1220);
	CHECK(s.y - MARGIN >= 350 && s.y + CAM_H + MARGIN <= 730);
	CHECK(s.last.chosen >= 0);
	CHECK(s.last.report[s.last.chosen].suitable);
	/* No choice rects in ANYWHERE mode: current + chosen only. */
	CHECK(s.last.n_report == 2);
	CHECK(sim_step(&s, 2) == PLACE_ACTION_NONE);
	/* No return home in ANYWHERE mode, even though BR is clear now. */
	s.map.inverted = false;
	s.map.n = 0;
	for (int i = 3; i < 10; i++)
		CHECK(sim_step(&s, i) == PLACE_ACTION_NONE);

	/* Two equal gaps: the nearer one wins, from either side. */
	for (int side = 0; side < 2; side++) {
		sim_init(&s, side ? RIGHT_X : LEFT_X, TOP_Y);
		s.cfg.mode = PLACE_MODE_ANYWHERE;
		s.map.inverted = true;
		map_add(&s.map, 100, 500, 500, 400);
		map_add(&s.map, 1320, 500, 500, 400);
		CHECK(sim_step(&s, 0) == PLACE_ACTION_MOVE);
		if (side)
			CHECK(s.x >= 1320);
		else
			CHECK(s.x + CAM_W <= 600);
		CHECK(s.y - MARGIN >= 500);
	}

	/* Preferred-then-anywhere: falls back to the search when all choices
	 * are busy. */
	sim_init(&s, RIGHT_X, BOTTOM_Y);
	s.map.inverted = true;
	map_add(&s.map, 700, 350, 520, 380);
	CHECK(sim_step(&s, 0) == PLACE_ACTION_MOVE);
	CHECK(s.x - MARGIN >= 700 && s.x + CAM_W + MARGIN <= 1220);
}

static void test_margins(void)
{
	struct sim s;

	/* Hole flush with the right edge: search reaches the far edge but
	 * never past the margin. */
	sim_init(&s, LEFT_X, TOP_Y);
	s.cfg.mode = PLACE_MODE_ANYWHERE;
	s.map.inverted = true;
	map_add(&s.map, 1500, 0, 420, CANVAS_H);
	CHECK(sim_step(&s, 0) == PLACE_ACTION_MOVE);
	CHECK(within_margins(s.x, s.y));
	CHECK(s.x >= 1524);
	CHECK_NEAR(s.y, TOP_Y);

	/* Random busy maps: every MOVE/SHOW target stays within margins. */
	srand(1234);
	for (int iter = 0; iter < 200; iter++) {
		sim_init(&s, LEFT_X, TOP_Y);
		s.cfg.mode = (enum place_mode)(iter % 3);
		s.cfg.fallback = (enum place_fallback)((iter / 3) % 2);
		int n = 1 + rand() % 8;
		for (int i = 0; i < n; i++)
			map_add(&s.map, rand() % 1920, rand() % 1080,
				rand() % 900, rand() % 600);
		for (int t = 0; t < 3; t++) {
			enum place_action a = sim_step(&s, t * 1.5);
			if (a == PLACE_ACTION_MOVE || a == PLACE_ACTION_SHOW)
				CHECK(within_margins(s.x, s.y));
			for (int i = 0; i < s.last.n_report; i++)
				CHECK(s.last.report[i].score >= 0.0 &&
				      s.last.report[i].score <= 1.0);
			CHECK(s.last.n_report <= PLACE_MAX_REPORT);
		}
	}
}

static void test_ease(void)
{
	CHECK_NEAR(place_ease_in_out(0.0), 0.0);
	CHECK_NEAR(place_ease_in_out(1.0), 1.0);
	CHECK_NEAR(place_ease_in_out(0.5), 0.5);
	CHECK_NEAR(place_ease_in_out(-1.0), 0.0);
	CHECK_NEAR(place_ease_in_out(2.0), 1.0);
	CHECK(place_ease_in_out(0.25) < 0.25); /* eases in */
	CHECK(place_ease_in_out(0.75) > 0.75); /* eases out */
	double prev = 0.0;
	bool mono = true;
	for (int i = 1; i <= 1000; i++) {
		double v = place_ease_in_out(i / 1000.0);
		if (v < prev)
			mono = false;
		prev = v;
	}
	CHECK(mono);
}

static void test_images(void)
{
	struct sim s;

	/* Image on the right: from BL to BR on new content. */
	sim_init_images(&s, LEFT_X, BOTTOM_Y);
	sim_slide(&s, 2);
	CHECK(sim_step_nc(&s, 0, true) == PLACE_ACTION_MOVE);
	CHECK(at(&s, RIGHT_X, BOTTOM_Y));
	CHECK(s.last.chosen >= 0);
	CHECK(s.last.report[s.last.chosen].on_image);
	CHECK_NEAR(s.last.report[s.last.chosen].image_frac, 1.0);
	CHECK(sim_step(&s, 5) == PLACE_ACTION_NONE);

	/* Image on the left with BR (empty, suitable) as 1st choice: BL. */
	sim_init_images(&s, RIGHT_X, BOTTOM_Y);
	sim_slide(&s, 1);
	CHECK(sim_step_nc(&s, 0, true) == PLACE_ACTION_MOVE);
	CHECK(at(&s, LEFT_X, BOTTOM_Y));
	/* ... and it stays there: return home doesn't pull it off the image
	 * onto the empty (non-image) 1st choice. */
	for (int i = 1; i < 12; i++)
		CHECK(sim_step(&s, i) == PLACE_ACTION_NONE);
	CHECK(at(&s, LEFT_X, BOTTOM_Y));
	/* Repeated new_content on the same slide: already well placed. */
	CHECK(sim_step_nc(&s, 12, true) == PLACE_ACTION_NONE);

	/* Next slide has its image on the right: re-rank moves to BR. */
	sim_slide(&s, 2);
	CHECK(sim_step_nc(&s, 13, true) == PLACE_ACTION_MOVE);
	CHECK(at(&s, RIGHT_X, BOTTOM_Y));
	/* Then a slide without an image: BR is clear, so it stays put. */
	sim_slide(&s, 0);
	CHECK(sim_step_nc(&s, 14, true) == PLACE_ACTION_NONE);
	CHECK(at(&s, RIGHT_X, BOTTOM_Y));

	/* Without an image the behaviour is unchanged: stay put at BL while
	 * clear, then return home to BR after the delay. */
	sim_init_images(&s, LEFT_X, BOTTOM_Y);
	sim_slide(&s, 0);
	CHECK(sim_step_nc(&s, 0, true) == PLACE_ACTION_NONE);
	CHECK(sim_step(&s, 2) == PLACE_ACTION_NONE);
	CHECK(sim_step(&s, 3) == PLACE_ACTION_MOVE);
	CHECK(at(&s, RIGHT_X, BOTTOM_Y));

	/* prefer_images off: images are ignored, BR stays. */
	sim_init(&s, RIGHT_X, BOTTOM_Y);
	s.cfg.choices[1] = PLACE_ANCHOR_BOTTOM_LEFT;
	s.cfg.choices[2] = PLACE_ANCHOR_NONE;
	sim_slide(&s, 1);
	CHECK(sim_step_nc(&s, 0, true) == PLACE_ACTION_NONE);
	CHECK(at(&s, RIGHT_X, BOTTOM_Y));
	CHECK(!s.last.report[0].on_image);

	/* Image-suitable needs suitable: content on the photo's corner (the
	 * caption of an image, say) keeps the camera off it. */
	sim_init_images(&s, RIGHT_X, BOTTOM_Y);
	sim_slide(&s, 1);
	map_busy_at(&s.map, LEFT_X, BOTTOM_Y);
	CHECK(sim_step_nc(&s, 0, true) == PLACE_ACTION_NONE);
	CHECK(at(&s, RIGHT_X, BOTTOM_Y));

	/* Image coverage below t_image is not enough. */
	sim_init_images(&s, RIGHT_X, BOTTOM_Y);
	s.map.img[s.map.n_img++] = (struct rect){0, 0, LEFT_X + CAM_W * 0.8,
						 CANVAS_H};
	CHECK(sim_step_nc(&s, 0, true) == PLACE_ACTION_NONE);
	s.cfg.t_image = 0.75;
	CHECK(sim_step_nc(&s, 1, true) == PLACE_ACTION_MOVE);
	CHECK(at(&s, LEFT_X, BOTTOM_Y));
}

static void test_images_cooldown(void)
{
	struct sim s;

	/* Re-rank during the cooldown waits and stays pending without
	 * further new_content. */
	sim_init_images(&s, LEFT_X, BOTTOM_Y);
	sim_slide(&s, 2);
	CHECK(sim_step_nc(&s, 0, true) == PLACE_ACTION_MOVE); /* -> BR */
	sim_slide(&s, 1);
	CHECK(sim_step_nc(&s, 0.4, true) == PLACE_ACTION_NONE); /* cooling */
	CHECK(sim_step(&s, 0.7) == PLACE_ACTION_NONE);
	CHECK(at(&s, RIGHT_X, BOTTOM_Y));
	CHECK(sim_step(&s, 1.0) == PLACE_ACTION_MOVE); /* sticky request */
	CHECK(at(&s, LEFT_X, BOTTOM_Y));
	CHECK(sim_step(&s, 2.5) == PLACE_ACTION_NONE);

	/* Superseded: the next new_content (no image) drops the request. */
	sim_init_images(&s, LEFT_X, BOTTOM_Y);
	sim_slide(&s, 2);
	CHECK(sim_step_nc(&s, 0, true) == PLACE_ACTION_MOVE); /* -> BR */
	sim_slide(&s, 1);
	CHECK(sim_step_nc(&s, 0.4, true) == PLACE_ACTION_NONE);
	sim_slide(&s, 0);
	CHECK(sim_step_nc(&s, 0.8, true) == PLACE_ACTION_NONE);
	CHECK(sim_step(&s, 1.2) == PLACE_ACTION_NONE);
	CHECK(at(&s, RIGHT_X, BOTTOM_Y));

	/* Confirm checks don't delay the re-rank. */
	sim_init_images(&s, RIGHT_X, BOTTOM_Y);
	s.cfg.confirm_checks = 3;
	sim_slide(&s, 1);
	CHECK(sim_step_nc(&s, 0, true) == PLACE_ACTION_MOVE);
	CHECK(at(&s, LEFT_X, BOTTOM_Y));
}

static void test_images_hidden(void)
{
	struct sim s;

	/* Hidden camera shows straight onto the image (no confirm checks). */
	sim_init_images(&s, RIGHT_X, BOTTOM_Y);
	s.cfg.confirm_checks = 3;
	s.visible = false;
	sim_slide(&s, 1);
	CHECK(sim_step_nc(&s, 0, true) == PLACE_ACTION_SHOW);
	CHECK(s.visible);
	CHECK(at(&s, LEFT_X, BOTTOM_Y));

	/* Hidden, no image: the usual confirm checks apply, and the shown
	 * position is the 1st suitable choice. */
	sim_init_images(&s, LEFT_X, BOTTOM_Y);
	s.cfg.confirm_checks = 2;
	s.visible = false;
	sim_slide(&s, 0);
	CHECK(sim_step_nc(&s, 0, true) == PLACE_ACTION_NONE);
	CHECK(sim_step(&s, 1) == PLACE_ACTION_SHOW);
	CHECK(at(&s, RIGHT_X, BOTTOM_Y));

	/* Hidden without new_content: shows at the image-suitable choice
	 * once confirmed, ahead of a higher-priority empty choice. */
	sim_init_images(&s, RIGHT_X, BOTTOM_Y);
	s.visible = false;
	sim_slide(&s, 1);
	CHECK(sim_step(&s, 0) == PLACE_ACTION_SHOW);
	CHECK(at(&s, LEFT_X, BOTTOM_Y));

	/* Current spot turns busy: the move prefers the image choice. */
	sim_init_images(&s, LEFT_X, TOP_Y);
	s.cfg.choices[0] = PLACE_ANCHOR_TOP_LEFT;
	s.cfg.choices[1] = PLACE_ANCHOR_BOTTOM_RIGHT;
	s.cfg.choices[2] = PLACE_ANCHOR_BOTTOM_LEFT;
	map_photo(&s.map, false);
	map_busy_at(&s.map, LEFT_X, TOP_Y);
	CHECK(sim_step(&s, 0) == PLACE_ACTION_MOVE);
	CHECK(at(&s, LEFT_X, BOTTOM_Y));
}

static void test_images_anywhere(void)
{
	struct sim s;

	/* ANYWHERE: the re-rank searches the grid for an image-suitable
	 * spot; the auto search prefers one too. */
	sim_init_images(&s, RIGHT_X, TOP_Y);
	s.cfg.mode = PLACE_MODE_ANYWHERE;
	sim_slide(&s, 1);
	CHECK(sim_step_nc(&s, 0, true) == PLACE_ACTION_MOVE);
	CHECK(s.x + CAM_W <= CANVAS_W / 2 + 1e-6);
	CHECK(within_margins(s.x, s.y));
	CHECK(s.last.report[s.last.chosen].on_image);
	CHECK(sim_step_nc(&s, 3, true) == PLACE_ACTION_NONE); /* stays */

	/* Preferred-then-anywhere, choices all busy: auto search lands on
	 * the photo rather than the nearest empty spot. */
	for (int pref = 0; pref < 2; pref++) {
		sim_init_images(&s, RIGHT_X, BOTTOM_Y);
		s.cfg.prefer_images = pref;
		s.cfg.choices[1] = PLACE_ANCHOR_TOP_RIGHT;
		s.map.inverted = true;
		map_add(&s.map, 1000, 300, 600, 500); /* empty hole, near */
		map_add(&s.map, 0, 0, 900, CANVAS_H); /* empty hole = photo */
		s.map.img[s.map.n_img++] = (struct rect){0, 0, 900, CANVAS_H};
		CHECK(sim_step(&s, 0) == PLACE_ACTION_MOVE);
		if (pref)
			CHECK(s.x + CAM_W <= 900);
		else
			CHECK(s.x >= 1000);
	}
}

int main(void)
{
	test_anchors();
	test_defaults();
	test_stays_put();
	test_priority();
	test_confirm_checks();
	test_cooldown();
	test_return_home();
	test_hide_show();
	test_least_busy();
	test_anywhere();
	test_margins();
	test_ease();
	test_images();
	test_images_cooldown();
	test_images_hidden();
	test_images_anywhere();

	if (failures) {
		fprintf(stderr, "test_place: %d of %d checks FAILED\n",
			failures, checks);
		return 1;
	}
	printf("test_place: all %d checks passed\n", checks);
	return 0;
}
