/*
 * obs-sidestep — benchmark for core/busy.c
 *
 * Times one "check" (busy_process + ~3000 busy_rect_score lookups) at each
 * analysis resolution.
 *
 * SPDX-License-Identifier: MIT
 */
#include "busy.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define RECTS_PER_CHECK 3000

static double now_us(void)
{
	struct timespec ts;
	timespec_get(&ts, TIME_UTC);
	return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static uint32_t rng_state = 1;
static uint32_t rng(void)
{
	rng_state = rng_state * 1664525u + 1013904223u;
	return rng_state >> 8;
}

/* Keeps the compiler from discarding the scores. */
static volatile double sink;

static void bench(uint32_t w, uint32_t h, int iters)
{
	const uint32_t linesize = w * 4;
	const int nframes = 2;
	uint8_t *frames[2];
	for (int f = 0; f < nframes; f++) {
		frames[f] = malloc((size_t)linesize * h);
		if (!frames[f]) {
			fprintf(stderr, "out of memory\n");
			exit(1);
		}
		/* Slide-like content: white page, text in the top half,
		 * noisy photo in the bottom right, differs between frames. */
		for (uint32_t y = 0; y < h; y++) {
			uint8_t *row = frames[f] + (size_t)y * linesize;
			for (uint32_t x = 0; x < w; x++) {
				uint8_t v = 250;
				if (y < h / 2 && (x % 6) < 4 && (y % 10) < 7)
					v = 20;
				else if (x > w / 2 && y > h / 2)
					v = (uint8_t)(rng() + f * 40);
				row[4 * x + 0] = v;
				row[4 * x + 1] = v;
				row[4 * x + 2] = v;
				row[4 * x + 3] = 255;
			}
		}
	}

	/* Precompute candidate rects (camera ~1/5 width, varying offsets). */
	double *rects = malloc(sizeof(double) * 4 * RECTS_PER_CHECK);
	if (!rects) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	for (int i = 0; i < RECTS_PER_CHECK; i++) {
		double rw = w / 5.0 + (rng() % 8), rh = rw * 9.0 / 16.0;
		rects[4 * i + 0] = (double)(rng() % (w + 20)) - 10.0 + 0.3;
		rects[4 * i + 1] = (double)(rng() % (h + 20)) - 10.0 + 0.7;
		rects[4 * i + 2] = rw;
		rects[4 * i + 3] = rh;
	}

	struct busy_analyzer *b = busy_create(w, h);
	struct busy_params p;
	busy_params_default(&p);

	/* Warm-up. */
	for (int i = 0; i < 10; i++)
		busy_process(b, frames[i & 1], linesize, BUSY_FMT_BGRA, &p);

	double t_proc = 0.0, t_rect = 0.0, acc = 0.0;
	for (int i = 0; i < iters; i++) {
		double t0 = now_us();
		busy_process(b, frames[i & 1], linesize, BUSY_FMT_BGRA, &p);
		double t1 = now_us();
		for (int r = 0; r < RECTS_PER_CHECK; r++)
			acc += busy_rect_score(b, rects[4 * r], rects[4 * r + 1],
					       rects[4 * r + 2],
					       rects[4 * r + 3]);
		double t2 = now_us();
		t_proc += t1 - t0;
		t_rect += t2 - t1;
	}
	sink = acc;

	printf("%4ux%-4u  process %8.1f us   %d rects %7.1f us   "
	       "total %8.1f us/check\n",
	       w, h, t_proc / iters, RECTS_PER_CHECK, t_rect / iters,
	       (t_proc + t_rect) / iters);

	busy_destroy(b);
	free(rects);
	for (int f = 0; f < nframes; f++)
		free(frames[f]);
}

int main(int argc, char **argv)
{
	int iters = argc > 1 ? atoi(argv[1]) : 500;
	if (iters < 1)
		iters = 1;

	printf("bench_busy: %d iterations per resolution\n", iters);
	bench(160, 90, iters);
	bench(320, 180, iters);
	bench(480, 270, iters);
	return 0;
}
