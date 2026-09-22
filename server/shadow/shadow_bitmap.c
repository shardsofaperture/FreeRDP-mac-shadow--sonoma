/** FreeRDP shadow bitmap scheduling. Licensed under the Apache License, Version 2.0. */
#include <freerdp/config.h>
#include <freerdp/codec/color.h>
#include <stdlib.h>
#include <string.h>

#include "shadow_bitmap.h"

#define MAX_TILE_SIZE 64U
#define COPY_RADIUS 128
#define BITMAP_UPDATE_WIRE_OVERHEAD 30U
#define FASTPATH_FRAGMENT_HEADROOM 128U

struct shadow_bitmap_state
{
	UINT32 width, height, stride, columns, count, cursor, bitsPerPixel, bytesPerPixel, format;
	UINT32 maxRequestSize;
	UINT32 tileWidth, tileHeight;
	BYTE* latest;
	BYTE* sent;
	BYTE* known;
	BYTE* dirty;
	UINT32 pending;
	BOOL chooseDirection;
	BOOL reverseX;
	BOOL reverseY;
	BOOL haveMove;
	INT32 moveX, moveY;
	UINT32 lastCopyCandidateProbes;
};

UINT32 shadow_bitmap_color_depth(UINT32 requested)
{
	return (requested == 15 || requested == 16 || requested == 32) ? requested : 16;
}

BOOL shadow_bitmap_supported(const rdpSettings* settings, BOOL surfaceCommands)
{
	if (!settings || surfaceCommands)
		return FALSE;
	const UINT32 depth = freerdp_settings_get_uint32(settings, FreeRDP_ColorDepth);
	UINT32 tileWidth = 0, tileHeight = 0;
	return shadow_bitmap_tile_size(
	           depth, freerdp_settings_get_uint32(settings, FreeRDP_MultifragMaxRequestSize),
	           &tileWidth, &tileHeight) &&
	       !freerdp_settings_get_bool(settings, FreeRDP_SupportGraphicsPipeline) &&
	       !surfaceCommands;
}

BOOL shadow_bitmap_tile_size(UINT32 bitsPerPixel, UINT32 maxRequestSize, UINT32* width,
                             UINT32* height)
{
	if (!width || !height || ((bitsPerPixel != 16) && (bitsPerPixel != 32)) ||
	    (maxRequestSize <= BITMAP_UPDATE_WIRE_OVERHEAD + FASTPATH_FRAGMENT_HEADROOM))
		return FALSE;
	/* Interleaved 16-bit data can require close to three bytes per pixel; planar 32-bit can
	 * require four. Reserve the same 128-byte framing margin as FreeRDP's safe fast-path size. */
	const UINT32 worstBytesPerPixel = (bitsPerPixel == 16) ? 3U : 4U;
	const UINT32 maxPixels = (maxRequestSize - BITMAP_UPDATE_WIRE_OVERHEAD -
	                          FASTPATH_FRAGMENT_HEADROOM) /
	                         worstBytesPerPixel;
	UINT32 tileWidth = MIN(MAX_TILE_SIZE, (maxPixels / 8U) & ~3U);
	if (tileWidth < 8U)
		return FALSE;
	UINT32 tileHeight = MIN(MAX_TILE_SIZE, maxPixels / tileWidth);
	if (tileHeight < 8U)
		return FALSE;
	*width = tileWidth;
	*height = tileHeight;
	return TRUE;
}

BOOL shadow_bitmap_pack(BYTE* packed, const BYTE* pixels, UINT32 stride, UINT32 x, UINT32 y,
                        UINT32 width, UINT32 height)
{
	if (!packed || !pixels || !width || !height || width > 64 || height > 64 ||
	    (UINT64)(x + (UINT64)width) * 4 > stride)
		return FALSE;
	memset(packed, 0, 64U * 64U * 4U);
	for (UINT32 row = 0; row < height; row++)
		memcpy(packed + row * 256U, pixels + ((size_t)y + row) * stride + (size_t)x * 4U,
		       width * 4U);
	return TRUE;
}

shadowBitmapState* shadow_bitmap_new(UINT32 width, UINT32 height, UINT32 bitsPerPixel,
                                     UINT32 maxRequestSize)
{
	/* Bound per-client storage, independently of a client's advertised dimensions. */
	if (!width || !height || (width > 8192) || (height > 8192) ||
	    ((UINT64)width * height > 16U * 1024U * 1024U) ||
	    ((bitsPerPixel != 16) && (bitsPerPixel != 32)))
		return nullptr;
	shadowBitmapState* state = calloc(1, sizeof(*state));
	if (!state)
		return nullptr;
	state->width = width;
	state->height = height;
	state->bitsPerPixel = bitsPerPixel;
	state->maxRequestSize = maxRequestSize;
	state->bytesPerPixel = bitsPerPixel / 8U;
	state->format = (bitsPerPixel == 16) ? PIXEL_FORMAT_RGB16 : PIXEL_FORMAT_BGRX32;
	if (!shadow_bitmap_tile_size(bitsPerPixel, maxRequestSize, &state->tileWidth,
	                             &state->tileHeight))
	{
		free(state);
		return nullptr;
	}
	state->stride = ((width + 3U) & ~3U) * state->bytesPerPixel;
	state->columns = (width + state->tileWidth - 1) / state->tileWidth;
	state->count =
	    state->columns * ((height + state->tileHeight - 1) / state->tileHeight);
	state->latest = calloc(height, state->stride);
	state->sent = calloc(height, state->stride);
	state->known = calloc(state->count, 1);
	state->dirty = calloc(state->count, 1);
	if (!state->latest || !state->sent || !state->known || !state->dirty)
	{
		shadow_bitmap_free(state);
		return nullptr;
	}
	return state;
}

void shadow_bitmap_free(shadowBitmapState* state)
{
	if (!state)
		return;
	free(state->latest);
	free(state->sent);
	free(state->known);
	free(state->dirty);
	free(state);
}

BOOL shadow_bitmap_size_matches(const shadowBitmapState* state, UINT32 width, UINT32 height,
	                            UINT32 bitsPerPixel, UINT32 maxRequestSize)
{
	return state && (state->width == width) && (state->height == height) &&
	       (state->bitsPerPixel == bitsPerPixel) && (state->maxRequestSize == maxRequestSize);
}

static shadowBitmapTile bitmap_tile(const shadowBitmapState* state, UINT32 index)
{
	shadowBitmapTile tile = { 0 };
	tile.index = index;
	tile.x = (index % state->columns) * state->tileWidth;
	tile.y = (index / state->columns) * state->tileHeight;
	tile.width = MIN(state->tileWidth, state->width - tile.x);
	tile.height = MIN(state->tileHeight, state->height - tile.y);
	return tile;
}

static BOOL pixels_equal(const shadowBitmapState* state, const shadowBitmapTile* tile,
                         UINT32 sourceX, UINT32 sourceY)
{
	const BYTE* latest = state->latest +
	                     (size_t)tile->y * state->stride + tile->x * state->bytesPerPixel;
	const BYTE* sent = state->sent +
	                   (size_t)sourceY * state->stride + sourceX * state->bytesPerPixel;
	for (UINT32 row = 0; row < tile->height; row++)
	{
		if (memcmp(latest, sent, tile->width * state->bytesPerPixel) != 0)
			return FALSE;
		latest += state->stride;
		sent += state->stride;
	}
	return TRUE;
}

BOOL shadow_bitmap_stage(shadowBitmapState* state, const BYTE* pixels, UINT32 format, UINT32 stride,
                         const REGION16* refresh)
{
	if (!state || !pixels)
		return FALSE;
	/* Compare in the negotiated RGB565 space. Changes discarded by quantization need no PDU.
	 * Replacing latest does not change sent: unsent damage survives arbitrarily many stages. */
	if (!freerdp_image_copy_no_overlap(state->latest, state->format, state->stride, 0, 0,
	                                   state->width, state->height, pixels, format, stride, 0, 0,
	                                   nullptr, FREERDP_FLIP_NONE))
		return FALSE;
	state->pending = 0;
	state->chooseDirection = TRUE;
	state->haveMove = FALSE;
	state->lastCopyCandidateProbes = 0;
	for (UINT32 index = 0; index < state->count; index++)
	{
		const shadowBitmapTile tile = bitmap_tile(state, index);
		const RECTANGLE_16 rect = { (UINT16)tile.x, (UINT16)tile.y, (UINT16)(tile.x + tile.width),
			                        (UINT16)(tile.y + tile.height) };
		if (refresh && region16_intersects_rect(refresh, &rect))
			state->known[index] = FALSE;
		state->dirty[index] = !state->known[index] || !pixels_equal(state, &tile, tile.x, tile.y);
		state->pending += state->dirty[index] ? 1U : 0U;
	}
	return TRUE;
}

BOOL shadow_bitmap_pending(const shadowBitmapState* state)
{
	return state && (state->pending > 0);
}

static BOOL source_known(const shadowBitmapState* state, const shadowBitmapTile* tile, UINT32 x,
                         UINT32 y)
{
	for (UINT32 row = y / state->tileHeight;
	     row <= (y + tile->height - 1) / state->tileHeight; row++)
	{
		for (UINT32 col = x / state->tileWidth;
		     col <= (x + tile->width - 1) / state->tileWidth; col++)
		{
			if (!state->known[row * state->columns + col])
				return FALSE;
		}
	}
	return TRUE;
}

static BOOL find_copy(shadowBitmapState* state, shadowBitmapTile* tile)
{
	/* Bounded search, including non-tile-aligned scrolls and diagonal window moves. Check a
	 * short signature before the exact rectangle; never trust a hash or an unsent source.
	 * Each committed copy updates sent, so overlapping subsequent copies remain correct. */
	state->lastCopyCandidateProbes = 0;
	if ((tile->width < 8) || (tile->height < 8) || !state->known[tile->index])
		return FALSE;
	const INT32 minX = MAX(0, (INT32)tile->x - COPY_RADIUS);
	const INT32 minY = MAX(0, (INT32)tile->y - COPY_RADIUS);
	const INT32 maxX = MIN((INT32)(state->width - tile->width), (INT32)tile->x + COPY_RADIUS);
	const INT32 maxY = MIN((INT32)(state->height - tile->height), (INT32)tile->y + COPY_RADIUS);
	const BYTE* signature = state->latest +
	                        (size_t)tile->y * state->stride + tile->x * state->bytesPerPixel;
	UINT32 comparisons = 0;
	UINT32 candidateProbes = 0;
	/* Visit candidates from the tile outward.  This keeps the common small scroll
	 * case warm while the explicit candidate limit bounds the worst case. */
	for (INT32 radius = 0; radius <= COPY_RADIUS; radius++)
	{
		for (INT32 dy = -radius; dy <= radius; dy++)
		{
			const INT32 y = (INT32)tile->y + dy;
			if ((y < minY) || (y > maxY))
				continue;
			for (INT32 dx = -radius; dx <= radius; dx++)
			{
				if ((MAX(abs(dx), abs(dy)) != radius))
					continue;
				const INT32 x = (INT32)tile->x + dx;
				if ((x < minX) || (x > maxX) || ((x == (INT32)tile->x) &&
				                                      (y == (INT32)tile->y)))
					continue;
				if (candidateProbes >= SHADOW_BITMAP_COPY_CANDIDATE_LIMIT)
				{
					state->lastCopyCandidateProbes = candidateProbes;
					return FALSE;
				}
				candidateProbes++;
				const BYTE* candidate = state->sent +
				                        (size_t)y * state->stride + (size_t)x * state->bytesPerPixel;
				const size_t signatureBytes = 8U * state->bytesPerPixel;
				if (memcmp(signature, candidate, signatureBytes) != 0)
					continue;
				const size_t last =
				    (size_t)(tile->height - 1) * state->stride +
				    (tile->width - 8U) * state->bytesPerPixel;
				const size_t middle = (size_t)(tile->height / 2U) * state->stride;
				if ((memcmp(signature + last, candidate + last, signatureBytes) != 0) ||
				    (memcmp(signature + middle, candidate + middle, signatureBytes) != 0))
					continue;
				/* Flat/repetitive content must not turn a bounded search into thousands of
				 * full-tile comparisons. Falling back to a bitmap is always safe. */
				if (++comparisons > 32)
				{
					state->lastCopyCandidateProbes = candidateProbes;
					return FALSE;
				}
				if (source_known(state, tile, (UINT32)x, (UINT32)y) &&
				    pixels_equal(state, tile, (UINT32)x, (UINT32)y))
				{
					tile->sourceX = (UINT32)x;
					tile->sourceY = (UINT32)y;
					tile->copy = TRUE;
					state->lastCopyCandidateProbes = candidateProbes;
					return TRUE;
				}
			}
		}
	}
	state->lastCopyCandidateProbes = candidateProbes;
	return FALSE;
}

static shadowBitmapTile trim_damage(const shadowBitmapState* state, shadowBitmapTile tile)
{
	if (!state->known[tile.index])
		return tile;
	UINT32 left = tile.x + tile.width, top = tile.y + tile.height;
	UINT32 right = tile.x, bottom = tile.y;
	for (UINT32 y = tile.y; y < tile.y + tile.height; y++)
	{
		for (UINT32 x = tile.x; x < tile.x + tile.width; x++)
		{
			const size_t offset =
			    (size_t)y * state->stride + x * state->bytesPerPixel;
			if (memcmp(state->latest + offset, state->sent + offset,
			           state->bytesPerPixel) != 0)
			{
				left = MIN(left, x);
				top = MIN(top, y);
				right = MAX(right, x + 1U);
				bottom = MAX(bottom, y + 1U);
			}
		}
	}
	if ((right > left) && (bottom > top))
	{
		tile.x = left;
		tile.y = top;
		tile.width = right - left;
		tile.height = bottom - top;
	}
	return tile;
}

static BOOL copy_moved_part(const shadowBitmapState* state, shadowBitmapTile* tile)
{
	if (!state->haveMove || !state->known[tile->index])
		return FALSE;
	shadowBitmapTile part = *tile;
	const INT32 left = MAX((INT32)tile->x, -state->moveX);
	const INT32 top = MAX((INT32)tile->y, -state->moveY);
	const INT32 right = MIN((INT32)(tile->x + tile->width), (INT32)state->width - state->moveX);
	const INT32 bottom = MIN((INT32)(tile->y + tile->height), (INT32)state->height - state->moveY);
	if ((right <= left) || (bottom <= top))
		return FALSE;
	part.x = (UINT32)left;
	part.y = (UINT32)top;
	part.width = (UINT32)(right - left);
	part.height = (UINT32)(bottom - top);
	part.sourceX = (UINT32)(left + state->moveX);
	part.sourceY = (UINT32)(top + state->moveY);
	if (pixels_equal(state, &part, part.x, part.y))
		return FALSE; /* Copying an already-correct subrectangle would make no progress. */
	if (!source_known(state, &part, part.sourceX, part.sourceY) ||
	    !pixels_equal(state, &part, part.sourceX, part.sourceY))
		return FALSE;
	part.copy = TRUE;
	*tile = part;
	return TRUE;
}

BOOL shadow_bitmap_next(shadowBitmapState* state, BOOL allowCopy, shadowBitmapTile* tile)
{
	if (!state || !tile || !state->pending)
		return FALSE;
	if (allowCopy && state->chooseDirection)
	{
		/* Copy away from the source, as memmove would. Otherwise early bitmap/copy
		 * writes can destroy the source needed by the rest of a downward/rightward move. */
		const UINT32 probes[] = { state->count / 2U, state->count - 1U, 0 };
		state->chooseDirection = FALSE;
		for (size_t n = 0; n < ARRAYSIZE(probes); n++)
		{
			shadowBitmapTile probe = bitmap_tile(state, probes[n]);
			if (state->dirty[probe.index] && find_copy(state, &probe))
			{
				state->haveMove = TRUE;
				state->moveX = (INT32)probe.sourceX - (INT32)probe.x;
				state->moveY = (INT32)probe.sourceY - (INT32)probe.y;
				const BOOL reverseX = probe.sourceX < probe.x;
				const BOOL reverseY = probe.sourceY < probe.y;
				if ((reverseX != state->reverseX) || (reverseY != state->reverseY))
					state->cursor = 0;
				state->reverseX = reverseX;
				state->reverseY = reverseY;
				break;
			}
		}
	}
	for (UINT32 n = 0; n < state->count; n++)
	{
		const UINT32 ordinal = (state->cursor + n) % state->count;
		UINT32 row = ordinal / state->columns;
		UINT32 col = ordinal % state->columns;
		if (state->reverseY)
			row = state->count / state->columns - 1U - row;
		if (state->reverseX)
			col = state->columns - 1U - col;
		const UINT32 index = row * state->columns + col;
		if (!state->dirty[index])
			continue;
		*tile = trim_damage(state, bitmap_tile(state, index));
		if (allowCopy && !copy_moved_part(state, tile))
			(void)find_copy(state, tile);
		return TRUE;
	}
	return FALSE;
}

void shadow_bitmap_commit(shadowBitmapState* state, const shadowBitmapTile* tile)
{
	if (!state || !tile || (tile->index >= state->count) || !state->dirty[tile->index])
		return;
	for (UINT32 row = 0; row < tile->height; row++)
	{
		const size_t offset = (size_t)(tile->y + row) * state->stride +
		                      tile->x * state->bytesPerPixel;
		memcpy(state->sent + offset, state->latest + offset,
		       tile->width * state->bytesPerPixel);
	}
	const shadowBitmapTile full = bitmap_tile(state, tile->index);
	if ((tile->x == full.x) && (tile->y == full.y) && (tile->width == full.width) &&
	    (tile->height == full.height))
		state->known[tile->index] = TRUE;
	state->dirty[tile->index] =
	    !state->known[tile->index] || !pixels_equal(state, &full, full.x, full.y);
	if (!state->dirty[tile->index])
		state->pending--;
	UINT32 row = tile->index / state->columns;
	UINT32 col = tile->index % state->columns;
	if (state->reverseY)
		row = state->count / state->columns - 1U - row;
	if (state->reverseX)
		col = state->columns - 1U - col;
	state->cursor =
	    (row * state->columns + col + (state->dirty[tile->index] ? 0U : 1U)) % state->count;
}

const BYTE* shadow_bitmap_pixels(const shadowBitmapState* state, UINT32* stride)
{
	if (!state || !stride)
		return nullptr;
	*stride = state->stride;
	return state->latest;
}

UINT32 shadow_bitmap_last_copy_candidate_probes(const shadowBitmapState* state)
{
	return state ? state->lastCopyCandidateProbes : 0;
}
