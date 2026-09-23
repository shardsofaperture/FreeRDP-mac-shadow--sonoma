/* Deterministic server-side publication/codec/pacer workload. Apache-2.0. */
#include <freerdp/config.h>
#include <freerdp/codec/color.h>
#include <freerdp/codec/interleaved.h>
#include <freerdp/codec/planar.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../shadow_publication.h"
#include "../shadow_surface.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); return 1; } } while (0)

typedef struct
{
    UINT64 bytes, firstMs, lastMs, completionMs;
    UINT32 ops, deferrals, maxPending, maxServiceIntervalMs;
    UINT32 earlyOps, earlyMinY, earlyMaxY;
    BOOL qualified, started;
} WorkResult;

static BOOL encode(BITMAP_PLANAR_CONTEXT* planar, BITMAP_INTERLEAVED_CONTEXT* interleaved,
                   const shadowBitmapState* state, const shadowBitmapTile* tile, UINT32 depth,
                   UINT32* wireSize)
{
    BYTE packed[64 * 64 * 4] = { 0 };
    BYTE output[64 * 64 * 4] = { 0 };
    UINT32 stride = 0, length = sizeof(output);
    const BYTE* pixels = shadow_bitmap_pixels(state, &stride);
    const UINT32 encodedWidth = (tile->width + 3U) & ~3U;
    if (depth == 32)
    {
        for (UINT32 y = 0; y < tile->height; y++)
            memcpy(packed + y * 256U,
                   pixels + (size_t)(tile->y + y) * stride + tile->x * 4U,
                   tile->width * 4U);
        if (!freerdp_bitmap_compress_planar(planar, packed, PIXEL_FORMAT_BGRX32,
                                           encodedWidth, tile->height, 256, output, &length))
            return FALSE;
    }
    else
    {
        for (UINT32 y = 0; y < tile->height; y++)
            memcpy(packed + y * 128U,
                   pixels + (size_t)(tile->y + y) * stride + tile->x * 2U,
                   tile->width * 2U);
        if (!interleaved_compress(interleaved, output, &length, encodedWidth,
                                  tile->height, packed, PIXEL_FORMAT_RGB16, 128,
                                  0, 0, nullptr, 16))
            return FALSE;
    }
    *wireSize = 30U + length; /* Bitmap update + data + compression headers. */
    return TRUE;
}

/* policy: 0=F, 1=RC3 burst, 2=N coverage with F pacing. */
static int run_workload(UINT32 width, UINT32 height, UINT32 depth, UINT32 policy,
                        WorkResult* out)
{
    const UINT32 bpp = depth / 8U, format = depth == 32 ? PIXEL_FORMAT_BGRX32 : PIXEL_FORMAT_RGB16;
    const size_t size = (size_t)width * height * bpp;
    rdpShadowSurface* surface = shadow_surface_new(NULL, 0, 0, width, height);
    BYTE* frame = surface ? surface->data : NULL;
    if (surface) { surface->format = format; surface->scanline = width * bpp; }
    BYTE* client = calloc(1, size);
    shadowBitmapState* state = shadow_bitmap_new(width, height, depth, 0x3f0000);
    BITMAP_PLANAR_CONTEXT* planar = freerdp_bitmap_planar_context_new(PLANAR_FORMAT_HEADER_RLE, 64, 64);
    BITMAP_INTERLEAVED_CONTEXT* interleaved = bitmap_interleaved_context_new(TRUE);
    shadowPacer pacer;
    REGION16 refresh;
    shadowPublicationResult staged = { 0 };
    UINT64 ms = 1000;
    region16_init(&refresh);
    CHECK(frame && client && state && planar && interleaved);
    REGION16* publication = &surface->invalidRegion;
    CHECK(shadow_pacer_set_fixed_rate_kib(&pacer, 150));
    if (policy == 1) CHECK(shadow_pacer_enable_large_refresh_diagnostic_m(&pacer));
    if (policy == 2) shadow_bitmap_enable_coverage(state, TRUE);
    CHECK(policy == 1 || (pacer.fixed && pacer.baseRate == 153600.0 &&
                          !pacer.largeRefreshBurstEnabled));
    /* Warm, correctly known client cache. Same deterministic surface for both runs. */
    CHECK(shadow_bitmap_stage(state, frame, format, width * bpp, NULL));
    while (shadow_bitmap_pending(state))
    {
        shadowBitmapTile tile = { 0 };
        CHECK(shadow_bitmap_next(state, FALSE, &tile));
        shadow_bitmap_commit(state, &tile);
    }
    const RECTANGLE_16 window = { (UINT16)(width / 8), (UINT16)(height / 8),
                                  (UINT16)(width * 7 / 8), (UINT16)(height * 7 / 8) };
    CHECK(region16_union_rect(publication, publication, &window));
    for (UINT32 y = window.top; y < window.bottom; y++)
        for (UINT32 x = window.left; x < window.right; x++)
        {
            if (depth == 32)
            {
                BYTE* p = frame + ((size_t)y * width + x) * 4U;
                p[0] = (BYTE)(x * 37U + y * 17U);
                p[1] = (BYTE)(x * 11U + y * 23U);
                p[2] = (BYTE)(x * 3U + y * 29U);
            }
            else
            {
                const UINT16 value = (UINT16)(((x + y) & 31U) |
                                      (((x * 3U + y) & 63U) << 5) |
                                      (((x + y * 7U) & 31U) << 11));
                memcpy(frame + ((size_t)y * width + x) * 2U, &value, 2);
            }
        }
    CHECK(shadow_publication_stage(state, &pacer, surface, &refresh, ms, &staged));
    out->qualified = staged.qualified;
    out->started = staged.burstStarted;
    CHECK(shadow_bitmap_schedule_state(state).active == (policy == 2));
    CHECK(staged.publicationArea >= (UINT64)width * height / 4U);
    CHECK(shadow_bitmap_pending(state));
    for (; ms < 30000 && shadow_bitmap_pending(state); ms++)
    {
        UINT32 cycleBytes = 0;
        shadow_pacer_observe(&pacer, ms, TRUE, 0, FALSE, TRUE);
        const UINT32 pending = shadow_bitmap_pending_tiles(state);
        if (pending > out->maxPending) out->maxPending = pending;
        for (UINT32 n = 0; n < 8 && cycleBytes < 16384U; n++)
        {
            shadowBitmapTile tile = { 0 };
            UINT32 wireSize = 0, stride = 0;
            if (!shadow_pacer_preflight(&pacer, 2048))
            {
                out->deferrals++;
                break;
            }
            CHECK(shadow_bitmap_next(state, FALSE, &tile));
            CHECK(encode(planar, interleaved, state, &tile, depth, &wireSize));
            CHECK(wireSize <= 0x3f0000U);
            if (cycleBytes && cycleBytes + wireSize > 16384U) break;
            if (!shadow_pacer_can_submit(&pacer, wireSize + 64U))
            {
                out->deferrals++;
                break;
            }
            const BYTE* latest = shadow_bitmap_pixels(state, &stride);
            for (UINT32 y = 0; y < tile.height; y++)
                memcpy(client + ((size_t)(tile.y + y) * width + tile.x) * bpp,
                       latest + (size_t)(tile.y + y) * stride + tile.x * bpp,
                       tile.width * bpp);
            shadow_publication_commit(state, &pacer, &tile, wireSize + 64U);
            if (ms <= 1200)
            {
                if (!out->earlyOps) out->earlyMinY = tile.y;
                out->earlyMinY = MIN(out->earlyMinY, tile.y);
                out->earlyMaxY = MAX(out->earlyMaxY, tile.y);
                out->earlyOps++;
            }
            if (!out->firstMs) out->firstMs = ms;
            out->lastMs = ms;
            out->ops++;
            out->bytes += wireSize + 64U; /* Mock output delta = reserved estimate. */
            cycleBytes += wireSize;
        }
        out->maxServiceIntervalMs = 1; /* Input/channel service mocked once per tick. */
    }
    out->completionMs = ms;
    CHECK(!shadow_bitmap_pending(state));
    CHECK(memcmp(frame, client, size) == 0);
    printf("%ux%u@%u %s: qualified=%u burst=%u bytes=%llu firstMs=%llu lastMs=%llu "
           "completeMs=%llu dirtyTiles=%u maxPending=%u deferrals=%u serviceTickMaxMs=%u "
           "first200Ops=%u first200MinY=%u first200MaxY=%u\n",
           width, height, depth, policy == 2 ? "N-coverage" :
                                 (policy == 1 ? "candidate" : "F-policy"),
           out->qualified, out->started, (unsigned long long)out->bytes,
           (unsigned long long)out->firstMs, (unsigned long long)out->lastMs,
           (unsigned long long)out->completionMs, out->ops, out->maxPending,
           out->deferrals, out->maxServiceIntervalMs, out->earlyOps,
           out->earlyMinY, out->earlyMaxY);
    region16_uninit(&refresh);
    freerdp_bitmap_planar_context_free(planar);
    bitmap_interleaved_context_free(interleaved);
    shadow_bitmap_free(state);
    free(client);
    shadow_surface_free(surface);
    return 0;
}

int main(void)
{
    WorkResult f32 = { 0 }, m32 = { 0 }, n32 = { 0 };
    WorkResult f16 = { 0 }, m16 = { 0 }, n16 = { 0 };
    CHECK(run_workload(1920, 1080, 32, 0, &f32) == 0);
    CHECK(run_workload(1920, 1080, 32, 1, &m32) == 0);
    CHECK(run_workload(1920, 1080, 32, 2, &n32) == 0);
    CHECK(run_workload(1024, 768, 16, 0, &f16) == 0);
    CHECK(run_workload(1024, 768, 16, 1, &m16) == 0);
    CHECK(run_workload(1024, 768, 16, 2, &n16) == 0);
    CHECK(!f32.started && m32.started && !f16.started && m16.started);
    CHECK(f32.bytes == m32.bytes && f16.bytes == m16.bytes);
    CHECK(f32.bytes == n32.bytes && f16.bytes == n16.bytes);
    CHECK(!n32.started && !n16.started);
    CHECK(n32.earlyMaxY - n32.earlyMinY > f32.earlyMaxY - f32.earlyMinY);
    CHECK(n16.earlyMaxY - n16.earlyMinY > f16.earlyMaxY - f16.earlyMinY);
    return 0;
}
