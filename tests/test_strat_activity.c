/*
 * obs-sidestep — unit tests for the "Screen share / live demo" strategy
 * (core/strat_activity.c): sequences of synthetic frames with timestamps.
 *
 * SPDX-License-Identifier: MIT
 */
#include "busy.h"
#include "strategy.h"

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

#define CHECK_RANGE(a, lo, hi)                                            \
	do {                                                              \
		double a_ = (a);                                          \
		checks++;                                                 \
		if (!(a_ >= (lo) && a_ <= (hi))) {                        \
			failures++;                                       \
			fprintf(stderr,                                   \
				"%s:%d: CHECK_RANGE failed: %s = %g, "    \
				"expected [%g, %g]\n",                    \
				__FILE__, __LINE__, #a, a_, (double)(lo), \
				(double)(hi));                            \
		}                                                         \
	} while (0)

#define W 320
#define H 180
#define SEC 1000000000ull

/* ------------------------------------------------------------------------ */
/* Synthetic frames (grey, RGBA)                                             */

static uint8_t frame[W * H * 4];

static uint32_t rng_state = 12345;
static uint32_t rng(void)
{
	rng_state = rng_state * 1664525u + 1013904223u;
	return rng_state >> 8;
}

static void set(uint32_t x, uint32_t y, uint8_t v)
{
	uint8_t *p = frame + ((size_t)y * W + x) * 4;
	p[0] = p[1] = p[2] = v;
	p[3] = 255;
}

static void fill(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint8_t v)
{
	for (uint32_t y = y0; y < y1; y++)
		for (uint32_t x = x0; x < x1; x++)
			set(x, y, v);
}

/* Fake text: 10 px lines of 6 px "glyphs". */
static void text(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint8_t ink)
{
	for (uint32_t y = y0; y < y1; y++) {
		uint32_t ly = (y - y0) % 10;
		if (ly >= 7)
			continue;
		for (uint32_t x = x0; x < x1; x++) {
			uint32_t gx = (x - x0) % 6;
			if (gx < 4 && ((gx + ly) % 3 != 0))
				set(x, y, ink);
		}
	}
}

static void noise(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
	for (uint32_t y = y0; y < y1; y++)
		for (uint32_t x = x0; x < x1; x++)
			set(x, y, (uint8_t)rng());
}

/* "Editor": white page, static text on the left half. */
static void base(void)
{
	fill(0, 0, W, H, 250);
	text(8, 8, 150, 172, 20);
}

/* Active area: a 16x16 "terminal" at (240, 120) whose content changes. */
#define AX 240
#define AY 120
#define AS 16

static struct busy_analyzer *b;
static struct busy_params p;
static uint64_t t_ns;

static void step(uint64_t dt_ns)
{
	t_ns += dt_ns;
	p.now_ns = t_ns;
	busy_process(b, frame, W * 4, BUSY_FMT_RGBA, &p);
}

static int max_cost(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
	int m = 0;
	for (uint32_t y = y0; y < y1; y++)
		for (uint32_t x = x0; x < x1; x++)
			if (b->cost[y * W + x] > m)
				m = b->cost[y * W + x];
	return m;
}

static double active_score(void)
{
	return busy_rect_score(b, AX, AY, AS, AS);
}

/* Camera-sized rect (1/4 width) around the active area. */
static double cam_score(void)
{
	return busy_rect_score(b, AX - 32, AY - 12, 80, 45);
}

static void setup(void)
{
	busy_destroy(b);
	b = busy_create(W, H);
	if (!b) {
		fprintf(stderr, "out of memory\n");
		exit(2);
	}
	busy_params_default(&p);
	p.strategy = BUSY_STRATEGY_ACTIVITY;
	t_ns = 1000 * SEC;
	base();
	step(0);
	step(SEC); /* settled: no global change any more */
}

/* Type into the active area for `n` seconds, one check per second. */
static void activity(int n)
{
	for (int i = 0; i < n; i++) {
		base();
		noise(AX, AY, AX + AS, AY + AS);
		step(SEC);
	}
	base();
	step(SEC); /* stops: last change still counts */
}

static void idle(int seconds, uint64_t dt_ns)
{
	uint64_t end = t_ns + (uint64_t)seconds * SEC;
	base();
	while (t_ns + dt_ns <= end)
		step(dt_ns);
}

/* ------------------------------------------------------------------------ */

static void test_registry(void)
{
	const struct busy_strategy_ops *s =
		busy_strategy_find(BUSY_STRATEGY_ACTIVITY);
	CHECK(s == &busy_strat_activity);
	CHECK(s->id == 3);
	CHECK(!s->prefers_images);
	CHECK(strcmp(s->name_key, "Strategy.Activity") == 0);
	CHECK(strcmp(s->desc_key, "Strategy.Activity.Description") == 0);
}

static void test_static_text(void)
{
	setup();
	idle(10, SEC);
	CHECK(b->strategy == &busy_strat_activity);
	/* Static text costs a little: below the default 1 % threshold,
	 * but more than empty space. */
	double txt = busy_rect_score(b, 20, 20, 80, 45);
	double empty = busy_rect_score(b, 200, 20, 80, 45);
	CHECK_RANGE(txt, 0.001, 0.01);
	CHECK(empty == 0.0);
	CHECK(max_cost(0, 0, W, H) <= 8);
}

static void test_heat_persists_and_decays(void)
{
	setup();
	activity(5);
	CHECK(max_cost(AX, AY, AX + AS, AY + AS) == 255);
	CHECK_RANGE(active_score(), 0.9, 1.0);
	CHECK(cam_score() > 0.05);

	/* Dilation: one tile (4 px) around the area is hot too, two tiles
	 * away is not. */
	CHECK(max_cost(AX - 4, AY, AX - 1, AY + AS) == 255);
	CHECK(max_cost(AX + AS, AY, AX + AS + 4, AY + AS) == 255);
	CHECK(max_cost(AX - 12, AY, AX - 8, AY + AS) == 0);
	CHECK(max_cost(AX, AY + AS + 8, AX + AS, AY + AS + 12) == 0);

	/* Still clearly hot 3 s after typing stops (half-life ~8 s). */
	idle(3, SEC);
	int c3 = max_cost(AX, AY, AX + AS, AY + AS);
	CHECK_RANGE(c3, 150, 230);
	CHECK(cam_score() > 0.01); /* unsuitable at default sensitivity */

	/* Low after ~30 s. The 16x16 area (24x24 dilated) fills 16 % of
	 * the camera rect, so it takes 4 half-lives to fall under 1 %. */
	idle(27, SEC);
	int c30 = max_cost(AX, AY, AX + AS, AY + AS);
	CHECK_RANGE(c30, 5, 30);
	CHECK(cam_score() < 0.015);
	idle(5, SEC);
	CHECK(cam_score() < 0.01);

	idle(60, SEC);
	CHECK(max_cost(AX, AY, AX + AS, AY + AS) == 0);
}

static void test_decay_independent_of_interval(void)
{
	/* Same wall time, 0.1 s vs 2 s checks: about the same heat. */
	setup();
	activity(3);
	idle(8, 100000000ull);
	int fast = max_cost(AX, AY, AX + AS, AY + AS);

	setup();
	activity(3);
	idle(8, 2 * SEC);
	int slow = max_cost(AX, AY, AX + AS, AY + AS);

	CHECK_RANGE(fast, 115, 140); /* ~255 / 2 */
	CHECK_RANGE(slow, 115, 140);
}

static void test_unknown_timestamps(void)
{
	/* now_ns = 0: one fixed 1 s step per frame. */
	setup();
	activity(3);
	base();
	for (int i = 0; i < 8; i++) {
		p.now_ns = 0;
		busy_process(b, frame, W * 4, BUSY_FMT_RGBA, &p);
	}
	CHECK_RANGE(max_cost(AX, AY, AX + AS, AY + AS), 115, 140);

	/* Timestamps going backwards still decay, and the clock restarts. */
	setup();
	activity(3);
	t_ns -= 100 * SEC;
	step(0);
	int back = max_cost(AX, AY, AX + AS, AY + AS);
	CHECK_RANGE(back, 200, 250);
	step(8 * SEC);
	CHECK_RANGE(max_cost(AX, AY, AX + AS, AY + AS), back / 2 - 10,
		    back / 2 + 5);
}

static void test_global_change(void)
{
	setup();
	/* Window switch / slide change: the whole frame changes. */
	fill(0, 0, W, H, 30);
	text(20, 20, 300, 160, 230);
	step(SEC);
	CHECK(b->global_change > 0.25);
	CHECK(max_cost(0, 0, W, H) <= 8);

	/* The next capture still differs in a large area (end of the
	 * transition, 20 % of the frame) — the tail, ignored too. */
	fill(0, 0, W, 72, 30);
	noise(0, 0, W, 36);
	step(SEC);
	CHECK(b->global_change <= 0.25);
	CHECK(max_cost(0, 0, W, H) <= 8);

	/* Settled, then real activity counts again. */
	step(SEC);
	noise(AX, AY, AX + AS, AY + AS);
	step(SEC);
	CHECK(max_cost(AX, AY, AX + AS, AY + AS) == 255);

	/* Continuous scrolling: every capture changes globally; existing
	 * heat only decays, nothing saturates. */
	for (int i = 0; i < 10; i++) {
		fill(0, 0, W, H, 250);
		text(8, (uint32_t)(i * 3) % 10, 312, 170, 20);
		noise(0, 0, W, 60);
		step(SEC);
		CHECK(b->global_change > 0.25);
	}
	CHECK(max_cost(0, 0, AX - 8, H) <= 8);
	CHECK(max_cost(AX + AS + 8, 0, W, H) <= 8);
	int c = max_cost(AX, AY, AX + AS, AY + AS);
	CHECK_RANGE(c, 100, 150); /* ~255 * 2^(-10/8) */
}

static void test_motion_off(void)
{
	setup();
	p.use_motion = false;
	activity(5);
	CHECK(max_cost(0, 0, W, H) <= 8);
}

static void test_reset(void)
{
	setup();
	activity(5);
	CHECK(max_cost(AX, AY, AX + AS, AY + AS) == 255);
	busy_reset_history(b);
	CHECK(b->strategy == &busy_strat_activity);
	base();
	step(SEC); /* first frame after reset: global change 1.0 */
	CHECK(max_cost(0, 0, W, H) <= 8);
	step(SEC);
	CHECK(max_cost(0, 0, W, H) <= 8);
}

static void test_odd_size(void)
{
	/* Partial edge tiles and other resolutions don't crash and still
	 * detect activity. */
	const uint32_t sizes[][2] = {{161, 91}, {480, 270}, {7, 5}};
	for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
		uint32_t w = sizes[k][0], h = sizes[k][1];
		struct busy_analyzer *a = busy_create(w, h);
		uint8_t *px = calloc((size_t)w * h, 4);
		CHECK(a && px);
		if (!a || !px)
			return;
		struct busy_params q;
		busy_params_default(&q);
		q.strategy = BUSY_STRATEGY_ACTIVITY;
		for (int i = 0; i < 4; i++) {
			q.now_ns = (uint64_t)(i + 1) * SEC;
			/* the bottom-right 2x2 pixels flip */
			for (uint32_t y = h - 2; y < h; y++)
				memset(px + ((size_t)y * w + w - 2) * 4,
				       i & 1 ? 255 : 0, 8);
			busy_process(a, px, w * 4, BUSY_FMT_RGBA, &q);
		}
		CHECK(a->cost[(size_t)h * w - 1] > 100);
		CHECK(a->cost[0] == 0 || w < 16);
		busy_destroy(a);
		free(px);
	}
}

int main(void)
{
	test_registry();
	test_static_text();
	test_heat_persists_and_decays();
	test_decay_independent_of_interval();
	test_unknown_timestamps();
	test_global_change();
	test_motion_off();
	test_reset();
	test_odd_size();
	busy_destroy(b);

	if (failures) {
		fprintf(stderr, "test_strat_activity: %d of %d checks FAILED\n",
			failures, checks);
		return 1;
	}
	printf("test_strat_activity: all %d checks passed\n", checks);
	return 0;
}
