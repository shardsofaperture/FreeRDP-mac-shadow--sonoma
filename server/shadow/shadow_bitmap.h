/** FreeRDP shadow bitmap scheduling. Licensed under the Apache License, Version 2.0. */
#ifndef FREERDP_SHADOW_BITMAP_H
#define FREERDP_SHADOW_BITMAP_H

#include <freerdp/codec/region.h>
#include <winpr/wtypes.h>
#include <freerdp/settings.h>

typedef struct shadow_bitmap_state shadowBitmapState;
typedef struct
{
	UINT32 index;
	UINT32 x, y, width, height;
	UINT32 sourceX, sourceY;
	BOOL copy;
} shadowBitmapTile;

/* A warm-cache probe is a hint.  Exceeding this bound falls back to a bitmap
 * update, which is always safe and keeps a repetitive frame from monopolizing
 * the shadow session. */
#define SHADOW_BITMAP_COPY_CANDIDATE_LIMIT 16384U

UINT32 shadow_bitmap_color_depth(UINT32 requested);
BOOL shadow_bitmap_supported(const rdpSettings* settings, BOOL surfaceCommands);
BOOL shadow_bitmap_pack(BYTE* packed, const BYTE* pixels, UINT32 stride, UINT32 x, UINT32 y,
                        UINT32 width, UINT32 height);
BOOL shadow_bitmap_tile_size(UINT32 bitsPerPixel, UINT32 maxRequestSize, UINT32* width,
                             UINT32* height);
shadowBitmapState* shadow_bitmap_new(UINT32 width, UINT32 height, UINT32 bitsPerPixel,
                                     UINT32 maxRequestSize);
void shadow_bitmap_free(shadowBitmapState* state);
BOOL shadow_bitmap_size_matches(const shadowBitmapState* state, UINT32 width, UINT32 height,
	                            UINT32 bitsPerPixel, UINT32 maxRequestSize);
BOOL shadow_bitmap_stage(shadowBitmapState* state, const BYTE* pixels, UINT32 format, UINT32 stride,
                         const REGION16* refresh);
BOOL shadow_bitmap_pending(const shadowBitmapState* state);
BOOL shadow_bitmap_next(shadowBitmapState* state, BOOL allowCopy, shadowBitmapTile* tile);
void shadow_bitmap_commit(shadowBitmapState* state, const shadowBitmapTile* tile);
const BYTE* shadow_bitmap_pixels(const shadowBitmapState* state, UINT32* stride);
UINT32 shadow_bitmap_last_copy_candidate_probes(const shadowBitmapState* state);

#endif
