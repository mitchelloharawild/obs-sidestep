/*
 * obs-sidestep — avoidance strategy and modifier registries.
 *
 * SPDX-License-Identifier: MIT
 */
#include "strategy.h"

/* GUI order. ids are persisted; the first entry is the fallback. */
static const struct busy_strategy_ops *const strategies[] = {
	&busy_strat_any,
	&busy_strat_bgimage,
	&busy_strat_solid,
	&busy_strat_activity,
};

/* Post-process modifiers, applied in this order after the strategy.
 * The trailing NULL keeps the array valid C. */
static const struct busy_modifier_ops *const modifiers[] = {
	&busy_mod_template,
	NULL,
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

size_t busy_strategy_count(void)
{
	return COUNT(strategies);
}

const struct busy_strategy_ops *busy_strategy_get(size_t index)
{
	return index < COUNT(strategies) ? strategies[index] : NULL;
}

const struct busy_strategy_ops *busy_strategy_find(int id)
{
	for (size_t i = 0; i < COUNT(strategies); i++) {
		if (strategies[i]->id == id)
			return strategies[i];
	}
	return strategies[0];
}

size_t busy_modifier_count(void)
{
	size_t n = 0;
	while (n < COUNT(modifiers) && modifiers[n])
		n++;
	return n;
}

const struct busy_modifier_ops *busy_modifier_get(size_t index)
{
	return index < busy_modifier_count() ? modifiers[index] : NULL;
}
