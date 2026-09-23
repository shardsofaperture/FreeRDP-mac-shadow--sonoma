/* Production publication staging and commit regressions. Licensed under Apache-2.0. */
#include <freerdp/config.h>
#include <freerdp/codec/color.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../shadow_publication.h"
#include "../shadow_surface.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

static int drain(shadowBitmapState* state, shadowPacer* pacer, BYTE* client,
                 UINT32 width, UINT32 bpp, UINT64* ms)
{
    int count = 0;
    while (shadow_bitmap_pending(state))
    {
        shadowBitmapTile tile = { 0 };
        UINT32 stride = 0;
        const BYTE* latest = shadow_bitmap_pixels(state, &stride);
        shadow_pacer_observe(pacer, *ms, TRUE, 0, FALSE, TRUE);
        if (!shadow_pacer_preflight(pacer, 2048))
        {
            (*ms)++;
            continue;
        }
        CHECK(shadow_bitmap_next(state, FALSE, &tile));
        if (!shadow_pacer_can_submit(pacer, 128))
        {
            (*ms)++;
            continue;
        }
        for (UINT32 y = 0; y < tile.height; y++)
            memcpy(client + ((size_t)(tile.y + y) * width + tile.x) * bpp,
                   latest + (size_t)(tile.y + y) * stride + tile.x * bpp,
                   tile.width * bpp);
        shadow_publication_commit(state, pacer, &tile, 128);
        CHECK(++count < 4096);
        (*ms)++;
    }
    return count;
}

static int exercise(UINT32 depth, UINT32 format)
{
    const UINT32 width = 256, height = 192, bpp = depth / 8;
    rdpShadowSurface* surface = shadow_surface_new(NULL, 0, 0, width, height);
    BYTE* pixels = surface ? surface->data : NULL;
    if (surface) { surface->format = format; surface->scanline = width * bpp; }
    BYTE* client = calloc((size_t)width * height, bpp);
    shadowBitmapState* state = shadow_bitmap_new(width, height, depth, 0x3f0000);
    shadowPacer f, m;
    REGION16 refresh;
    shadowPublicationResult result = { 0 };
    UINT64 ms = 1000;
    region16_init(&refresh);
    CHECK(pixels && client && state);
    REGION16* publication = &surface->invalidRegion;
    CHECK(shadow_pacer_set_fixed_rate_kib(&f, 150));
    CHECK(shadow_pacer_set_fixed_rate_kib(&m, 150));
    CHECK(shadow_pacer_enable_large_refresh_diagnostic_m(&m));
    /* First send establishes known sent-cache tiles. */
    CHECK(shadow_publication_stage(state, &f, surface, &refresh, ms, &result));
    CHECK(drain(state, &f, client, width, bpp, &ms) == 12);
    CHECK(!shadow_bitmap_pending(state));
    const RECTANGLE_16 large = { 0, 0, 256, 96 };
    CHECK(region16_union_rect(publication, publication, &large));
    pixels[0] = 33;
    CHECK(shadow_publication_stage(state, &m, surface, &refresh, ms, &result));
    CHECK(result.clientRefreshArea == 0 && result.publicationArea == 256U * 96U);
    CHECK(result.qualified && result.burstStarted && result.denialReason == 0);
    CHECK(m.rate == 614400.0 && shadow_bitmap_pending_tiles(state) == 1);
    /* Failed write is not committed. A new publication replaces that unsent pixel. */
    shadowBitmapTile failed = { 0 }, newest = { 0 };
    CHECK(shadow_bitmap_next(state, FALSE, &failed));
    CHECK(shadow_pacer_can_submit(&m, 128));
    pixels[0] = 44;
    region16_clear(publication);
    CHECK(shadow_publication_stage(state, &m, surface, &refresh, ms + 1, &result));
    CHECK(result.publicationArea == 0 && !result.burstStarted);
    CHECK(shadow_bitmap_next(state, FALSE, &newest) && newest.index == failed.index);
    CHECK(drain(state, &m, client, width, bpp, &ms) == 1);
    CHECK(client[0] == 44 && memcmp(client, pixels, (size_t)width * height * bpp) == 0);
    /* Explicit client refresh invalidates cache despite identical pixels. */
    const RECTANGLE_16 tile = { 0, 0, 64, 64 };
    CHECK(region16_union_rect(&refresh, &refresh, &tile));
    client[0] = 99;
    CHECK(shadow_publication_stage(state, &m, surface, &refresh, ms, &result));
    CHECK(result.publicationArea == 0 && result.clientRefreshArea == 4096);
    CHECK(shadow_bitmap_pending_tiles(state) == 1);
    CHECK(drain(state, &m, client, width, bpp, &ms) == 1);
    CHECK(memcmp(client, pixels, (size_t)width * height * bpp) == 0);
    region16_clear(&refresh);
    /* Clipped area and normalized overlap; capture alone preserves known cache. */
    const RECTANGLE_16 quarter = { 0, 0, 128, 96 };
    const RECTANGLE_16 outside = { 240, 180, 300, 220 };
    CHECK(region16_union_rect(publication, publication, &quarter));
    CHECK(region16_union_rect(publication, publication, &quarter));
    CHECK(region16_union_rect(publication, publication, &outside));
    CHECK(shadow_publication_stage(state, &m, surface, &refresh, ms, &result));
    CHECK(result.publicationArea == 128U * 96U + 16U * 12U && result.qualified);
    CHECK(!shadow_bitmap_pending(state));
    const UINT64 recentlyLarge = m.lastPublicationDamageMs + m.burstDurationMs +
                                 m.burstCooldownMs + 1;
    CHECK(shadow_publication_stage(state, &m, surface, &refresh, recentlyLarge, &result));
    CHECK(result.qualified && !result.burstStarted && result.denialReason == 7);
    CHECK(shadow_publication_stage(state, &m, surface, &refresh,
                                   recentlyLarge + SHADOW_PACER_LARGE_REFRESH_QUIET_REARM_MS,
                                   &result));
    CHECK(result.qualified && result.burstStarted);
    /* A changed upper tile does not repeatedly put a lower pending tile behind
     * it: the production cursor survives successive newest-state stages. */
    region16_clear(publication);
    const RECTANGLE_16 full = { 0, 0, 256, 192 };
    const RECTANGLE_16 top = { 0, 0, 1, 1 };
    shadowBitmapState* progress = shadow_bitmap_new(width, height, depth, 0x3f0000);
    CHECK(progress && shadow_bitmap_stage(progress, pixels, format, width * bpp, NULL));
    while (shadow_bitmap_pending(progress))
    {
        shadowBitmapTile baseline = { 0 };
        CHECK(shadow_bitmap_next(progress, FALSE, &baseline));
        shadow_bitmap_commit(progress, &baseline);
    }
    CHECK(region16_union_rect(publication, publication, &full));
    pixels[0] = 45;
    pixels[((size_t)width * height - 1) * bpp] = 66;
    CHECK(shadow_publication_stage(progress, &f, surface, &refresh, ms, &result));
    shadowBitmapTile first = { 0 }, lower = { 0 };
    CHECK(shadow_bitmap_next(progress, FALSE, &first) && first.y == 0);
    CHECK(shadow_pacer_can_submit(&f, 128));
    UINT32 latestStride = 0;
    const BYTE* latest = shadow_bitmap_pixels(progress, &latestStride);
    for (UINT32 y = 0; y < first.height; y++)
        memcpy(client + ((size_t)(first.y + y) * width + first.x) * bpp,
               latest + (size_t)(first.y + y) * latestStride + first.x * bpp,
               first.width * bpp);
    shadow_publication_commit(progress, &f, &first, 128);
    region16_clear(publication);
    CHECK(region16_union_rect(publication, publication, &top));
    for (UINT32 generation = 0; generation < 20; generation++)
    {
        pixels[0] = (BYTE)(70 + generation);
        CHECK(shadow_publication_stage(progress, &f, surface, &refresh,
                                       ms + generation, &result));
    }
    CHECK(shadow_bitmap_next(progress, FALSE, &lower) && lower.y >= 128);
    CHECK(drain(progress, &f, client, width, bpp, &ms) == 2);
    CHECK(memcmp(client, pixels, (size_t)width * height * bpp) == 0);
    shadow_bitmap_free(progress);
    const UINT64 burstBytes = m.burstBytes;
    surface->data = NULL;
    CHECK(!shadow_publication_stage(state, &m, surface, &refresh, ms, &result));
    surface->data = pixels;
    CHECK(m.burstBytes == burstBytes && !region16_is_empty(publication));
    region16_uninit(&refresh);
    shadow_bitmap_free(state);
    free(client);
    shadow_surface_free(surface);
    return 0;
}

static int empty_event_keeps_deferred_wait(void)
{
    rdpShadowSurface* surface = shadow_surface_new(NULL, 0, 0, 64, 64);
    shadowBitmapState* state = shadow_bitmap_new(64, 64, 32, 0x3f0000);
    shadowPacer pacer;
    REGION16 refresh;
    shadowPublicationResult result = { 0 };
    region16_init(&refresh);
    CHECK(surface && state && shadow_pacer_set_fixed_rate_kib(&pacer, 150));
    surface->format = PIXEL_FORMAT_BGRX32;
    surface->scanline = 64 * 4;
    CHECK(shadow_publication_stage(state, &pacer, surface, &refresh, 0, &result));
    CHECK(shadow_bitmap_pending_tiles(state) == 1);
    CHECK(shadow_pacer_can_submit(&pacer, 15000));
    shadow_pacer_submitted(&pacer, 15000);
    CHECK(!shadow_pacer_can_submit(&pacer, 16000));
    CHECK(pacer.requiredCredit == 16000);
    CHECK(shadow_publication_stage(state, &pacer, surface, &refresh, 10, &result));
    CHECK(result.publicationArea == 0 && result.clientRefreshArea == 0);
    CHECK(pacer.requiredCredit == 16000 && !shadow_pacer_preflight(&pacer, 2048));
    const RECTANGLE_16 full = { 0, 0, 64, 64 };
    CHECK(region16_union_rect(&refresh, &refresh, &full));
    CHECK(shadow_publication_stage(state, &pacer, surface, &refresh, 11, &result));
    CHECK(result.clientRefreshArea == 4096);
    CHECK(pacer.requiredCredit == 0 && shadow_pacer_preflight(&pacer, 2048));
    region16_uninit(&refresh);
    shadow_bitmap_free(state);
    shadow_surface_free(surface);
    return 0;
}

int main(void)
{
    return exercise(32, PIXEL_FORMAT_BGRX32) || exercise(16, PIXEL_FORMAT_RGB16) ||
           empty_event_keeps_deferred_wait();
}
