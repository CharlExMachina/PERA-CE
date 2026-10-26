/*
CUSTOM_EDITION_HUD.H

Placement of Halo Custom Edition weapon warnings. Those states are entries
in the retail crosshair list (hud_weapon.c). A map made around the old
widescreen fix parked them at the edges of a 640x480 view, so a wider
window leaves them toward the middle. Charge and the two trigger-ready
states stay reticles.
*/

#ifndef CUSTOM_EDITION_HUD_H
#define CUSTOM_EDITION_HUD_H

#include "cache_file_formats.h"
#include "custom_edition_cache.h"
#include "interface/hud_definitions.h"

/* flash ammo through the secondary empty reload, and the depleted battery
flash. The numbers are the retail crosshair states. */
static boolean custom_edition_hud_warning(short state)
{
	return custom_edition_cache_tags_loaded() &&
		((state >= 3 && state <= 15) || state == 18);
}

static void custom_edition_hud_warning_placement(short state, short width, short height,
	struct hud_placement_definition *placement)
{
	long extra;

	if (!custom_edition_hud_warning(state) ||
		!custom_edition_cache_relies_on(_custom_edition_behaviour_old_widescreen_fix))
		return;
	extra = (width - 624) / 2;
	if (placement->offset.x > 0)
		placement->offset.x = (short)PIN(placement->offset.x + extra, SHORT_MIN, SHORT_MAX);
	else if (placement->offset.x < 0)
		placement->offset.x = (short)PIN(placement->offset.x - extra, SHORT_MIN, SHORT_MAX);
	extra = (height - 464) / 2;
	if (placement->offset.y > 0)
		placement->offset.y = (short)PIN(placement->offset.y + extra, SHORT_MIN, SHORT_MAX);
	else if (placement->offset.y < 0)
		placement->offset.y = (short)PIN(placement->offset.y - extra, SHORT_MIN, SHORT_MAX);
}

#endif
