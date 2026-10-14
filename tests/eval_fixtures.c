/*
 * obs-sidestep — fixture evaluation for avoidance strategies (not a unit
 * test; used to tune thresholds against real slides).
 *
 * Usage: eval_fixtures [fixture_dir] [strategy_id] [out_dir]
 *
 * Reads <fixture_dir>/labels.txt ("<file.ppm> <left|right|none>" per
 * line; '#' comments), runs the strategy on each binary PPM (320x180 by
 * default) and simulates one placement decision for a 384x216 camera on a
 * 1920x1080 canvas (margin 24, content filling the canvas, choices bottom
 * right then bottom left, preferred positions only, hide when nothing
 * fits), starting from each of the two corners.
 * Expected outcome: "right"/"left" -> the bottom corner on that side, on
 * the image; "none" -> a clear BR / BL or hide. With out_dir, writes
 * <name>.debug.ppm: the slide dimmed, cost tinted red, image tinted blue.
 *
 * SPDX-License-Identifier: MIT
 */
#include "busy.h"
#include "place.h"
#include "strategy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CANVAS_W 1920.0
#define CANVAS_H 1080.0
#define CAM_W 384.0
#define CAM_H 216.0
#define MARGIN 24.0

struct ppm {
	uint32_t w, h;
	uint8_t *rgba;
};

static int read_token(FILE *f, char *buf, size_t n)
{
	int c;
	size_t i = 0;
	do {
		c = fgetc(f);
		if (c == '#')
			while (c != '\n' && c != EOF)
				c = fgetc(f);
	} while (c == ' ' || c == '\n' || c == '\r' || c == '\t');
	while (c != EOF && c != ' ' && c != '\n' && c != '\r' && c != '\t') {
		if (i + 1 < n)
			buf[i++] = (char)c;
		c = fgetc(f);
	}
	buf[i] = 0;
	return (int)i;
}

static bool ppm_read(const char *path, struct ppm *im)
{
	FILE *f = fopen(path, "rb");
	char tok[32];
	if (!f)
		return false;
	bool ok = read_token(f, tok, sizeof(tok)) && !strcmp(tok, "P6");
	unsigned w = 0, h = 0, maxv = 0;
	if (ok && read_token(f, tok, sizeof(tok)))
		w = (unsigned)atoi(tok);
	if (ok && read_token(f, tok, sizeof(tok)))
		h = (unsigned)atoi(tok);
	if (ok && read_token(f, tok, sizeof(tok)))
		maxv = (unsigned)atoi(tok);
	ok = ok && w && h && maxv == 255;
	if (ok) {
		size_t n = (size_t)w * h;
		uint8_t *rgb = malloc(n * 3);
		im->rgba = malloc(n * 4);
		ok = rgb && im->rgba && fread(rgb, 3, n, f) == n;
		for (size_t i = 0; ok && i < n; i++) {
			im->rgba[4 * i + 0] = rgb[3 * i + 0];
			im->rgba[4 * i + 1] = rgb[3 * i + 1];
			im->rgba[4 * i + 2] = rgb[3 * i + 2];
			im->rgba[4 * i + 3] = 255;
		}
		free(rgb);
		im->w = w;
		im->h = h;
	}
	fclose(f);
	return ok;
}

static void write_debug(const char *path, const struct ppm *im,
			const struct busy_analyzer *b)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%u %u\n255\n", im->w, im->h);
	for (size_t i = 0, n = (size_t)im->w * im->h; i < n; i++) {
		int c = b->cost[i];
		bool img = b->has_image && b->image[i];
		uint8_t px[3];
		for (int k = 0; k < 3; k++) {
			int v = im->rgba[4 * i + k] / 2 + 64;
			if (k == 0)
				v += c / 2;
			if (k == 2 && img)
				v += 100;
			px[k] = (uint8_t)(v > 255 ? 255 : v);
		}
		fwrite(px, 1, 3, f);
	}
	fclose(f);
}

/* Content fills the canvas: canvas px -> analysis px is a plain scale. */
struct eval_ctx {
	const struct busy_analyzer *b;
	double sx, sy;
};

static double eval_score(void *p, double x, double y, double w, double h)
{
	const struct eval_ctx *e = p;
	return busy_rect_score(e->b, x * e->sx, y * e->sy, w * e->sx, h * e->sy);
}

static double eval_image(void *p, double x, double y, double w, double h)
{
	const struct eval_ctx *e = p;
	return busy_rect_image_frac(e->b, x * e->sx, y * e->sy, w * e->sx,
				    h * e->sy);
}

static bool near(double ax, double ay, double bx, double by)
{
	return ax - bx < 0.5 && bx - ax < 0.5 && ay - by < 0.5 && by - ay < 0.5;
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "tests/fixtures/user2026";
	int strategy = argc > 2 ? atoi(argv[2]) : BUSY_STRATEGY_BG_IMAGE;
	const char *out = argc > 3 ? argv[3] : NULL;
	char path[1024], name[256], label[32];

	snprintf(path, sizeof(path), "%s/labels.txt", dir);
	FILE *lf = fopen(path, "r");
	if (!lf) {
		fprintf(stderr, "cannot open %s\n", path);
		return 2;
	}

	const struct busy_strategy_ops *ops = busy_strategy_find(strategy);
	printf("strategy %d (%s), camera %.0fx%.0f on %.0fx%.0f, margin %.0f\n\n", ops->id, ops->name_key, CAM_W,
	       CAM_H, CANVAS_W, CANVAS_H, MARGIN);
	printf("%-12s %-6s | %7s %6s | %7s %6s | %-5s %-5s %s\n", "fixture", "expect", "BR busy", "BR img", "BL busy",
	       "BL img", "pick", "image", "result");

	int pass = 0, total = 0;
	char line[512];
	while (fgets(line, sizeof(line), lf)) {
		if (line[0] == '#' || sscanf(line, "%255s %31s", name, label) != 2)
			continue;
		struct ppm im = {0};
		snprintf(path, sizeof(path), "%s/%s", dir, name);
		if (!ppm_read(path, &im)) {
			fprintf(stderr, "cannot read %s\n", path);
			continue;
		}

		struct busy_analyzer *b = busy_create(im.w, im.h);
		struct busy_params p;
		busy_params_default(&p);
		p.strategy = strategy;
		/* Two identical captures: a settled check with no motion. */
		busy_process(b, im.rgba, im.w * 4, BUSY_FMT_RGBA, &p);
		busy_process(b, im.rgba, im.w * 4, BUSY_FMT_RGBA, &p);

		struct eval_ctx ec = {b, im.w / CANVAS_W, im.h / CANVAS_H};
		struct place_config c;
		place_config_default(&c);
		c.choices[0] = PLACE_ANCHOR_BOTTOM_RIGHT;
		c.choices[1] = PLACE_ANCHOR_BOTTOM_LEFT;
		c.choices[2] = PLACE_ANCHOR_NONE;
		c.margin = MARGIN;
		c.mode = PLACE_MODE_PREFERRED;
		c.prefer_images = ops->prefers_images;

		double brx, bry, blx, bly;
		place_anchor_pos(PLACE_ANCHOR_BOTTOM_RIGHT, &c, CANVAS_W, CANVAS_H, CAM_W, CAM_H, &brx, &bry);
		place_anchor_pos(PLACE_ANCHOR_BOTTOM_LEFT, &c, CANVAS_W, CANVAS_H, CAM_W, CAM_H, &blx, &bly);
		double br_s = eval_score(&ec, brx - MARGIN, bry - MARGIN, CAM_W + 2 * MARGIN, CAM_H + 2 * MARGIN);
		double bl_s = eval_score(&ec, blx - MARGIN, bly - MARGIN, CAM_W + 2 * MARGIN, CAM_H + 2 * MARGIN);
		double br_i = eval_image(&ec, brx, bry, CAM_W, CAM_H);
		double bl_i = eval_image(&ec, blx, bly, CAM_W, CAM_H);

		/* Decide from both starting corners; both must agree. */
		const char *pick[2];
		bool on_img[2];
		for (int start = 0; start < 2; start++) {
			struct place_state st;
			place_state_reset(&st);
			struct place_input in = {
				.now_ns = 100000000000ULL,
				.canvas_w = CANVAS_W,
				.canvas_h = CANVAS_H,
				.cam_w = CAM_W,
				.cam_h = CAM_H,
				.cam_x = start ? blx : brx,
				.cam_y = start ? bly : bry,
				.cam_visible = true,
				.score = eval_score,
				.score_ctx = &ec,
				.image_frac = eval_image,
				.new_content = true,
			};
			struct place_decision d = place_evaluate(&st, &c, &in);
			double x = in.cam_x, y = in.cam_y;
			bool vis = true;
			if (d.action == PLACE_ACTION_MOVE || d.action == PLACE_ACTION_SHOW) {
				x = d.x;
				y = d.y;
			} else if (d.action == PLACE_ACTION_HIDE) {
				vis = false;
			}
			pick[start] = !vis ? "hide" : near(x, y, brx, bry) ? "BR" : near(x, y, blx, bly) ? "BL" : "other";
			on_img[start] = vis && eval_image(&ec, x, y, CAM_W, CAM_H) >= c.t_image;
		}

		bool ok;
		if (!strcmp(label, "right"))
			ok = !strcmp(pick[0], "BR") && !strcmp(pick[1], "BR") && on_img[0];
		else if (!strcmp(label, "left"))
			ok = !strcmp(pick[0], "BL") && !strcmp(pick[1], "BL") && on_img[0];
		else
			ok = !on_img[0] && !on_img[1] && strcmp(pick[0], "other") && strcmp(pick[1], "other");
		total++;
		pass += ok;

		char picks[16];
		if (!strcmp(pick[0], pick[1]))
			snprintf(picks, sizeof(picks), "%s", pick[0]);
		else
			snprintf(picks, sizeof(picks), "%s/%s", pick[0], pick[1]);
		printf("%-12s %-6s | %6.2f%% %5.0f%% | %6.2f%% %5.0f%% | %-5s %-5s %s\n", name, label, br_s * 100.0,
		       br_i * 100.0, bl_s * 100.0, bl_i * 100.0, picks, on_img[0] ? "yes" : "no",
		       ok ? "ok" : "FAIL");

		if (out) {
			char dbg[1024];
			snprintf(dbg, sizeof(dbg), "%s/%.*s.debug.ppm", out, (int)(strlen(name) > 4 ? strlen(name) - 4 : strlen(name)), name);
			write_debug(dbg, &im, b);
		}
		busy_destroy(b);
		free(im.rgba);
	}
	fclose(lf);
	printf("\n%d / %d fixtures as expected\n", pass, total);
	return pass == total ? 0 : 1;
}
