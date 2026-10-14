/*
 * obs-sidestep — placement logic (no libobs dependency).
 *
 * Implementation notes / decisions where the header leaves room:
 *  - Choices are matched to positions with a 0.5 px tolerance. Duplicate
 *    choices (same anchor, or an anchor resolving to the same position as
 *    an earlier one) are ignored, so the earlier priority wins.
 *  - ANYWHERE mode ignores the choice list entirely (the UI hides it), so
 *    there is no return home and no choice rects in the report.
 *  - Anchors are inset by the margin. If the camera does not fit between
 *    the margins on an axis it is centred on that axis. The captured
 *    position is used as-is (it is an explicit user layout), only clamped
 *    to keep the camera on the canvas.
 *  - "Least busy" uses the same cost as the automatic search
 *    (score + lambda * dist / diag), so ties go to the nearest candidate.
 *  - Hidden state is taken from in->cam_visible (place_state has no field
 *    of its own). While hidden we only SHOW once something is *suitable*;
 *    the least-busy fallback never re-shows a hidden camera, so a camera
 *    hidden by the user (hotkey) is not forced visible.
 *  - When idle, `chosen` points at the current position if it is visible
 *    and suitable; it is -1 when hiding or hidden with nothing selected.
 *
 * SPDX-License-Identifier: MIT
 */
#include "place.h"

#include <math.h>
#include <string.h>

#define POS_TOL 0.5

void place_config_default(struct place_config *c)
{
	memset(c, 0, sizeof(*c));
	c->mode = PLACE_MODE_PREFERRED_THEN_ANYWHERE;
	c->choices[0] = PLACE_ANCHOR_BOTTOM_RIGHT;
	c->choices[1] = PLACE_ANCHOR_TOP_RIGHT;
	c->choices[2] = PLACE_ANCHOR_BOTTOM_LEFT;
	c->choices[3] = PLACE_ANCHOR_NONE;
	c->margin = 24.0;
	c->return_home = true;
	c->fallback = PLACE_FALLBACK_HIDE;
	c->t_suitable = 0.01;
	c->confirm_checks = 1;
	c->return_delay_ns = 3000000000ULL;
	c->cooldown_ns = 1000000000ULL;
	c->lambda = 0.01;
	c->grid_divisions = 48;
}

void place_state_reset(struct place_state *s)
{
	memset(s, 0, sizeof(*s));
}

double place_ease_in_out(double t)
{
	if (t <= 0.0)
		return 0.0;
	if (t >= 1.0)
		return 1.0;
	if (t < 0.5)
		return 4.0 * t * t * t;
	double u = -2.0 * t + 2.0;
	return 1.0 - u * u * u / 2.0;
}

/* Position on one axis: 0 = start, 1 = centre, 2 = end, inset by margin. */
static double axis_pos(int which, double canvas, double size, double margin)
{
	double lo = margin;
	double hi = canvas - size - margin;
	if (hi < lo)
		return (canvas - size) / 2.0;
	if (which == 0)
		return lo;
	if (which == 2)
		return hi;
	return (canvas - size) / 2.0;
}

static double clamp_range(double v, double lo, double hi)
{
	if (hi < lo)
		return (lo + hi) / 2.0;
	return v < lo ? lo : (v > hi ? hi : v);
}

bool place_anchor_pos(enum place_anchor a, const struct place_config *c,
		      double canvas_w, double canvas_h, double cam_w,
		      double cam_h, double *x, double *y)
{
	if (a == PLACE_ANCHOR_CAPTURED) {
		if (!c->has_captured)
			return false;
		*x = clamp_range(c->captured_x, 0.0, canvas_w - cam_w);
		*y = clamp_range(c->captured_y, 0.0, canvas_h - cam_h);
		return true;
	}
	if (a <= PLACE_ANCHOR_NONE || a > PLACE_ANCHOR_BOTTOM_RIGHT)
		return false;

	int idx = (int)a - (int)PLACE_ANCHOR_TOP_LEFT; /* 0..8 */
	*x = axis_pos(idx % 3, canvas_w, cam_w, c->margin);
	*y = axis_pos(idx / 3, canvas_h, cam_h, c->margin);
	return true;
}

/* ------------------------------------------------------------------------ */

struct cand {
	double x, y;
	double score;
	bool suitable;
};

struct ctx {
	const struct place_config *c;
	const struct place_input *in;
	struct place_decision *d;
	double diag;
};

static bool same_pos(double ax, double ay, double bx, double by)
{
	return fabs(ax - bx) <= POS_TOL && fabs(ay - by) <= POS_TOL;
}

static void score_at(struct ctx *k, double x, double y, struct cand *out)
{
	double m = k->c->margin;
	const struct place_input *in = k->in;
	out->x = x;
	out->y = y;
	out->score = in->score(in->score_ctx, x - m, y - m, in->cam_w + 2.0 * m,
			       in->cam_h + 2.0 * m);
	out->suitable = out->score <= k->c->t_suitable;
}

/* Add a rect to the report (deduplicated by position); returns its index
 * or -1 if the report is full. */
static int report_add(struct ctx *k, const struct cand *cd)
{
	struct place_decision *d = k->d;
	for (int i = 0; i < d->n_report; i++) {
		if (same_pos(d->report[i].x, d->report[i].y, cd->x, cd->y))
			return i;
	}
	if (d->n_report >= PLACE_MAX_REPORT)
		return -1;
	struct place_report_item *r = &d->report[d->n_report];
	r->x = cd->x;
	r->y = cd->y;
	r->w = k->in->cam_w;
	r->h = k->in->cam_h;
	r->score = cd->score;
	r->suitable = cd->suitable;
	return d->n_report++;
}

static double cost(struct ctx *k, const struct cand *cd)
{
	double dx = cd->x - k->in->cam_x;
	double dy = cd->y - k->in->cam_y;
	double dist = sqrt(dx * dx + dy * dy);
	return cd->score + k->c->lambda * dist / k->diag;
}

/* Grid positions along one axis: lo, lo+stride, ..., plus hi. */
static int grid_axis(double canvas, double size, double margin, double stride,
		     int i, double *v)
{
	double lo = margin;
	double hi = canvas - size - margin;
	if (hi < lo) {
		if (i > 0)
			return 0;
		*v = (canvas - size) / 2.0;
		return 1;
	}
	double p = lo + stride * i;
	if (p < hi - POS_TOL) {
		*v = p;
		return 1;
	}
	/* Final step lands exactly on the far edge. */
	if (i == 0 || lo + stride * (i - 1) < hi - POS_TOL) {
		*v = hi;
		return 1;
	}
	return 0;
}

/* Automatic search. Minimises cost among suitable rects (or among all
 * rects when any_score is set, for the least-busy fallback). */
static bool grid_search(struct ctx *k, bool any_score, struct cand *best)
{
	const struct place_input *in = k->in;
	double m = k->c->margin;
	int div = k->c->grid_divisions > 0 ? k->c->grid_divisions : 48;
	double stride = in->canvas_w / div;
	if (stride < 1.0)
		stride = 1.0;

	bool found = false;
	double best_cost = 0.0;
	double x, y;
	for (int j = 0; grid_axis(in->canvas_h, in->cam_h, m, stride, j, &y);
	     j++) {
		for (int i = 0;
		     grid_axis(in->canvas_w, in->cam_w, m, stride, i, &x);
		     i++) {
			struct cand cd;
			score_at(k, x, y, &cd);
			if (!any_score && !cd.suitable)
				continue;
			double cc = cost(k, &cd);
			if (!found || cc < best_cost) {
				*best = cd;
				best_cost = cc;
				found = true;
			}
		}
	}
	return found;
}

struct place_decision place_evaluate(struct place_state *s,
				     const struct place_config *c,
				     const struct place_input *in)
{
	struct place_decision d;
	memset(&d, 0, sizeof(d));
	d.action = PLACE_ACTION_NONE;
	d.x = in->cam_x;
	d.y = in->cam_y;
	d.chosen = -1;

	if (!s->initialised) {
		place_state_reset(s);
		s->initialised = true;
	}

	struct ctx k = {c, in, &d, 0.0};
	k.diag = sqrt(in->canvas_w * in->canvas_w + in->canvas_h * in->canvas_h);
	if (k.diag <= 0.0)
		k.diag = 1.0;

	uint64_t now = in->now_ns;
	bool use_choices = c->mode != PLACE_MODE_ANYWHERE;
	bool use_auto = c->mode != PLACE_MODE_PREFERRED;
	int confirm = c->confirm_checks < 1 ? 1 : c->confirm_checks;
	bool cooling = s->has_acted && now - s->last_action_ns < c->cooldown_ns;

	/* Score the choices and track their continuous suitability. */
	struct cand ch[PLACE_MAX_CHOICES];
	bool ch_valid[PLACE_MAX_CHOICES] = {false};
	int cur_prio = PLACE_MAX_CHOICES; /* not a choice: lowest priority */
	for (int i = 0; i < PLACE_MAX_CHOICES && use_choices; i++) {
		double x, y;
		if (!place_anchor_pos(c->choices[i], c, in->canvas_w,
				      in->canvas_h, in->cam_w, in->cam_h, &x,
				      &y))
			continue;
		bool dup = false;
		for (int j = 0; j < i; j++) {
			if (ch_valid[j] && same_pos(ch[j].x, ch[j].y, x, y))
				dup = true;
		}
		if (dup)
			continue;
		ch_valid[i] = true;
		score_at(&k, x, y, &ch[i]);
		report_add(&k, &ch[i]);
		if (cur_prio == PLACE_MAX_CHOICES &&
		    same_pos(x, y, in->cam_x, in->cam_y))
			cur_prio = i;
	}
	for (int i = 0; i < PLACE_MAX_CHOICES; i++) {
		if (ch_valid[i] && ch[i].suitable) {
			if (!s->suitable_since_ns[i])
				s->suitable_since_ns[i] = now ? now : 1;
		} else {
			s->suitable_since_ns[i] = 0;
		}
	}

	/* Selection: highest-priority suitable choice, else auto search. */
	struct cand sel = {0};
	bool has_sel = false;
	for (int i = 0; i < PLACE_MAX_CHOICES && !has_sel; i++) {
		if (ch_valid[i] && ch[i].suitable) {
			sel = ch[i];
			has_sel = true;
		}
	}
	bool need_sel;
	struct cand cur = {0};
	if (in->cam_visible) {
		score_at(&k, in->cam_x, in->cam_y, &cur);
		int ci = report_add(&k, &cur);
		need_sel = !cur.suitable && s->unsuitable_count + 1 >= confirm;
		if (cur.suitable)
			d.chosen = ci;
	} else {
		need_sel = true;
	}
	if (!has_sel && use_auto && need_sel)
		has_sel = grid_search(&k, false, &sel);

	bool act = false;
	if (!in->cam_visible) {
		s->unsuitable_count = 0;
		s->show_count = has_sel ? s->show_count + 1 : 0;
		if (has_sel) {
			d.chosen = report_add(&k, &sel);
			if (s->show_count >= confirm && !cooling) {
				d.action = PLACE_ACTION_SHOW;
				d.x = sel.x;
				d.y = sel.y;
				act = true;
			}
		}
	} else if (cur.suitable) {
		s->show_count = 0;
		s->unsuitable_count = 0;
		/* Return home to the highest choice above the current one that
		 * has been suitable for the return delay. */
		for (int i = 0; i < cur_prio && c->return_home; i++) {
			uint64_t since = s->suitable_since_ns[i];
			if (!since || now < since ||
			    now - since < c->return_delay_ns)
				continue;
			if (!cooling) {
				d.action = PLACE_ACTION_MOVE;
				d.x = ch[i].x;
				d.y = ch[i].y;
				d.chosen = report_add(&k, &ch[i]);
				act = true;
			}
			break;
		}
	} else {
		s->show_count = 0;
		s->unsuitable_count++;
		if (s->unsuitable_count >= confirm) {
			struct cand target = {0};
			bool move = false;
			if (has_sel) {
				target = sel;
				move = true;
			} else if (c->fallback == PLACE_FALLBACK_HIDE) {
				if (!cooling) {
					d.action = PLACE_ACTION_HIDE;
					act = true;
				}
			} else if (use_auto) {
				/* Least busy over grid + choices. */
				move = grid_search(&k, true, &target);
				for (int i = 0; i < PLACE_MAX_CHOICES; i++) {
					if (ch_valid[i] &&
					    (!move || cost(&k, &ch[i]) <
							      cost(&k, &target))) {
						target = ch[i];
						move = true;
					}
				}
			}
			if (move) {
				d.chosen = report_add(&k, &target);
				if (!cooling && !same_pos(target.x, target.y,
							  in->cam_x,
							  in->cam_y)) {
					d.action = PLACE_ACTION_MOVE;
					d.x = target.x;
					d.y = target.y;
					act = true;
				}
			}
		}
	}

	if (act) {
		s->last_action_ns = now;
		s->has_acted = true;
		s->unsuitable_count = 0;
		s->show_count = 0;
	}
	return d;
}
