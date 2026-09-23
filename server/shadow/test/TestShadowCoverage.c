/* Production shadow bitmap traversal regression. Apache-2.0. */
#include <freerdp/config.h>
#include <freerdp/codec/color.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../shadow_publication.h"
#include "../shadow_surface.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

static int broad_publication_spreads_early_rows(void)
{
    rdpShadowSurface* surface = shadow_surface_new(NULL, 0, 0, 512, 512);
    shadowBitmapState* state = shadow_bitmap_new(512, 512, 32, 0x3f0000);
    shadowPacer pacer;
    shadowPublicationResult result = { 0 };
    REGION16 refresh;
    region16_init(&refresh);
    CHECK(surface && state && shadow_pacer_set_fixed_rate_kib(&pacer, 150));
    shadow_bitmap_enable_coverage(state, TRUE);
    surface->format = PIXEL_FORMAT_BGRX32;
    surface->scanline = 512 * 4;
    const RECTANGLE_16 full = { 0, 0, 512, 512 };
    CHECK(region16_union_rect(&surface->invalidRegion, &surface->invalidRegion, &full));
    CHECK(shadow_publication_stage(state, &pacer, surface, &refresh, 1000, &result));
    CHECK(result.publicationArea == 512U * 512U);
    const shadowBitmapScheduleState schedule = shadow_bitmap_schedule_state(state);
    CHECK(schedule.active && schedule.rows == 8 && schedule.columns == 8);
    CHECK(schedule.rowStep == 3 && schedule.cursor == 0);
    UINT32 minY = UINT32_MAX, maxY = 0;
    for (UINT32 n = 0; n < 8; n++)
    {
        shadowBitmapTile tile = { 0 };
        CHECK(shadow_bitmap_next(state, FALSE, &tile));
        minY = MIN(minY, tile.y);
        maxY = MAX(maxY, tile.y);
        shadow_bitmap_commit(state, &tile);
    }
    printf("first eight tiles: minY=%u maxY=%u\n", minY, maxY);
    CHECK(maxY - minY >= 384);
    CHECK(shadow_bitmap_schedule_state(state).cursor == 8);
    region16_uninit(&refresh);
    shadow_bitmap_free(state);
    shadow_surface_free(surface);
    return 0;
}

static void paint(rdpShadowSurface* surface, UINT32 depth, UINT32 seed)
{
    const UINT32 bpp = depth / 8U;
    for (UINT32 y = 0; y < surface->height; y++)
        for (UINT32 x = 0; x < surface->width; x++)
        {
            const size_t offset = ((size_t)y * surface->width + x) * bpp;
            if (depth == 16)
            {
                const UINT16 value = (UINT16)(((x + seed) & 31U) |
                                      (((y + seed * 3U) & 63U) << 5U) |
                                      (((x + y + seed) & 31U) << 11U));
                memcpy(surface->data + offset, &value, 2);
            }
            else
            {
                surface->data[offset] = (BYTE)(x * 17U + seed);
                surface->data[offset + 1] = (BYTE)(y * 13U + seed * 3U);
                surface->data[offset + 2] = (BYTE)(x + y + seed * 7U);
                surface->data[offset + 3] = 0;
            }
        }
}

static BOOL apply_tile(shadowBitmapState* state, BYTE* client, UINT32 width, UINT32 bpp,
                       const shadowBitmapTile* tile)
{
    BYTE copy[64 * 64 * 4];
    UINT32 stride = 0;
    const BYTE* latest = shadow_bitmap_pixels(state, &stride);
    if (!latest || tile->width > 64 || tile->height > 64)
        return FALSE;
    for (UINT32 y = 0; y < tile->height; y++)
    {
        const BYTE* src = tile->copy
            ? client + ((size_t)(tile->sourceY + y) * width + tile->sourceX) * bpp
            : latest + (size_t)(tile->y + y) * stride + tile->x * bpp;
        memcpy(copy + (size_t)y * tile->width * bpp, src, tile->width * bpp);
    }
    for (UINT32 y = 0; y < tile->height; y++)
        memcpy(client + ((size_t)(tile->y + y) * width + tile->x) * bpp,
               copy + (size_t)y * tile->width * bpp, tile->width * bpp);
    return TRUE;
}

static BOOL deliver(shadowBitmapState* state, BYTE* client, UINT32 width, UINT32 bpp,
                    const shadowBitmapTile* tile)
{
    if (!apply_tile(state, client, width, bpp, tile))
        return FALSE;
    shadow_bitmap_commit(state, tile);
    return TRUE;
}

static int drain(shadowBitmapState* state, BYTE* client, UINT32 width, UINT32 bpp,
                 BOOL allowCopy, UINT32* copies)
{
    int operations = 0;
    while (shadow_bitmap_pending(state))
    {
        shadowBitmapTile tile = { 0 };
        CHECK(shadow_bitmap_next(state, allowCopy, &tile));
        if (tile.copy && copies) (*copies)++;
        CHECK(deliver(state, client, width, bpp, &tile));
        CHECK(++operations < 4096);
    }
    return operations;
}

static int control_replacement_and_convergence(UINT32 depth, UINT32 format)
{
    const UINT32 width = 512, height = 512, bpp = depth / 8U;
    rdpShadowSurface* surface = shadow_surface_new(NULL, 0, 0, width, height);
    shadowBitmapState* state = shadow_bitmap_new(width, height, depth, 0x3f0000);
    shadowBitmapState* control = shadow_bitmap_new(width, height, depth, 0x3f0000);
    BYTE* client = calloc((size_t)width * height, bpp);
    shadowPacer pacer;
    shadowPublicationResult result = { 0 };
    REGION16 refresh;
    region16_init(&refresh);
    CHECK(surface && state && control && client && shadow_pacer_set_fixed_rate_kib(&pacer, 150));
    surface->format = format;
    surface->scanline = width * bpp;
    shadow_bitmap_enable_coverage(state, TRUE);

    /* A reconnect without a published region still completes the first frame. */
    paint(surface, depth, 1);
    CHECK(shadow_publication_stage(state, &pacer, surface, &refresh, 1000, &result));
    CHECK(!shadow_bitmap_schedule_state(state).active);
    CHECK(drain(state, client, width, bpp, FALSE, NULL) == 64);
    CHECK(memcmp(client, surface->data, (size_t)width * height * bpp) == 0);

    /* A small publication stays on the ordinary traversal. */
    surface->data[((size_t)400 * width + 400) * bpp] ^= 0x7f;
    const RECTANGLE_16 small = { 400, 400, 401, 401 };
    CHECK(region16_union_rect(&surface->invalidRegion, &surface->invalidRegion, &small));
    CHECK(shadow_publication_stage(state, &pacer, surface, &refresh, 1100, &result));
    CHECK(!shadow_bitmap_schedule_state(state).active);
    CHECK(shadow_bitmap_pending_tiles(state) == 1);
    CHECK(drain(state, client, width, bpp, FALSE, NULL) == 1);
    CHECK(memcmp(client, surface->data, (size_t)width * height * bpp) == 0);
    region16_clear(&surface->invalidRegion);

    /* F remains row-major for the same new broad frame. */
    paint(surface, depth, 2);
    CHECK(shadow_bitmap_stage(control, surface->data, format, surface->scanline, NULL));
    for (UINT32 n = 0; n < 8; n++)
    {
        shadowBitmapTile tile = { 0 };
        CHECK(shadow_bitmap_next(control, FALSE, &tile) && tile.y == 0);
        shadow_bitmap_commit(control, &tile);
    }
    const RECTANGLE_16 full = { 0, 0, 512, 512 };
    CHECK(region16_union_rect(&surface->invalidRegion, &surface->invalidRegion, &full));
    CHECK(shadow_publication_stage(state, &pacer, surface, &refresh, 1200, &result));
    CHECK(shadow_bitmap_schedule_state(state).active);
    CHECK(shadow_bitmap_pending_tiles(state) == 64);

    /* Selection and a failed/deferred admission cannot advance cache or cursor. */
    shadowBitmapTile first = { 0 }, retry = { 0 };
    CHECK(shadow_bitmap_next(state, FALSE, &first));
    const UINT32 cursor = shadow_bitmap_schedule_state(state).cursor;
    const UINT32 pending = shadow_bitmap_pending_tiles(state);
    CHECK(shadow_pacer_can_submit(&pacer, 15000));
    shadow_pacer_submitted(&pacer, 15000);
    CHECK(!shadow_pacer_can_submit(&pacer, 16000));
    CHECK(shadow_bitmap_next(state, FALSE, &retry) && retry.index == first.index);
    CHECK(shadow_bitmap_schedule_state(state).cursor == cursor);
    CHECK(shadow_bitmap_pending_tiles(state) == pending);

    /* Newer pixels replace the unsent frame without moving its cursor. */
    paint(surface, depth, 3);
    CHECK(shadow_publication_stage(state, &pacer, surface, &refresh, 1300, &result));
    CHECK(shadow_bitmap_next(state, FALSE, &retry) && retry.index == first.index);
    CHECK(shadow_bitmap_schedule_state(state).cursor == cursor);
    shadow_pacer_observe(&pacer, 1400, TRUE, 0, FALSE, TRUE);
    CHECK(shadow_pacer_can_submit(&pacer, 128));
    const double credit = pacer.credit;
    /* The transport callback can fail after admission. Nothing commits yet. */
    CHECK(shadow_bitmap_next(state, FALSE, &first) && first.index == retry.index);
    CHECK(shadow_bitmap_schedule_state(state).cursor == cursor);
    CHECK(shadow_bitmap_pending_tiles(state) == pending && pacer.credit == credit);
    CHECK(shadow_pacer_can_submit(&pacer, 128));
    CHECK(apply_tile(state, client, width, bpp, &retry));
    shadow_publication_commit(state, &pacer, &retry, 128);
    CHECK(pacer.credit == credit - 128);
    CHECK(shadow_bitmap_pending_tiles(state) == pending - 1);
    CHECK(shadow_bitmap_schedule_state(state).cursor != cursor);
    CHECK(drain(state, client, width, bpp, FALSE, NULL) == 63);
    CHECK(!shadow_bitmap_schedule_state(state).active);
    CHECK(memcmp(client, surface->data, (size_t)width * height * bpp) == 0);

    region16_uninit(&refresh);
    shadow_bitmap_free(control);
    shadow_bitmap_free(state);
    shadow_surface_free(surface);
    free(client);
    return 0;
}

static int upper_motion_does_not_starve_lower(void)
{
    const UINT32 width = 512, height = 512;
    rdpShadowSurface* surface = shadow_surface_new(NULL, 0, 0, width, height);
    shadowBitmapState* state = shadow_bitmap_new(width, height, 32, 0x3f0000);
    BYTE* client = calloc((size_t)width * height, 4);
    shadowPacer pacer;
    shadowPublicationResult result = { 0 };
    REGION16 refresh;
    region16_init(&refresh);
    CHECK(surface && state && client && shadow_pacer_set_fixed_rate_kib(&pacer, 150));
    surface->format = PIXEL_FORMAT_BGRX32;
    surface->scanline = width * 4;
    shadow_bitmap_enable_coverage(state, TRUE);
    paint(surface, 32, 1);
    const RECTANGLE_16 full = { 0, 0, 512, 512 };
    const RECTANGLE_16 top = { 0, 0, 1, 1 };
    CHECK(region16_union_rect(&surface->invalidRegion, &surface->invalidRegion, &full));
    CHECK(shadow_publication_stage(state, &pacer, surface, &refresh, 1000, &result));
    region16_clear(&surface->invalidRegion);
    BOOL reachedLower = FALSE;
    for (UINT32 n = 0; n < 32; n++)
    {
        surface->data[0] = (BYTE)(100 + n);
        CHECK(region16_union_rect(&surface->invalidRegion, &surface->invalidRegion, &top));
        CHECK(shadow_publication_stage(state, &pacer, surface, &refresh, 1001 + n, &result));
        CHECK(shadow_bitmap_schedule_state(state).active);
        region16_clear(&surface->invalidRegion);
        shadowBitmapTile tile = { 0 };
        CHECK(shadow_bitmap_next(state, FALSE, &tile));
        reachedLower |= tile.y >= 384;
        CHECK(deliver(state, client, width, 4, &tile));
    }
    CHECK(reachedLower);
    CHECK(drain(state, client, width, 4, FALSE, NULL) > 0);
    CHECK(memcmp(client, surface->data, (size_t)width * height * 4) == 0);
    region16_uninit(&refresh);
    shadow_bitmap_free(state);
    shadow_surface_free(surface);
    free(client);
    return 0;
}

static int overlapping_copy_keeps_dependency_order(void)
{
    const UINT32 width = 256, height = 256, stride = width * 4;
    rdpShadowSurface* surface = shadow_surface_new(NULL, 0, 0, width, height);
    shadowBitmapState* state = shadow_bitmap_new(width, height, 32, 0x3f0000);
    BYTE* client = calloc((size_t)width * height, 4);
    shadowPacer pacer;
    shadowPublicationResult result = { 0 };
    REGION16 refresh;
    region16_init(&refresh);
    CHECK(surface && state && client && shadow_pacer_set_fixed_rate_kib(&pacer, 150));
    surface->format = PIXEL_FORMAT_BGRX32;
    surface->scanline = stride;
    shadow_bitmap_enable_coverage(state, TRUE);
    paint(surface, 32, 7);
    CHECK(shadow_publication_stage(state, &pacer, surface, &refresh, 1000, &result));
    CHECK(drain(state, client, width, 4, TRUE, NULL) > 0);
    memmove(surface->data + 13U * stride, surface->data, (height - 13U) * stride);
    memset(surface->data, 0xa7, 13U * stride);
    const RECTANGLE_16 full = { 0, 0, 256, 256 };
    CHECK(region16_union_rect(&surface->invalidRegion, &surface->invalidRegion, &full));
    CHECK(shadow_publication_stage(state, &pacer, surface, &refresh, 1200, &result));
    CHECK(shadow_bitmap_schedule_state(state).active);
    shadowBitmapTile first = { 0 };
    CHECK(shadow_bitmap_next(state, TRUE, &first));
    CHECK(shadow_bitmap_schedule_state(state).moveFallback);
    UINT32 copies = 0;
    CHECK(drain(state, client, width, 4, TRUE, &copies) > 0);
    CHECK(copies > 0);
    CHECK(memcmp(client, surface->data, (size_t)width * height * 4) == 0);
    region16_uninit(&refresh);
    shadow_bitmap_free(state);
    shadow_surface_free(surface);
    free(client);
    return 0;
}

int main(void)
{
    return broad_publication_spreads_early_rows() ||
           control_replacement_and_convergence(16, PIXEL_FORMAT_RGB16) ||
           control_replacement_and_convergence(32, PIXEL_FORMAT_BGRX32) ||
           upper_motion_does_not_starve_lower() ||
           overlapping_copy_keeps_dependency_order();
}
