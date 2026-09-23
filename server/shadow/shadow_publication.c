/* Mac shadow publication staging and pacing. Licensed under Apache-2.0. */
#include <freerdp/config.h>
#include <string.h>

#include "shadow_publication.h"

static UINT64 shadow_publication_clipped_area(const REGION16* region, UINT32 width,
                                               UINT32 height)
{
	UINT64 area = 0;
	UINT32 count = 0;
	const RECTANGLE_16* rects = region ? region16_rects(region, &count) : nullptr;
	for (UINT32 index = 0; index < count; index++)
	{
		const RECTANGLE_16* rect = &rects[index];
		const UINT32 left = MIN(rect->left, width);
		const UINT32 right = MIN(rect->right, width);
		const UINT32 top = MIN(rect->top, height);
		const UINT32 bottom = MIN(rect->bottom, height);
		if (right > left && bottom > top)
			area += (UINT64)(right - left) * (bottom - top);
	}
	return area;
}

BOOL shadow_publication_stage(shadowBitmapState* state, shadowPacer* pacer,
                              const rdpShadowSurface* surface, const REGION16* clientRefresh,
                              UINT64 nowMs, shadowPublicationResult* result)
{
	if (!state || !pacer || !surface || !result || !surface->width || !surface->height)
		return FALSE;
	memset(result, 0, sizeof(*result));
	if (!shadow_bitmap_stage(state, surface->data, surface->format, surface->scanline,
	                         clientRefresh))
		return FALSE;
	shadow_pacer_note_time(pacer, nowMs, shadow_bitmap_pending(state));
	result->desktopArea = (UINT64)surface->width * surface->height;
	result->clientRefreshArea =
	    shadow_publication_clipped_area(clientRefresh, surface->width, surface->height);
	result->publicationArea = shadow_publication_clipped_area(
	    &surface->invalidRegion, surface->width, surface->height);
	shadow_bitmap_note_publication_area(state, result->publicationArea, result->desktopArea);
	/* An empty update event does not supersede the deferred operation. Keep
	 * its remembered credit requirement so preflight avoids recompression. */
	if (result->publicationArea || result->clientRefreshArea)
		shadow_pacer_cancel_wait(pacer);
	result->qualified = result->publicationArea * 100U >=
	                    result->desktopArea * pacer->largeDamagePercent;
	result->denialReason = 6;
	if (pacer->largeRefreshBurstEnabled)
	{
		result->denialReason = 0;
		if (!result->qualified)
			result->denialReason = 1;
		else if (pacer->burstActive)
			result->denialReason = 2;
		else if (nowMs < pacer->burstEligibleMs)
			result->denialReason = 3;
		else if (pacer->pressure)
			result->denialReason = 4;
		else if (pacer->haveQueue && nowMs >= pacer->lastQueueMs &&
		         nowMs - pacer->lastQueueMs <= 100 &&
		         pacer->lastQueued >= pacer->baseMaxCredit)
			result->denialReason = 5;
		else if (pacer->largeRefreshDiagnosticM && pacer->havePublicationDamage &&
		         nowMs >= pacer->lastPublicationDamageMs &&
		         nowMs - pacer->lastPublicationDamageMs <
		             SHADOW_PACER_LARGE_REFRESH_QUIET_REARM_MS)
			result->denialReason = 7;
		result->burstStarted =
		    shadow_pacer_note_damage(pacer, nowMs, result->publicationArea, result->desktopArea);
	}
	return TRUE;
}

void shadow_publication_commit(shadowBitmapState* state, shadowPacer* pacer,
                               const shadowBitmapTile* tile, UINT32 chargedBytes)
{
	shadow_pacer_submitted(pacer, chargedBytes);
	shadow_bitmap_commit(state, tile);
}
