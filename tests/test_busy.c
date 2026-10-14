/*
 * obs-sidestep — unit tests for core/busy.c
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

struct image {
	uint32_t w, h, linesize;
	uint8_t *px;
};

static struct image img_new(uint32_t w, uint32_t h, uint32_t linesize)
{
	struct image im = {w, h, linesize, NULL};
	im.px = calloc((size_t)linesize * h, 1);
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

static void img_fill(struct image *im, uint32_t x0, uint32_t y0, uint32_t x1,
		     uint32_t y1, uint8_t v)
{
	for (uint32_t y = y0; y < y1 && y < im->h; y++)
		for (uint32_t x = x0; x < x1 && x < im->w; x++)
			img_set(im, x, y, v, v, v);
}

static uint32_t rng_state = 12345;
static uint32_t rng(void)
{
	rng_state = rng_state * 1664525u + 1013904223u;
	return rng_state >> 8;
}

/* Fake text: rows of "glyphs" (small dark blobs with gaps) on the current
 * background. */
static void img_text(struct image *im, uint32_t x0, uint32_t y0, uint32_t x1,
		     uint32_t y1, uint8_t ink)
{
	for (uint32_t y = y0; y < y1; y++) {
		uint32_t ly = (y - y0) % 10; /* 10 px line pitch */
		if (ly >= 7)
			continue; /* line gap */
		for (uint32_t x = x0; x < x1; x++) {
			uint32_t gx = (x - x0) % 6; /* 6 px glyph pitch */
			if (gx < 4 && ((gx + ly) % 3 != 0))
				img_set(im, x, y, ink, ink, ink);
		}
	}
}

static double quadrant(const struct busy_analyzer *b, int q)
{
	double hw = b->w / 2.0, hh = b->h / 2.0;
	return busy_rect_score(b, (q & 1) ? hw : 0, (q & 2) ? hh : 0, hw, hh);
}

static struct busy_params defaults(void)
{
	struct busy_params p;
	busy_params_default(&p);
	return p;
}

/* ------------------------------------------------------------------------ */

static void test_params_and_create(void)
{
	struct busy_params p = defaults();
	CHECK(p.t_edge == 24);
	CHECK(p.t_motion == 24);
	CHECK(p.use_motion == true);
	CHECK(p.strategy == BUSY_STRATEGY_ANY);

	CHECK(busy_create(0, 10) == NULL);
	CHECK(busy_create(10, 0) == NULL);

	struct busy_analyzer *b = busy_create(16, 9);
	CHECK(b != NULL);
	if (b) {
		CHECK(b->w == 16 && b->h == 9);
		CHECK(!b->has_prev);
	}
	busy_destroy(b);
	busy_destroy(NULL);
}

static void test_blank(void)
{
	struct image im = img_new(320, 180, 320 * 4);
	img_fill(&im, 0, 0, im.w, im.h, 255);
	struct busy_analyzer *b = busy_create(im.w, im.h);
	struct busy_params p = defaults();

	busy_process(b, im.px, im.linesize, BUSY_FMT_RGBA, &p);
	CHECK(b->luma[0] == 255);
	for (int q = 0; q < 4; q++)
		CHECK(quadrant(b, q) == 0.0);
	CHECK(busy_rect_score(b, 0, 0, im.w, im.h) == 0.0);
	CHECK(b->sat[(size_t)(im.w + 1) * (im.h + 1) - 1] == 0);

	busy_destroy(b);
	img_free(&im);
}

static void test_gradient(void)
{
	struct image im = img_new(320, 180, 320 * 4);
	/* Diagonal gradient plus a vignette-like radial falloff. */
	for (uint32_t y = 0; y < im.h; y++) {
		for (uint32_t x = 0; x < im.w; x++) {
			double dx = x - 160.0, dy = y - 90.0;
			double v = 40.0 + 150.0 * x / im.w + 50.0 * y / im.h -
				   0.002 * (dx * dx + dy * dy);
			uint8_t c = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
			img_set(&im, x, y, c, (uint8_t)(c / 2), (uint8_t)(255 - c));
		}
	}
	struct busy_analyzer *b = busy_create(im.w, im.h);
	struct busy_params p = defaults();
	busy_process(b, im.px, im.linesize, BUSY_FMT_RGBA, &p);
	double s = busy_rect_score(b, 0, 0, im.w, im.h);
	CHECK(s < 0.001);
	busy_destroy(b);
	img_free(&im);
}

static void test_text_corner(void)
{
	struct image im = img_new(320, 180, 320 * 4);
	img_fill(&im, 0, 0, im.w, im.h, 250);
	img_text(&im, 8, 8, 140, 70, 20);

	struct busy_analyzer *b = busy_create(im.w, im.h);
	struct busy_params p = defaults();
	busy_process(b, im.px, im.linesize, BUSY_FMT_RGBA, &p);

	double tl = quadrant(b, 0);
	CHECK(tl > 0.1);
	CHECK(quadrant(b, 1) == 0.0);
	CHECK(quadrant(b, 2) == 0.0);
	CHECK(quadrant(b, 3) == 0.0);
	/* Camera-sized rects in the other corners are clear. */
	CHECK(busy_rect_score(b, 240, 0, 80, 45) == 0.0);
	CHECK(busy_rect_score(b, 0, 135, 80, 45) == 0.0);
	CHECK(busy_rect_score(b, 240, 135, 80, 45) == 0.0);
	CHECK(busy_rect_score(b, 0, 0, 80, 45) > 0.2);

	busy_destroy(b);
	img_free(&im);
}

static void test_light_grey_text(void)
{
	struct image im = img_new(320, 180, 320 * 4);
	img_fill(&im, 0, 0, im.w, im.h, 255);
	img_text(&im, 180, 110, 310, 170, 215); /* low contrast: dY = 40 */

	struct busy_analyzer *b = busy_create(im.w, im.h);
	struct busy_params p = defaults();
	busy_process(b, im.px, im.linesize, BUSY_FMT_RGBA, &p);

	double br = quadrant(b, 3);
	CHECK(br > 0.1);
	CHECK(quadrant(b, 0) == 0.0);

	/* Same layout in black ink: binary mask => identical score. */
	struct image im2 = img_new(320, 180, 320 * 4);
	img_fill(&im2, 0, 0, im2.w, im2.h, 255);
	img_text(&im2, 180, 110, 310, 170, 0);
	struct busy_analyzer *b2 = busy_create(im2.w, im2.h);
	busy_process(b2, im2.px, im2.linesize, BUSY_FMT_RGBA, &p);
	CHECK_NEAR(quadrant(b2, 3), br, 1e-12);

	busy_destroy(b);
	busy_destroy(b2);
	img_free(&im);
	img_free(&im2);
}

static void test_noise_photo(void)
{
	struct image im = img_new(320, 180, 320 * 4);
	/* Photo-like texture: blocky random luminance with fine noise. */
	for (uint32_t y = 0; y < im.h; y++)
		for (uint32_t x = 0; x < im.w; x++) {
			uint8_t v = (uint8_t)(rng() & 0xff);
			img_set(&im, x, y, v, (uint8_t)(255 - v),
				(uint8_t)(rng() & 0xff));
		}
	struct busy_analyzer *b = busy_create(im.w, im.h);
	struct busy_params p = defaults();
	busy_process(b, im.px, im.linesize, BUSY_FMT_RGBA, &p);

	for (int q = 0; q < 4; q++)
		CHECK(quadrant(b, q) > 0.5);
	CHECK(busy_rect_score(b, 0, 0, im.w, im.h) > 0.5);
	CHECK(busy_rect_score(b, 0, 0, im.w, im.h) <= 1.0);

	/* SAT agrees with a brute-force count for random rects. */
	int bad = 0;
	for (int i = 0; i < 500; i++) {
		uint32_t x0 = rng() % im.w, y0 = rng() % im.h;
		uint32_t rw = 1 + rng() % (im.w - x0);
		uint32_t rh = 1 + rng() % (im.h - y0);
		uint32_t sum = 0;
		for (uint32_t y = y0; y < y0 + rh; y++)
			for (uint32_t x = x0; x < x0 + rw; x++)
				sum += b->cost[(size_t)y * im.w + x] / 255;
		double want = (double)sum / ((double)rw * rh);
		if (fabs(busy_rect_score(b, x0, y0, rw, rh) - want) > 1e-12)
			bad++;
	}
	CHECK(bad == 0);

	busy_destroy(b);
	img_free(&im);
}

static void test_motion(void)
{
	struct image f1 = img_new(320, 180, 320 * 4);
	struct image f2 = img_new(320, 180, 320 * 4);
	img_fill(&f1, 0, 0, f1.w, f1.h, 100);
	img_fill(&f2, 0, 0, f2.w, f2.h, 100);
	img_fill(&f1, 200, 100, 300, 150, 100); /* flat "video" region */
	img_fill(&f2, 200, 100, 300, 150, 160); /* brighter next capture */

	struct busy_analyzer *b = busy_create(f1.w, f1.h);
	struct busy_params p = defaults();

	busy_process(b, f1.px, f1.linesize, BUSY_FMT_RGBA, &p);
	CHECK(b->has_prev);
	CHECK(b->global_change == 1.0);
	CHECK(busy_rect_score(b, 210, 110, 80, 30) == 0.0);

	busy_process(b, f2.px, f2.linesize, BUSY_FMT_RGBA, &p);
	/* Interior is flat: only motion can make it busy. */
	CHECK(busy_rect_score(b, 210, 110, 80, 30) == 1.0);
	CHECK(busy_rect_score(b, 0, 0, 160, 90) == 0.0);
	CHECK_NEAR(b->global_change, (100.0 * 50.0) / (320.0 * 180.0), 1e-12);

	/* Without motion: interior is not busy, but global_change is still
	 * computed. */
	struct busy_params nm = p;
	nm.use_motion = false;
	busy_reset_history(b);
	CHECK(!b->has_prev);
	busy_process(b, f1.px, f1.linesize, BUSY_FMT_RGBA, &nm);
	busy_process(b, f2.px, f2.linesize, BUSY_FMT_RGBA, &nm);
	CHECK(busy_rect_score(b, 210, 110, 80, 30) == 0.0);
	CHECK(busy_rect_score(b, 200, 100, 100, 50) > 0.0); /* region edges */
	CHECK_NEAR(b->global_change, (100.0 * 50.0) / (320.0 * 180.0), 1e-12);

	/* Below-threshold brightness change: no motion. */
	struct image f3 = img_new(320, 180, 320 * 4);
	img_fill(&f3, 0, 0, f3.w, f3.h, 110);
	busy_reset_history(b);
	busy_process(b, f1.px, f1.linesize, BUSY_FMT_RGBA, &p);
	busy_process(b, f3.px, f3.linesize, BUSY_FMT_RGBA, &p);
	CHECK(b->global_change == 0.0);
	CHECK(busy_rect_score(b, 0, 0, 320, 180) == 0.0);

	busy_destroy(b);
	img_free(&f1);
	img_free(&f2);
	img_free(&f3);
}

static void test_global_change(void)
{
	struct image im = img_new(64, 36, 64 * 4);
	for (uint32_t y = 0; y < im.h; y++)
		for (uint32_t x = 0; x < im.w; x++)
			img_set(&im, x, y, (uint8_t)rng(), (uint8_t)rng(),
				(uint8_t)rng());
	struct busy_analyzer *b = busy_create(im.w, im.h);
	struct busy_params p = defaults();

	busy_process(b, im.px, im.linesize, BUSY_FMT_RGBA, &p);
	CHECK(b->global_change == 1.0);
	busy_process(b, im.px, im.linesize, BUSY_FMT_RGBA, &p);
	CHECK(b->global_change == 0.0);
	busy_process(b, im.px, im.linesize, BUSY_FMT_RGBA, &p);
	CHECK(b->global_change == 0.0);
	busy_reset_history(b);
	busy_process(b, im.px, im.linesize, BUSY_FMT_RGBA, &p);
	CHECK(b->global_change == 1.0);

	/* Inverted frame: (nearly) every pixel changes. */
	struct image inv = img_new(64, 36, 64 * 4);
	img_fill(&inv, 0, 0, inv.w, inv.h, 0);
	struct image white = img_new(64, 36, 64 * 4);
	img_fill(&white, 0, 0, white.w, white.h, 255);
	busy_process(b, inv.px, inv.linesize, BUSY_FMT_RGBA, &p);
	busy_process(b, white.px, white.linesize, BUSY_FMT_RGBA, &p);
	CHECK(b->global_change == 1.0);

	busy_destroy(b);
	img_free(&im);
	img_free(&inv);
	img_free(&white);
}

static void test_outside_rects(void)
{
	/* Checkerboard: every pixel is an edge except the bottom-right one
	 * (no right or bottom neighbour). */
	struct image im = img_new(40, 20, 40 * 4);
	for (uint32_t y = 0; y < im.h; y++)
		for (uint32_t x = 0; x < im.w; x++) {
			uint8_t v = ((x + y) & 1) ? 255 : 0;
			img_set(&im, x, y, v, v, v);
		}
	struct busy_analyzer *b = busy_create(im.w, im.h);
	struct busy_params p = defaults();
	busy_process(b, im.px, im.linesize, BUSY_FMT_RGBA, &p);
	CHECK(b->cost[0] == 255);
	CHECK(b->cost[(size_t)im.w * im.h - 1] == 0);

	CHECK(busy_rect_score(b, 0, 0, 10, 10) == 1.0);
	/* Partially outside: 10x10 inside of a 20x20 rect. */
	CHECK_NEAR(busy_rect_score(b, -10, -10, 20, 20), 0.25, 1e-12);
	CHECK_NEAR(busy_rect_score(b, 35, -5, 10, 10), 25.0 / 100.0, 1e-12);
	/* Fully outside on each side. */
	CHECK(busy_rect_score(b, -50, 0, 20, 20) == 0.0);
	CHECK(busy_rect_score(b, 40, 0, 20, 20) == 0.0);
	CHECK(busy_rect_score(b, 0, -30, 20, 20) == 0.0);
	CHECK(busy_rect_score(b, 0, 20, 20, 20) == 0.0);
	CHECK(busy_rect_score(b, 1e300, 1e300, 1e300, 1e300) == 0.0);
	/* Enclosing rect: whole image (minus one pixel) over 4x the area. */
	CHECK_NEAR(busy_rect_score(b, -20, -10, 80, 40),
		   (40.0 * 20.0 - 1.0) / (80.0 * 40.0), 1e-12);
	/* Fractional bounds round outward: [0.5,1.5) -> pixels 0..1. */
	CHECK(busy_rect_score(b, 0.5, 0.5, 1.0, 1.0) == 1.0);
	CHECK_NEAR(busy_rect_score(b, -0.5, -0.5, 1.0, 1.0), 0.25, 1e-12);
	/* Zero / negative area. */
	CHECK(busy_rect_score(b, 5, 5, 0, 10) == 0.0);
	CHECK(busy_rect_score(b, 5, 5, 10, 0) == 0.0);
	CHECK(busy_rect_score(b, 5, 5, -3, 10) == 0.0);
	CHECK(busy_rect_score(b, 5, 5, NAN, 10) == 0.0);

	busy_destroy(b);
	img_free(&im);
}

static void test_bgra_equivalence(void)
{
	const uint32_t w = 96, h = 54;
	struct image rgba = img_new(w, h, w * 4);
	struct image bgra = img_new(w, h, w * 4);
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++) {
			uint8_t r = (uint8_t)rng(), g = (uint8_t)rng();
			uint8_t bl = (uint8_t)((x * 7 + y * 3) & 0xff);
			img_set(&rgba, x, y, r, g, bl);
			img_set(&bgra, x, y, bl, g, r); /* bytes B,G,R,A */
		}
	struct busy_analyzer *a = busy_create(w, h);
	struct busy_analyzer *c = busy_create(w, h);
	struct busy_params p = defaults();
	busy_process(a, rgba.px, rgba.linesize, BUSY_FMT_RGBA, &p);
	busy_process(c, bgra.px, bgra.linesize, BUSY_FMT_BGRA, &p);
	CHECK(memcmp(a->luma, c->luma, (size_t)w * h) == 0);
	CHECK(memcmp(a->cost, c->cost, (size_t)w * h) == 0);

	/* Luma formula spot checks. */
	CHECK(a->luma[0] ==
	      (uint8_t)((77 * rgba.px[0] + 150 * rgba.px[1] + 29 * rgba.px[2]) >>
			8));
	struct image pure = img_new(3, 1, 12);
	img_set(&pure, 0, 0, 255, 0, 0);
	img_set(&pure, 1, 0, 0, 255, 0);
	img_set(&pure, 2, 0, 0, 0, 255);
	struct busy_analyzer *d = busy_create(3, 1);
	busy_process(d, pure.px, pure.linesize, BUSY_FMT_RGBA, &p);
	CHECK(d->luma[0] == 76 && d->luma[1] == 149 && d->luma[2] == 28);
	busy_process(d, pure.px, pure.linesize, BUSY_FMT_BGRA, &p);
	CHECK(d->luma[0] == 28 && d->luma[1] == 149 && d->luma[2] == 76);

	busy_destroy(a);
	busy_destroy(c);
	busy_destroy(d);
	img_free(&rgba);
	img_free(&bgra);
	img_free(&pure);
}

static void test_linesize_padding(void)
{
	const uint32_t w = 50, h = 30;
	struct image tight = img_new(w, h, w * 4);
	struct image padded = img_new(w, h, w * 4 + 36);
	/* Fill padding with garbage that would be busy if read. */
	for (size_t i = 0; i < (size_t)padded.linesize * h; i++)
		padded.px[i] = (uint8_t)rng();
	img_fill(&tight, 0, 0, w, h, 200);
	img_text(&tight, 5, 5, 25, 25, 10);
	for (uint32_t y = 0; y < h; y++)
		memcpy(padded.px + (size_t)y * padded.linesize,
		       tight.px + (size_t)y * tight.linesize, (size_t)w * 4);

	struct busy_analyzer *a = busy_create(w, h);
	struct busy_analyzer *c = busy_create(w, h);
	struct busy_params p = defaults();
	busy_process(a, tight.px, tight.linesize, BUSY_FMT_RGBA, &p);
	busy_process(c, padded.px, padded.linesize, BUSY_FMT_RGBA, &p);
	CHECK(memcmp(a->luma, c->luma, (size_t)w * h) == 0);
	CHECK(memcmp(a->cost, c->cost, (size_t)w * h) == 0);
	CHECK(busy_rect_score(c, 30, 0, 20, 30) == 0.0);
	CHECK(busy_rect_score(c, 0, 0, 25, 25) > 0.1);

	busy_destroy(a);
	busy_destroy(c);
	img_free(&tight);
	img_free(&padded);
}

static void test_sensitivity(void)
{
	CHECK_NEAR(busy_sensitivity_to_threshold(0), 0.1, 1e-12);
	CHECK_NEAR(busy_sensitivity_to_threshold(50), 0.01, 1e-12);
	CHECK_NEAR(busy_sensitivity_to_threshold(100), 0.001, 1e-12);
	CHECK_NEAR(busy_sensitivity_to_threshold(-20), 0.1, 1e-12);
	CHECK_NEAR(busy_sensitivity_to_threshold(250), 0.001, 1e-12);
	CHECK(busy_sensitivity_to_threshold(25) <
	      busy_sensitivity_to_threshold(10));
}

/* ------------------------------------------------------------------------ */
/* Strategies                                                                */

static void test_registry(void)
{
	size_t n = busy_strategy_count();
	CHECK(n >= 2);
	CHECK(busy_strategy_get(0) == &busy_strat_any);
	CHECK(busy_strategy_get(n) == NULL);
	CHECK(busy_strategy_find(BUSY_STRATEGY_ANY) == &busy_strat_any);
	CHECK(busy_strategy_find(BUSY_STRATEGY_BG_IMAGE) == &busy_strat_bgimage);
	CHECK(busy_strategy_find(-1) == &busy_strat_any);
	CHECK(busy_strategy_find(9999) == &busy_strat_any);
	for (size_t i = 0; i < n; i++) {
		const struct busy_strategy_ops *s = busy_strategy_get(i);
		CHECK(s && s->process && s->name_key && s->desc_key);
		for (size_t j = 0; j < i; j++)
			CHECK(busy_strategy_get(j)->id != s->id);
	}
	CHECK(!busy_strat_any.prefers_images);
	CHECK(busy_strat_bgimage.prefers_images);
	CHECK(busy_modifier_get(busy_modifier_count()) == NULL);
}

static void test_cost_map(void)
{
	struct image im = img_new(320, 180, 320 * 4);
	img_fill(&im, 0, 0, im.w, im.h, 250);
	img_text(&im, 8, 8, 140, 70, 20);
	struct busy_analyzer *b = busy_create(im.w, im.h);
	struct busy_params p = defaults();
	busy_process(b, im.px, im.linesize, BUSY_FMT_RGBA, &p);

	/* ANY: cost is exactly 0 or 255, matching the edge rule. */
	bool binary = true, agrees = true;
	for (size_t i = 0, n = (size_t)im.w * im.h; i < n; i++) {
		binary = binary && (b->cost[i] == 0 || b->cost[i] == 255);
		agrees = agrees && (b->cost[i] == 255) == (b->edge[i] > p.t_edge);
	}
	CHECK(binary);
	CHECK(agrees);
	CHECK(b->strategy == &busy_strat_any);
	CHECK(!b->has_image);
	CHECK(busy_rect_image_frac(b, 0, 0, im.w, im.h) == 0.0);

	/* Unknown ids fall back to ANY with identical results. */
	struct busy_analyzer *c = busy_create(im.w, im.h);
	struct busy_params q = p;
	q.strategy = 4242;
	busy_process(c, im.px, im.linesize, BUSY_FMT_RGBA, &q);
	CHECK(c->strategy == &busy_strat_any);
	CHECK(memcmp(b->cost, c->cost, (size_t)im.w * im.h) == 0);
	CHECK(busy_rect_score(b, 0, 0, 160, 90) ==
	      busy_rect_score(c, 0, 0, 160, 90));

	/* Switching strategy, explicitly and through params. */
	CHECK(busy_set_strategy(c, BUSY_STRATEGY_BG_IMAGE));
	CHECK(c->strategy == &busy_strat_bgimage);
	CHECK(c->strategy_state != NULL);
	busy_reset_history(c);
	CHECK(c->strategy == &busy_strat_bgimage);
	busy_process(c, im.px, im.linesize, BUSY_FMT_RGBA, &q);
	CHECK(c->strategy == &busy_strat_any); /* q.strategy wins */
	CHECK(c->strategy_state == NULL);

	busy_destroy(b);
	busy_destroy(c);
	img_free(&im);
}

/* Continuous-tone "photo": smooth colour ramps plus fine noise, so almost
 * no pixel is exactly flat (like a downscaled photograph). */
static void img_photo(struct image *im, uint32_t x0, uint32_t y0, uint32_t x1,
		      uint32_t y1)
{
	for (uint32_t y = y0; y < y1; y++)
		for (uint32_t x = x0; x < x1; x++) {
			int v = 60 + (int)((x * 3 + y * 2) % 120) +
				(int)(rng() % 48);
			img_set(im, x, y, (uint8_t)v, (uint8_t)(v / 2 + 40),
				(uint8_t)(200 - v / 2));
		}
}

static void img_fill_rgb(struct image *im, uint32_t x0, uint32_t y0,
			 uint32_t x1, uint32_t y1, uint8_t r, uint8_t g,
			 uint8_t b)
{
	for (uint32_t y = y0; y < y1 && y < im->h; y++)
		for (uint32_t x = x0; x < x1 && x < im->w; x++)
			img_set(im, x, y, r, g, b);
}

static struct busy_params bgimage_params(void)
{
	struct busy_params p = defaults();
	p.strategy = BUSY_STRATEGY_BG_IMAGE;
	return p;
}

static void test_bgimage_side_photo(void)
{
	/* Letterbox slide: photo filling the right half, text on the left. */
	struct image im = img_new(320, 180, 320 * 4);
	img_fill(&im, 0, 0, im.w, im.h, 255);
	img_text(&im, 10, 10, 140, 80, 30);
	img_photo(&im, 160, 0, 320, 180);

	struct busy_analyzer *b = busy_create(im.w, im.h);
	struct busy_params p = bgimage_params();
	busy_process(b, im.px, im.linesize, BUSY_FMT_RGBA, &p);
	busy_process(b, im.px, im.linesize, BUSY_FMT_RGBA, &p);

	CHECK(b->strategy == &busy_strat_bgimage);
	CHECK(b->has_image);
	/* Camera-sized rect in the photo's bottom-right corner: free, image. */
	CHECK(busy_rect_score(b, 250, 130, 64, 36) == 0.0);
	CHECK(busy_rect_image_frac(b, 250, 130, 64, 36) == 1.0);
	CHECK(busy_rect_image_frac(b, 160, 0, 160, 180) > 0.95);
	/* Text column: busy, not image; its bottom-left corner stays clear. */
	CHECK(busy_rect_score(b, 10, 10, 130, 70) > 0.1);
	CHECK(busy_rect_image_frac(b, 0, 0, 150, 180) < 0.05);
	CHECK(busy_rect_score(b, 4, 130, 64, 36) == 0.0);
	/* Outside the frame counts as not image. */
	CHECK_NEAR(busy_rect_image_frac(b, 288, 0, 64, 36), 0.5, 1e-12);

	/* The same frame under ANY: the photo is all content. */
	struct busy_analyzer *a = busy_create(im.w, im.h);
	struct busy_params q = defaults();
	busy_process(a, im.px, im.linesize, BUSY_FMT_RGBA, &q);
	CHECK(busy_rect_score(a, 250, 130, 64, 36) > 0.1);
	CHECK(busy_rect_score(a, 10, 10, 130, 70) ==
	      busy_rect_score(b, 10, 10, 130, 70));

	busy_destroy(a);
	busy_destroy(b);
	img_free(&im);
}

static void test_bgimage_fill_and_small(void)
{
	/* Flat coloured panel along the right edge (FILL) and a small photo
	 * in the middle (inline figure): neither is a background image. */
	struct image im = img_new(320, 180, 320 * 4);
	img_fill(&im, 0, 0, im.w, im.h, 255);
	img_fill_rgb(&im, 200, 0, 320, 180, 40, 90, 160);
	img_photo(&im, 70, 70, 110, 100);

	struct busy_analyzer *b = busy_create(im.w, im.h);
	struct busy_params p = bgimage_params();
	busy_process(b, im.px, im.linesize, BUSY_FMT_RGBA, &p);

	CHECK(!b->has_image);
	CHECK(busy_rect_image_frac(b, 0, 0, im.w, im.h) == 0.0);
	/* The panel's flat interior counts as content (ANY misses it). */
	CHECK(busy_rect_score(b, 240, 60, 40, 40) == 1.0);
	CHECK(busy_rect_score(b, 75, 75, 30, 20) == 1.0);
	/* Background stays free, right up to the panel. */
	CHECK(busy_rect_score(b, 120, 0, 76, 180) == 0.0);

	struct busy_analyzer *a = busy_create(im.w, im.h);
	struct busy_params q = defaults();
	busy_process(a, im.px, im.linesize, BUSY_FMT_RGBA, &q);
	CHECK(busy_rect_score(a, 240, 60, 40, 40) == 0.0);

	busy_destroy(a);
	busy_destroy(b);
	img_free(&im);
}

static void test_bgimage_motion(void)
{
	/* A "video" filling the right half changes between captures: not an
	 * image to sit on. */
	struct image f1 = img_new(320, 180, 320 * 4);
	struct image f2 = img_new(320, 180, 320 * 4);
	img_fill(&f1, 0, 0, f1.w, f1.h, 255);
	img_fill(&f2, 0, 0, f2.w, f2.h, 255);
	img_photo(&f1, 160, 0, 320, 180);
	for (uint32_t y = 0; y < 180; y++)
		for (uint32_t x = 160; x < 320; x++) {
			const uint8_t *s = f1.px + (size_t)y * f1.linesize + 4 * x;
			img_set(&f2, x, y, (uint8_t)(255 - s[0]),
				(uint8_t)(255 - s[1]), (uint8_t)(255 - s[2]));
		}

	struct busy_analyzer *b = busy_create(320, 180);
	struct busy_params p = bgimage_params();
	busy_process(b, f1.px, f1.linesize, BUSY_FMT_RGBA, &p);
	CHECK(b->has_image); /* first capture: no motion information yet */
	busy_process(b, f2.px, f2.linesize, BUSY_FMT_RGBA, &p);
	CHECK(!b->has_image);
	CHECK(busy_rect_score(b, 250, 130, 64, 36) > 0.9);

	/* Static again: an image once more. */
	busy_process(b, f2.px, f2.linesize, BUSY_FMT_RGBA, &p);
	CHECK(b->has_image);
	CHECK(busy_rect_score(b, 250, 130, 64, 36) == 0.0);

	busy_destroy(b);
	img_free(&f1);
	img_free(&f2);
}

static void test_bgimage_no_dominant_bg(void)
{
	/* Full-bleed photo: no dominant colour, scored exactly like ANY. */
	struct image im = img_new(320, 180, 320 * 4);
	img_photo(&im, 0, 0, 320, 180);
	struct busy_analyzer *a = busy_create(im.w, im.h);
	struct busy_analyzer *b = busy_create(im.w, im.h);
	struct busy_params q = defaults();
	struct busy_params p = bgimage_params();
	busy_process(a, im.px, im.linesize, BUSY_FMT_RGBA, &q);
	busy_process(b, im.px, im.linesize, BUSY_FMT_RGBA, &p);
	CHECK(!b->has_image);
	CHECK(memcmp(a->cost, b->cost, (size_t)im.w * im.h) == 0);

	/* Other resolutions and BGRA work too. */
	for (uint32_t w = 160; w <= 480; w += 160) {
		uint32_t h = w * 9 / 16;
		struct image s = img_new(w, h, w * 4);
		img_fill(&s, 0, 0, w, h, 255);
		img_photo(&s, 0, 0, w / 2, h);
		struct busy_analyzer *c = busy_create(w, h);
		busy_process(c, s.px, s.linesize, BUSY_FMT_BGRA, &p);
		CHECK(c->has_image);
		CHECK(busy_rect_image_frac(c, 0, h / 2.0, w / 5.0, h / 5.0) ==
		      1.0);
		CHECK(busy_rect_image_frac(c, w * 0.6, 0, w * 0.4, h) == 0.0);
		busy_destroy(c);
		img_free(&s);
	}

	busy_destroy(a);
	busy_destroy(b);
	img_free(&im);
}

int main(void)
{
	test_params_and_create();
	test_blank();
	test_gradient();
	test_text_corner();
	test_light_grey_text();
	test_noise_photo();
	test_motion();
	test_global_change();
	test_outside_rects();
	test_bgra_equivalence();
	test_linesize_padding();
	test_sensitivity();
	test_registry();
	test_cost_map();
	test_bgimage_side_photo();
	test_bgimage_fill_and_small();
	test_bgimage_motion();
	test_bgimage_no_dominant_bg();

	if (failures) {
		fprintf(stderr, "test_busy: %d of %d checks FAILED\n", failures,
			checks);
		return 1;
	}
	printf("test_busy: all %d checks passed\n", checks);
	return 0;
}
