/*
 * obs-sidestep — unit tests for the "Ignore template elements" modifier
 * (core/mod_template.c), run under several strategies.
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

#define W 320
#define H 180
#define LS (W * 4)

/* Logo, bottom-right (tile aligned at 4 px). */
#define LOGO_X0 280
#define LOGO_Y0 148
#define LOGO_X1 312
#define LOGO_Y1 172
/* Slide text lives in this area. */
#define TEXT_X0 8
#define TEXT_Y0 8
#define TEXT_X1 260
#define TEXT_Y1 136

static uint8_t frame[LS * H];

static uint32_t rng_state;
static uint32_t rng(void)
{
	rng_state = rng_state * 1664525u + 1013904223u;
	return rng_state >> 8;
}

static void fill(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint8_t r, uint8_t g, uint8_t b)
{
	for (uint32_t y = y0; y < y1; y++) {
		for (uint32_t x = x0; x < x1; x++) {
			uint8_t *p = frame + (size_t)y * LS + 4 * x;
			p[0] = r;
			p[1] = g;
			p[2] = b;
			p[3] = 255;
		}
	}
}

static void draw_logo(void)
{
	fill(LOGO_X0, LOGO_Y0, LOGO_X1, LOGO_Y1, 20, 40, 140);
	for (uint32_t x = LOGO_X0 + 4; x + 2 < LOGO_X1; x += 6)
		fill(x, LOGO_Y0 + 4, x + 2, LOGO_Y1 - 4, 250, 250, 250);
}

/* Random "text lines" of glyph noise in the text area, from `seed`. */
static void draw_text(uint32_t seed)
{
	rng_state = seed * 2654435761u + 12345u;
	for (int line = 0; line < 6; line++) {
		uint32_t y0 = TEXT_Y0 + (uint32_t)line * 20 + rng() % 6;
		uint32_t x0 = TEXT_X0 + rng() % 40;
		uint32_t x1 = x0 + 60 + rng() % (TEXT_X1 - x0 - 60);
		for (uint32_t y = y0; y < y0 + 10 && y < TEXT_Y1; y++) {
			for (uint32_t x = x0; x < x1; x++) {
				if (rng() % 3 == 0)
					fill(x, y, x + 1, y + 1, 10, 10, 10);
			}
		}
	}
}

enum {
	WITH_LOGO = 1,
	WITH_SIDEBAR = 2,
};

/* Slide `n`: white background, slide text, optional logo / sidebar. */
static void make_slide(uint32_t n, int flags)
{
	fill(0, 0, W, H, 255, 255, 255);
	if (flags & WITH_SIDEBAR) {
		/* Persistent agenda sidebar: same glyph noise every slide;
		 * slide text moves right of it. */
		rng_state = 999;
		for (uint32_t y = 8; y < 136; y++)
			for (uint32_t x = 8; x < 140; x++)
				if (rng() % 3 == 0)
					fill(x, y, x + 1, y + 1, 10, 10, 10);
		rng_state = n * 2654435761u + 7u;
		for (int line = 0; line < 5; line++) {
			uint32_t y0 = 10 + (uint32_t)line * 22 + rng() % 6;
			for (uint32_t y = y0; y < y0 + 10; y++)
				for (uint32_t x = 150; x < 260; x++)
					if (rng() % 3 == 0)
						fill(x, y, x + 1, y + 1, 10, 10, 10);
		}
	} else {
		draw_text(n);
	}
	if (flags & WITH_LOGO)
		draw_logo();
}

static void make_transition(void)
{
	fill(0, 0, W, H, 128, 128, 128);
}

/* A test analyzer (modifier on) and a baseline (modifier off) fed the
 * same frames. */
struct pair {
	struct busy_analyzer *on, *off;
	struct busy_params pon, poff;
};

static void pair_init(struct pair *pr, int strategy)
{
	pr->on = busy_create(W, H);
	pr->off = busy_create(W, H);
	if (!pr->on || !pr->off) {
		fprintf(stderr, "out of memory\n");
		exit(2);
	}
	busy_params_default(&pr->pon);
	busy_params_default(&pr->poff);
	pr->pon.strategy = pr->poff.strategy = strategy;
	pr->pon.ignore_template = true;
}

static void pair_free(struct pair *pr)
{
	busy_destroy(pr->on);
	busy_destroy(pr->off);
}

static void pair_process(struct pair *pr)
{
	busy_process(pr->on, frame, LS, BUSY_FMT_RGBA, &pr->pon);
	busy_process(pr->off, frame, LS, BUSY_FMT_RGBA, &pr->poff);
}

/* Show slide n after a transition: transition, slide, slide (settled). */
static void pair_slide(struct pair *pr, uint32_t n, int flags)
{
	make_transition();
	pair_process(pr);
	make_slide(n, flags);
	pair_process(pr);
	pair_process(pr);
}

static double logo_score(const struct busy_analyzer *b)
{
	return busy_rect_score(b, LOGO_X0, LOGO_Y0, LOGO_X1 - LOGO_X0, LOGO_Y1 - LOGO_Y0);
}

static bool same_cost(const struct pair *pr)
{
	return memcmp(pr->on->cost, pr->off->cost, (size_t)W * H) == 0;
}

/* Cost identical to the baseline outside the logo (plus one tile). */
static bool same_cost_outside_logo(const struct pair *pr)
{
	for (uint32_t y = 0; y < H; y++) {
		for (uint32_t x = 0; x < W; x++) {
			if (x + 4 >= LOGO_X0 && x < LOGO_X1 + 4 && y + 4 >= LOGO_Y0 && y < LOGO_Y1 + 4)
				continue;
			size_t i = (size_t)y * W + x;
			if (pr->on->cost[i] != pr->off->cost[i])
				return false;
		}
	}
	return true;
}

/* ------------------------------------------------------------------------ */

static void test_logo_learnt(int strategy)
{
	struct pair pr;
	pair_init(&pr, strategy);

	/* Slide 0 is the first settled slide (reference); slides 1..3 are
	 * slide changes 1..3. */
	for (uint32_t n = 0; n < 40; n++) {
		pair_slide(&pr, n, WITH_LOGO);
		double base = logo_score(pr.off);
		double got = logo_score(pr.on);
		CHECK(base > 0.1);
		if (n < 3) {
			CHECK(same_cost(&pr));
		} else {
			/* Template: at most cost 1 of 255 anywhere. */
			CHECK(got <= 1.0 / 255.0);
			CHECK(got > 0.0); /* still not "truly empty" */
		}
		/* Slide text is never discounted. */
		CHECK(same_cost_outside_logo(&pr));
		CHECK(busy_rect_score(pr.on, TEXT_X0, TEXT_Y0, TEXT_X1 - TEXT_X0, TEXT_Y1 - TEXT_Y0) ==
		      busy_rect_score(pr.off, TEXT_X0, TEXT_Y0, TEXT_X1 - TEXT_X0, TEXT_Y1 - TEXT_Y0));
	}

	/* Mid-transition frame: the logo is gone, nothing discounted. */
	make_transition();
	pair_process(&pr);
	CHECK(same_cost(&pr));

	/* Content that appears over the template area without a slide
	 * change (e.g. a subtle change that didn't trigger) is not
	 * discounted: the tile no longer matches the reference. */
	make_slide(40, WITH_LOGO);
	pair_process(&pr);
	pair_process(&pr);
	CHECK(logo_score(pr.on) <= 1.0 / 255.0);
	fill(LOGO_X0, LOGO_Y0, LOGO_X1, LOGO_Y1, 255, 255, 255);
	for (uint32_t y = LOGO_Y0 + 2; y < LOGO_Y1 - 2; y += 4)
		fill(LOGO_X0 + 2, y, LOGO_X1 - 2, y + 2, 0, 0, 0);
	pair_process(&pr);
	pair_process(&pr);
	CHECK(same_cost(&pr));

	pair_free(&pr);
}

static void test_disabled(int strategy)
{
	struct pair pr;
	pair_init(&pr, strategy);
	pr.pon.ignore_template = false;
	for (uint32_t n = 0; n < 8; n++) {
		pair_slide(&pr, n, WITH_LOGO);
		CHECK(same_cost(&pr));
	}
	pair_free(&pr);
}

static void test_reset(int strategy)
{
	struct pair pr;
	pair_init(&pr, strategy);
	for (uint32_t n = 0; n < 6; n++)
		pair_slide(&pr, n, WITH_LOGO);
	CHECK(logo_score(pr.on) <= 1.0 / 255.0);

	busy_reset_history(pr.on);
	busy_reset_history(pr.off);
	/* Learning starts over: reference + 3 slide changes. */
	for (uint32_t n = 6; n < 12; n++) {
		pair_slide(&pr, n, WITH_LOGO);
		if (n < 9)
			CHECK(same_cost(&pr));
		else
			CHECK(logo_score(pr.on) <= 1.0 / 255.0);
	}
	pair_free(&pr);
}

static void test_static(int strategy)
{
	struct pair pr;
	pair_init(&pr, strategy);

	/* Never-changing frames. */
	make_slide(0, WITH_LOGO);
	for (int i = 0; i < 50; i++) {
		pair_process(&pr);
		CHECK(same_cost(&pr));
	}
	/* Flashes / pop-ups that return to the same slide are not slide
	 * changes, however many there are. */
	for (int i = 0; i < 10; i++) {
		pair_slide(&pr, 0, WITH_LOGO);
		CHECK(same_cost(&pr));
	}
	pair_free(&pr);
}

static void test_cap(int strategy)
{
	struct pair pr;
	pair_init(&pr, strategy);
	/* A persistent sidebar with as much content as the slide itself:
	 * too much "template" to be a template, so nothing is discounted
	 * (the logo neither: the cap is global). */
	for (uint32_t n = 0; n < 8; n++) {
		pair_slide(&pr, n, WITH_LOGO | WITH_SIDEBAR);
		CHECK(same_cost(&pr));
	}
	pair_free(&pr);
}

int main(void)
{
	const int strategies[] = {BUSY_STRATEGY_ANY, BUSY_STRATEGY_BG_IMAGE};
	for (size_t i = 0; i < sizeof(strategies) / sizeof(strategies[0]); i++) {
		int s = strategies[i];
		int before = failures;
		test_logo_learnt(s);
		test_disabled(s);
		test_reset(s);
		test_static(s);
		test_cap(s);
		if (failures != before)
			fprintf(stderr, "  (strategy %d)\n", s);
	}

	if (failures) {
		fprintf(stderr, "%d of %d checks failed\n", failures, checks);
		return 1;
	}
	printf("all %d checks passed\n", checks);
	return 0;
}
