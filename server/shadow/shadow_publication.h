/* Mac shadow publication staging and pacing. Licensed under Apache-2.0. */
#ifndef FREERDP_SHADOW_PUBLICATION_H
#define FREERDP_SHADOW_PUBLICATION_H

#include "shadow_bitmap.h"
#include "shadow_pacer.h"
#include <freerdp/server/shadow.h>

typedef struct
{
	UINT64 publicationArea;
	UINT64 clientRefreshArea;
	UINT64 desktopArea;
	BOOL qualified;
	BOOL burstStarted;
	UINT32 denialReason; /* 0 started, 1 area, 2 active, 3 cooldown,
	                      * 4 transport blocked, 5 local socket queue, 6 disabled,
	                      * 7 recent large capture publication */
} shadowPublicationResult;

/* Caller holds surface->lock through staging. The helper reads the published
 * surface region directly; clientRefresh alone forces cache invalidation. */
BOOL shadow_publication_stage(shadowBitmapState* state, shadowPacer* pacer,
                              const rdpShadowSurface* surface, const REGION16* clientRefresh,
                              UINT64 nowMs, shadowPublicationResult* result);
/* Call only after the ordered update callback succeeds. A failed write leaves the
 * tile and its admission uncommitted for session error handling. */
void shadow_publication_commit(shadowBitmapState* state, shadowPacer* pacer,
                               const shadowBitmapTile* tile, UINT32 chargedBytes);

#endif
