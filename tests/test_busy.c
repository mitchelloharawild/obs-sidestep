/*
 * obs-sidestep — unit tests for core/busy.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "busy.h"

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
				sum += b->mask[(size_t)y * im.w + x];
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
	CHECK(b->mask[0] == 1);
	CHECK(b->mask[(size_t)im.w * im.h - 1] == 0);

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
	CHECK(memcmp(a->mask, c->mask, (size_t)w * h) == 0);

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
	CHECK(memcmp(a->mask, c->mask, (size_t)w * h) == 0);
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

	if (failures) {
		fprintf(stderr, "test_busy: %d of %d checks FAILED\n", failures,
			checks);
		return 1;
	}
	printf("test_busy: all %d checks passed\n", checks);
	return 0;
}
