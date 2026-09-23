/* Strict launch argument for the signed, single-bundle rate sweep. */
#ifndef MAC_SHADOW_RATE_SWEEP_SELECTION_H
#define MAC_SHADOW_RATE_SWEEP_SELECTION_H

#include <string.h>

static inline unsigned shadow_rate_sweep_select(const char* argument)
{
	static const char prefix[] = "--rate-sweep-kib=";
	if (!argument || strncmp(argument, prefix, sizeof(prefix) - 1) != 0)
		return 0;
	const char* value = argument + sizeof(prefix) - 1;
	if (strcmp(value, "150") == 0)
		return 150;
	if (strcmp(value, "200") == 0)
		return 200;
	if (strcmp(value, "250") == 0)
		return 250;
	if (strcmp(value, "300") == 0)
		return 300;
	if (strcmp(value, "400") == 0)
		return 400;
	return 0;
}

#endif
