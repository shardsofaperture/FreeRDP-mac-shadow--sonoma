/* Deterministic latest-state publication workload. Apache-2.0. */
#include <freerdp/config.h>
#include <freerdp/codec/color.h>
#include <freerdp/codec/interleaved.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../shadow_publication.h"
#include "../shadow_surface.h"

#define WIDTH 512U
#define HEIGHT 384U
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

typedef struct
{
	UINT32 captures, publications, maxPending, maxAgeMs, menuDelayMs, convergedMs;
	UINT64 chargedBytes, restagedPendingTiles;
} Result;

/* The test uses the real bitmap encoder's interleaved codec and production
 * staging/commit/pacer functions. Output framing is represented by 94 bytes. */
static UINT32 encoded_charge(BITMAP_INTERLEAVED_CONTEXT* codec, const shadowBitmapState* state,
                             const shadowBitmapTile* tile)
{
	BYTE packed[64 * 64 * 2] = { 0 };
	BYTE output[64 * 64 * 4] = { 0 };
	UINT32 stride = 0, length = sizeof(output);
	const BYTE* pixels = shadow_bitmap_pixels(state, &stride);
	const UINT32 encodedWidth = (tile->width + 3U) & ~3U;
	for (UINT32 row = 0; row < tile->height; row++)
		memcpy(packed + row * 128U,
		       pixels + (size_t)(tile->y + row) * stride + tile->x * 2U,
		       tile->width * 2U);
	CHECK(interleaved_compress(codec, output, &length, encodedWidth, tile->height,
	                           packed, PIXEL_FORMAT_RGB16, 128, 0, 0, nullptr, 16));
	return length + 94U;
}

static void change(rdpShadowSurface* surface, const RECTANGLE_16* rect, UINT32 generation)
{
	UINT16* pixels = (UINT16*)surface->data;
	for (UINT32 y = rect->top; y < rect->bottom; y++)
		for (UINT32 x = rect->left; x < rect->right; x++)
			pixels[y * WIDTH + x] =
			    (UINT16)((x * 193U + y * 977U + generation * 1307U) ^
			             ((x + generation) * (y + 17U)));
	CHECK(region16_union_rect(&surface->invalidRegion, &surface->invalidRegion, rect));
}

/* kind 0: one large reveal, 1: small icon/menu changes, 2: video plus a menu.
 * The schedule models the production first-damage 50 ms gate. The Mac test
 * independently checks the actual gate, damage union, and publish path. */
static Result run(UINT32 kind, UINT32 aggregateMs)
{
	Result result = { 0 };
	rdpShadowSurface* surface = shadow_surface_new(NULL, 0, 0, WIDTH, HEIGHT);
	shadowBitmapState* state = shadow_bitmap_new(WIDTH, HEIGHT, 16, 0x3f0000);
	BITMAP_INTERLEAVED_CONTEXT* codec = bitmap_interleaved_context_new(TRUE);
	UINT16* client = calloc(WIDTH * HEIGHT, sizeof(UINT16));
	shadowPacer pacer = { 0 };
	REGION16 refresh;
	UINT32 pendingStart = 0, firstCapture = 0, menuAt = 0;
	CHECK(surface && state && codec && client);
	surface->format = PIXEL_FORMAT_RGB16;
	surface->scanline = WIDTH * 2U;
	region16_init(&refresh);
	CHECK(shadow_pacer_set_fixed_rate_kib(&pacer, 250));
	CHECK(pacer.fixed && pacer.baseRate == 256000.0 &&
	      !pacer.largeRefreshBurstEnabled);
	CHECK(shadow_bitmap_stage(state, surface->data, surface->format,
	                          surface->scanline, NULL));
	/* Start with a warm, correct client cache. */
	while (shadow_bitmap_pending(state))
	{
		shadowBitmapTile tile = { 0 };
		CHECK(shadow_bitmap_next(state, FALSE, &tile));
		shadow_bitmap_commit(state, &tile);
	}
	const RECTANGLE_16 large = { 64, 48, 448, 336 };
	const RECTANGLE_16 icon = { 448, 320, 480, 352 };
	const RECTANGLE_16 video = { 0, 0, 512, 256 };
	const RECTANGLE_16 menu = { 0, 320, 64, 384 };
	for (UINT32 ms = 1000; ms < 30000; ms++)
	{
		BOOL event = FALSE;
		if (kind == 0 && ms == 1000)
		{
			change(surface, &large, 1);
			event = TRUE;
		}
		else if (kind == 1 && ms < 1400 && (ms - 1000) % 20 == 0)
		{
			change(surface, &icon, (ms - 1000) / 20 + 1);
			event = TRUE;
		}
		else if (kind == 2)
		{
			if (ms < 2000 && (ms - 1000) % 10 == 0)
			{
				change(surface, &video, (ms - 1000) / 10 + 1);
				event = TRUE;
			}
			if (ms == 1500)
			{
				change(surface, &menu, 101);
				menuAt = ms;
				event = TRUE;
			}
		}
		if (event)
		{
			result.captures++;
			if (!firstCapture)
				firstCapture = ms;
		}
		if (firstCapture && ms >= firstCapture + aggregateMs)
		{
			shadowPublicationResult publication = { 0 };
			result.restagedPendingTiles += shadow_bitmap_pending_tiles(state);
			CHECK(shadow_publication_stage(state, &pacer, surface, &refresh, ms,
			                              &publication));
			result.publications++;
			region16_clear(&surface->invalidRegion);
			firstCapture = 0;
			if (shadow_bitmap_pending(state) && !pendingStart)
				pendingStart = ms;
		}
		shadow_pacer_observe(&pacer, ms, TRUE, 0, FALSE, shadow_bitmap_pending(state));
		const UINT32 pending = shadow_bitmap_pending_tiles(state);
		result.maxPending = MAX(result.maxPending, pending);
		if (pendingStart && pending)
			result.maxAgeMs = MAX(result.maxAgeMs, ms - pendingStart);
		if (pending && shadow_pacer_preflight(&pacer, 2048))
		{
			shadowBitmapTile tile = { 0 };
			CHECK(shadow_bitmap_next(state, FALSE, &tile));
			const UINT32 charge = encoded_charge(codec, state, &tile);
			if (shadow_pacer_can_submit(&pacer, charge))
			{
				UINT32 stride = 0;
				const BYTE* latest = shadow_bitmap_pixels(state, &stride);
				for (UINT32 row = 0; row < tile.height; row++)
					memcpy(client + (size_t)(tile.y + row) * WIDTH + tile.x,
					       latest + (size_t)(tile.y + row) * stride + tile.x * 2U,
					       tile.width * 2U);
				shadow_publication_commit(state, &pacer, &tile, charge);
				result.chargedBytes += charge;
			}
		}
		if (!shadow_bitmap_pending(state))
			pendingStart = 0;
		if (menuAt && !result.menuDelayMs && client[320U * WIDTH] ==
		    ((UINT16*)surface->data)[320U * WIDTH])
			result.menuDelayMs = ms - menuAt;
		if (ms > (kind == 2 ? 2000U : kind == 1 ? 1400U : 1000U) &&
		    !firstCapture && !shadow_bitmap_pending(state) &&
		    memcmp(client, surface->data, WIDTH * HEIGHT * 2U) == 0)
		{
			result.convergedMs = ms - 1000U;
			break;
		}
	}
	CHECK(result.convergedMs && result.publications && result.captures);
	CHECK(!pacer.largeRefreshBurstEnabled && pacer.baseRate == 256000.0);
	printf("%s %s captures=%u publications=%u charged=%llu maxPending=%u "
	       "maxPendingAgeMs=%u restagedPendingTiles=%llu menuDelayMs=%u "
	       "convergedMs=%u retainedGenerations=1\n",
	       kind == 0 ? "static" : kind == 1 ? "sparse" : "video",
	       aggregateMs ? "50ms" : "immediate", result.captures,
	       result.publications, (unsigned long long)result.chargedBytes,
	       result.maxPending, result.maxAgeMs,
	       (unsigned long long)result.restagedPendingTiles,
	       result.menuDelayMs, result.convergedMs);
	region16_uninit(&refresh);
	bitmap_interleaved_context_free(codec);
	shadow_bitmap_free(state);
	shadow_surface_free(surface);
	free(client);
	return result;
}

int main(void)
{
	for (UINT32 kind = 0; kind < 3; kind++)
	{
		const Result immediate = run(kind, 0);
		const Result aggregated = run(kind, 50);
		CHECK(immediate.captures == aggregated.captures);
		CHECK(aggregated.publications <= immediate.publications);
		if (kind)
			CHECK(aggregated.publications < immediate.publications);
		if (kind == 2)
			CHECK(aggregated.menuDelayMs && immediate.menuDelayMs);
	}
	return 0;
}
