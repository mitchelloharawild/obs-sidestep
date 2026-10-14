/*
 * obs-sidestep — unit tests for the "Solid backgrounds" strategy
 * (core/strat_solid.c).
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

#define CHECK_NEAR(a, b, eps)                                             \
	do {                                                              \
		double a_ = (a), b_ = (b);                                \
		checks++;                                                 \
		if (!(fabs(a_ - b_) <= (eps))) {                          \
			failures++;                                       \
			fprintf(stderr,                                   \
				"%s:%d: CHECK_NEAR failed: %s = %g, "     \
				"expected %g (eps %g)\n",                 \
				__FILE__, __LINE__, #a, a_, b_,           \
				(double)(eps));                           \
		}                                                         \
	} while (0)

/* ------------------------------------------------------------------------ */
/* Synthetic image helpers                                                   */

#define W 320
#define H 180

struct image {
	uint32_t w, h, linesize;
	uint8_t *px;
};

static struct image img_new(void)
{
	struct image im = {W, H, W * 4, NULL};
	im.px = calloc((size_t)im.linesize * im.h, 1);
	if (!im.px) {
		fprintf(stderr, "out of memory\n");
		exit(2);
	}
	return im;
}

static void img_free(struct image *im)
{
	free(im->px);
	im->px = NULL;
}

/* RGBA byte order. */
static void img_set(struct image *im, uint32_t x, uint32_t y, uint8_t r,
		    uint8_t g, uint8_t b)
{
	uint8_t *p = im->px + (size_t)y * im->linesize + 4 * (size_t)x;
	p[0] = r;
	p[1] = g;
	p[2] = b;
	p[3] = 255;
}

static void img_rect(struct image *im, uint32_t x0, uint32_t y0, uint32_t x1,
		     uint32_t y1, uint8_t r, uint8_t g, uint8_t b)
{
	for (uint32_t y = y0; y < y1 && y < im->h; y++)
		for (uint32_t x = x0; x < x1 && x < im->w; x++)
			img_set(im, x, y, r, g, b);
}

static uint32_t rng_state = 12345;
static uint32_t rng(void)
{
	rng_state = rng_state * 1664525u + 1013904223u;
	return rng_state >> 8;
}

/* Fake text: rows of small blobs with gaps. */
static void img_text(struct image *im, uint32_t x0, uint32_t y0, uint32_t x1,
		     uint32_t y1, uint8_t ink)
{
	for (uint32_t y = y0; y < y1; y++) {
		uint32_t ly = (y - y0) % 10;
		if (ly >= 7)
			continue;
		for (uint32_t x = x0; x < x1; x++) {
			uint32_t gx = (x - x0) % 6;
			if (gx < 4 && ((gx + ly) % 3 != 0))
				img_set(im, x, y, ink, ink, ink);
		}
	}
}

static uint8_t clamp8(int v)
{
	return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* Score one frame with a strategy (fresh analyzer, no motion history). */
static struct busy_analyzer *run(const struct image *im, int strategy)
{
	struct busy_analyzer *b = busy_create(im->w, im->h);
	struct busy_params p;
	busy_params_default(&p);
	p.strategy = strategy;
	busy_process(b, im->px, im->linesize, BUSY_FMT_RGBA, &p);
	return b;
}

static double quadrant(const struct busy_analyzer *b, int q)
{
	double hw = b->w / 2.0, hh = b->h / 2.0;
	return busy_rect_score(b, (q & 1) ? hw : 0, (q & 2) ? hh : 0, hw, hh);
}

/* ------------------------------------------------------------------------ */

static void test_registry(void)
{
	CHECK(busy_strategy_find(BUSY_STRATEGY_SOLID) == &busy_strat_solid);
	CHECK(!busy_strat_solid.prefers_images);
	CHECK(!strcmp(busy_strat_solid.name_key, "Strategy.Solid"));
	CHECK(!strcmp(busy_strat_solid.desc_key, "Strategy.Solid.Description"));
	bool listed = false;
	for (size_t i = 0; i < busy_strategy_count(); i++)
		listed = listed || busy_strategy_get(i) == &busy_strat_solid;
	CHECK(listed);
}

/* The filled-shape blind spot: a big flat panel's interior is content,
 * on light and dark slides alike. */
static void check_filled_panel(uint8_t bg_r, uint8_t bg_g, uint8_t bg_b,
			       uint8_t fg_r, uint8_t fg_g, uint8_t fg_b)
{
	struct image im = img_new();
	img_rect(&im, 0, 0, W, H, bg_r, bg_g, bg_b);
	img_rect(&im, 170, 90, 300, 170, fg_r, fg_g, fg_b);
	img_text(&im, 20, 20, 150, 60, bg_r > 128 ? 20 : 235);

	struct busy_analyzer *any = run(&im, BUSY_STRATEGY_ANY);
	struct busy_analyzer *b = run(&im, BUSY_STRATEGY_SOLID);

	/* Inside the panel: invisible to edges, content here. */
	CHECK(busy_rect_score(any, 190, 105, 80, 50) == 0.0);
	CHECK(busy_rect_score(b, 190, 105, 80, 50) == 1.0);
	/* Plain background stays free; text is still content. */
	CHECK(busy_rect_score(b, 10, 100, 140, 70) == 0.0);
	CHECK(busy_rect_score(b, 170, 10, 140, 60) == 0.0);
	CHECK(busy_rect_score(b, 20, 20, 130, 40) > 0.3);

	busy_destroy(any);
	busy_destroy(b);
	img_free(&im);
}

static void test_filled_shapes(void)
{
	check_filled_panel(255, 255, 255, 40, 90, 200);  /* blue on white */
	check_filled_panel(30, 30, 40, 200, 60, 60);     /* red on dark */
	check_filled_panel(30, 30, 40, 50, 52, 64);      /* subtle on dark */
	/* Light-grey code block on white: even its outline is below the
	 * edge threshold (luma step 15), but it differs by L1 45. */
	check_filled_panel(255, 255, 255, 240, 240, 240);
}

/* Linear gradients (gentle and steep, either direction) with no content
 * score ~0; a panel on the gentle one is still content. */
static void test_gradients(void)
{
	for (int k = 0; k < 4; k++) {
		struct image im = img_new();
		for (uint32_t y = 0; y < H; y++) {
			for (uint32_t x = 0; x < W; x++) {
				int v;
				if (k == 0) /* gentle, horizontal, light */
					v = 200 + (int)(55 * x / (W - 1));
				else if (k == 1) /* steep, horizontal */
					v = 40 + (int)(160 * x / (W - 1));
				else if (k == 2) /* gentle, vertical, dark */
					v = 20 + (int)(60 * y / (H - 1));
				else /* coloured diagonal */
					v = (int)(100 * (x + y) / (W + H));
				img_set(&im, x, y, clamp8(v),
					clamp8(k == 3 ? 180 - v : v),
					clamp8(k == 3 ? 120 + v / 2 : v));
			}
		}
		struct busy_analyzer *b = run(&im, BUSY_STRATEGY_SOLID);
		for (int q = 0; q < 4; q++)
			CHECK(quadrant(b, q) < 0.005);
		CHECK(busy_rect_score(b, 0, 0, W, H) < 0.005);
		busy_destroy(b);

		if (k == 0) {
			img_rect(&im, 40, 40, 140, 140, 40, 90, 200);
			b = run(&im, BUSY_STRATEGY_SOLID);
			CHECK(busy_rect_score(b, 60, 60, 60, 60) == 1.0);
			CHECK(busy_rect_score(b, 180, 20, 120, 140) == 0.0);
			busy_destroy(b);
		}
		img_free(&im);
	}
}

/* Full-bleed noise / photo: no dominant colour, busy everywhere — at
 * least as busy as "Any content". */
static void test_full_bleed_photo(void)
{
	struct image im = img_new();
	for (uint32_t y = 0; y < H; y++)
		for (uint32_t x = 0; x < W; x++) {
			uint8_t v = (uint8_t)(rng() & 0xff);
			img_set(&im, x, y, v, (uint8_t)(255 - v),
				(uint8_t)(rng() & 0xff));
		}
	struct busy_analyzer *any = run(&im, BUSY_STRATEGY_ANY);
	struct busy_analyzer *b = run(&im, BUSY_STRATEGY_SOLID);
	for (int q = 0; q < 4; q++) {
		CHECK(quadrant(b, q) > 0.9);
		CHECK(quadrant(b, q) >= quadrant(any, q));
	}
	busy_destroy(any);
	busy_destroy(b);

	/* Smooth "photo" (low-frequency blobs, no hard edges anywhere):
	 * local contrast still marks the blobs. */
	for (uint32_t y = 0; y < H; y++)
		for (uint32_t x = 0; x < W; x++) {
			int v = 128 + (int)(90 * sin(x / 9.0) * cos(y / 7.0));
			img_set(&im, x, y, clamp8(v), clamp8(v / 2 + 40),
				clamp8(255 - v));
		}
	b = run(&im, BUSY_STRATEGY_SOLID);
	for (int q = 0; q < 4; q++)
		CHECK(quadrant(b, q) > 0.3);
	busy_destroy(b);
	img_free(&im);
}

/* Two-tone template: a dark title bar across the top and a coloured band
 * down the left are background; text on them is still content. A panel
 * that doesn't touch the frame border is not a second background. */
static void test_two_tone(void)
{
	struct image im = img_new();
	img_rect(&im, 0, 0, W, H, 250, 250, 250);
	img_rect(&im, 0, 0, W, 40, 20, 30, 60);
	img_text(&im, 10, 10, 120, 27, 230);
	img_text(&im, 40, 70, 300, 110, 20);

	struct busy_analyzer *b = run(&im, BUSY_STRATEGY_SOLID);
	CHECK(busy_rect_score(b, 150, 2, 160, 34) == 0.0); /* empty bar */
	CHECK(busy_rect_score(b, 10, 10, 110, 17) > 0.3);  /* bar text */
	CHECK(busy_rect_score(b, 0, 120, W, 60) == 0.0);   /* body */
	CHECK(busy_rect_score(b, 40, 70, 260, 40) > 0.3);  /* body text */
	busy_destroy(b);

	/* Side band. */
	img_rect(&im, 0, 0, W, H, 250, 250, 250);
	img_rect(&im, 0, 0, 90, H, 200, 80, 40);
	img_text(&im, 120, 30, 300, 80, 20);
	b = run(&im, BUSY_STRATEGY_SOLID);
	CHECK(busy_rect_score(b, 2, 2, 86, H - 4) == 0.0);
	CHECK(busy_rect_score(b, 100, 100, 210, 70) == 0.0);
	busy_destroy(b);

	/* Same colour as a big panel inside the slide: content. */
	img_rect(&im, 0, 0, W, H, 250, 250, 250);
	img_rect(&im, 60, 30, 260, 150, 200, 80, 40);
	b = run(&im, BUSY_STRATEGY_SOLID);
	CHECK(busy_rect_score(b, 80, 50, 160, 80) == 1.0);
	CHECK(busy_rect_score(b, 0, 155, W, 25) == 0.0);
	busy_destroy(b);
	img_free(&im);
}

/* The dark backdrop of a side photo is not one flat colour, so it isn't
 * taken as a second background even though it is large and on the
 * border. */
static void test_photo_backdrop(void)
{
	struct image im = img_new();
	img_rect(&im, 0, 0, W, H, 255, 255, 255);
	for (uint32_t y = 0; y < H; y++)
		for (uint32_t x = 200; x < W; x++) {
			int v = 44 + (int)(rng() % 11) - 5 + (int)(x - 200) / 20;
			img_set(&im, x, y, clamp8(v), clamp8(v), clamp8(v + 2));
		}
	img_text(&im, 20, 20, 180, 60, 20);
	struct busy_analyzer *b = run(&im, BUSY_STRATEGY_SOLID);
	CHECK(busy_rect_score(b, 210, 20, 100, 140) == 1.0);
	CHECK(busy_rect_score(b, 10, 80, 180, 90) == 0.0);
	busy_destroy(b);
	img_free(&im);
}

/* Motion is content; BGRA gives the same map as RGBA. */
static void test_motion_and_bgra(void)
{
	struct image a = img_new(), c = img_new();
	img_rect(&a, 0, 0, W, H, 240, 245, 250);
	img_rect(&a, 20, 20, 120, 80, 200, 40, 30);
	memcpy(c.px, a.px, (size_t)a.linesize * a.h);
	/* A faint moving square (within T_BG, no edges): only motion. */
	img_rect(&c, 200, 100, 240, 140, 232, 237, 242);

	struct busy_analyzer *b = busy_create(W, H);
	struct busy_params p;
	busy_params_default(&p);
	p.strategy = BUSY_STRATEGY_SOLID;
	p.t_motion = 4;
	busy_process(b, a.px, a.linesize, BUSY_FMT_RGBA, &p);
	CHECK(busy_rect_score(b, 200, 100, 40, 40) == 0.0);
	busy_process(b, c.px, c.linesize, BUSY_FMT_RGBA, &p);
	CHECK(busy_rect_score(b, 200, 100, 40, 40) == 1.0);
	p.use_motion = false;
	busy_process(b, c.px, c.linesize, BUSY_FMT_RGBA, &p);
	CHECK(busy_rect_score(b, 200, 100, 40, 40) == 0.0);
	busy_destroy(b);

	/* BGRA: swap R and B in a copy. */
	for (size_t i = 0; i < (size_t)W * H; i++) {
		c.px[4 * i + 0] = a.px[4 * i + 2];
		c.px[4 * i + 1] = a.px[4 * i + 1];
		c.px[4 * i + 2] = a.px[4 * i + 0];
		c.px[4 * i + 3] = 255;
	}
	struct busy_analyzer *x = busy_create(W, H), *y = busy_create(W, H);
	p.use_motion = true;
	busy_process(x, a.px, a.linesize, BUSY_FMT_RGBA, &p);
	busy_process(y, c.px, c.linesize, BUSY_FMT_BGRA, &p);
	CHECK(!memcmp(x->cost, y->cost, (size_t)W * H));
	CHECK(busy_rect_score(x, 30, 30, 80, 40) == 1.0);
	busy_destroy(x);
	busy_destroy(y);
	img_free(&a);
	img_free(&c);
}

/* Other analysis sizes, reset and strategy switching don't break it. */
static void test_sizes_and_reset(void)
{
	const uint32_t sizes[][2] = {{1, 1}, {7, 3}, {160, 90}, {480, 270}};
	for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
		uint32_t w = sizes[k][0], h = sizes[k][1];
		uint8_t *px = malloc((size_t)w * h * 4);
		if (!px)
			exit(2);
		for (uint32_t i = 0; i < w * h; i++) {
			uint32_t x = i % w, y = i / w;
			bool in = x > w / 4 && x < w / 2 && y > h / 4 && y < h / 2;
			px[4 * i + 0] = in ? 30 : 250;
			px[4 * i + 1] = in ? 60 : 250;
			px[4 * i + 2] = in ? 200 : 250;
			px[4 * i + 3] = 255;
		}
		struct busy_analyzer *b = busy_create(w, h);
		struct busy_params p;
		busy_params_default(&p);
		p.strategy = BUSY_STRATEGY_SOLID;
		busy_process(b, px, w * 4, BUSY_FMT_RGBA, &p);
		busy_reset_history(b);
		busy_process(b, px, w * 4, BUSY_FMT_RGBA, &p);
		CHECK(b->strategy == &busy_strat_solid);
		CHECK(busy_rect_score(b, 3 * w / 4.0, 3 * h / 4.0, w / 4.0,
				      h / 4.0) == 0.0);
		if (w >= 160) {
			CHECK(busy_rect_score(b, w / 4.0 + 4, h / 4.0 + 4,
					      w / 4.0 - 8, h / 4.0 - 8) == 1.0);
		}
		p.strategy = BUSY_STRATEGY_ANY;
		busy_process(b, px, w * 4, BUSY_FMT_RGBA, &p);
		p.strategy = BUSY_STRATEGY_SOLID;
		busy_process(b, px, w * 4, BUSY_FMT_RGBA, &p);
		CHECK(b->strategy == &busy_strat_solid);
		busy_destroy(b);
		free(px);
	}
}

int main(void)
{
	test_registry();
	test_filled_shapes();
	test_gradients();
	test_full_bleed_photo();
	test_two_tone();
	test_photo_backdrop();
	test_motion_and_bgra();
	test_sizes_and_reset();

	if (failures) {
		fprintf(stderr, "test_strat_solid: %d of %d checks FAILED\n",
			failures, checks);
		return 1;
	}
	printf("test_strat_solid: all %d checks passed\n", checks);
	return 0;
}
