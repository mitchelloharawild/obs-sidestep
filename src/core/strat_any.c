/*
 * obs-sidestep — "Any content" strategy: every edge or moving pixel is
 * content (cost 255), everything else is free (cost 0).
 *
 * SPDX-License-Identifier: MIT
 */
#include "strategy.h"

void busy_cost_edge_motion(struct busy_analyzer *b,
			   const struct busy_params *p, size_t i0, size_t i1)
{
	const uint8_t *edge = b->edge;
	const uint8_t *motion = b->motion;
	uint8_t *cost = b->cost;
	const int t_edge = p->t_edge;

	if (p->use_motion) {
		for (size_t i = i0; i < i1; i++)
			cost[i] = (edge[i] > t_edge || motion[i]) ? 255 : 0;
	} else {
		for (size_t i = i0; i < i1; i++)
			cost[i] = edge[i] > t_edge ? 255 : 0;
	}
}

static void any_process(struct busy_analyzer *b, const struct busy_frame *fr,
			const struct busy_params *p, void *state)
{
	(void)fr;
	(void)state;
	busy_cost_edge_motion(b, p, 0, (size_t)b->w * b->h);
}

const struct busy_strategy_ops busy_strat_any = {
	.id = BUSY_STRATEGY_ANY,
	.name_key = "Strategy.Any",
	.desc_key = "Strategy.Any.Description",
	.process = any_process,
};
