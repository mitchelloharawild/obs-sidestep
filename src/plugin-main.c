/*
 * obs-sidestep — module entry point.
 *
 * SPDX-License-Identifier: MIT
 */
#include <obs-module.h>

#ifndef PLUGIN_VERSION
#define PLUGIN_VERSION "unknown"
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-sidestep", "en-US")

extern struct obs_source_info sidestep_filter_info;

MODULE_EXPORT const char *obs_module_name(void)
{
	return obs_module_text("Module.Name");
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Module.Description");
}

bool obs_module_load(void)
{
	obs_register_source(&sidestep_filter_info);
	blog(LOG_INFO, "[obs-sidestep] loaded (version %s)", PLUGIN_VERSION);
	return true;
}
