/** FreeRDP shadow regression tests. Licensed under the Apache License, Version 2.0. */
#include <freerdp/config.h>
#include <freerdp/codec/color.h>
#include <freerdp/codec/interleaved.h>
#include <freerdp/codec/planar.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../shadow_bitmap.h"

#define CHECK(condition)                                                    \
	do                                                                      \
	{                                                                       \
		if (!(condition))                                                   \
		{                                                                   \
			fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); \
			goto out;                                                       \
		}                                                                   \
	} while (0)

/* Apply the actual copy or compressed bitmap to an independent client framebuffer. */
static BOOL deliver(shadowBitmapState* state, UINT16* client, UINT32 width,
                    const shadowBitmapTile* tile)
{
	BOOL result = FALSE;
	BYTE compressed[64 * 64 * 4];
	UINT16 decoded[64 * 64] = { 0 };
	UINT32 length = sizeof(compressed);
	UINT32 stride = 0;
	const BYTE* latest = shadow_bitmap_pixels(state, &stride);
	const UINT32 encodedWidth = (tile->width + 3U) & ~3U;
	BITMAP_INTERLEAVED_CONTEXT* codec = bitmap_interleaved_context_new(TRUE);
	if (!codec)
		return FALSE;
	if (tile->copy)
	{
		for (UINT32 y = 0; y < tile->height; y++)
			memcpy(&decoded[y * 64U], &client[(tile->sourceY + y) * width + tile->sourceX],
			       tile->width * 2U);
	}
	else
	{
		BYTE packed[64 * 64 * 2] = { 0 };
		for (UINT32 row = 0; row < tile->height; row++)
			memcpy(packed + row * 128U, latest + (size_t)(tile->y + row) * stride + tile->x * 2U,
			       tile->width * 2U);
		CHECK(interleaved_compress(codec, compressed, &length, encodedWidth, tile->height, packed,
		                           PIXEL_FORMAT_RGB16, 128, 0, 0, nullptr, 16));
		CHECK(interleaved_decompress(codec, compressed, length, encodedWidth, tile->height, 16,
		                             (BYTE*)decoded, PIXEL_FORMAT_RGB16, 128, 0, 0, encodedWidth,
		                             tile->height, nullptr));
	}
	for (UINT32 y = 0; y < tile->height; y++)
	{
		CHECK(memcmp(&decoded[y * 64U], latest + (size_t)(tile->y + y) * stride + tile->x * 2U,
		             tile->width * 2U) == 0);
		memcpy(&client[(tile->y + y) * width + tile->x], &decoded[y * 64U], tile->width * 2U);
	}
	shadow_bitmap_commit(state, tile);
	result = TRUE;
out:
	bitmap_interleaved_context_free(codec);
	return result;
}

static int drain(shadowBitmapState* state, UINT16* client, UINT32 width, BOOL copies,
                 UINT32* copyCount)
{
	shadowBitmapTile tile = { 0 };
	int count = 0;
	while (shadow_bitmap_next(state, copies, &tile))
	{
		if (++count > 4096 || !deliver(state, client, width, &tile))
			return -1;
		if (copyCount && tile.copy)
			(*copyCount)++;
	}
	return count;
}

static BOOL test_sparse_and_latest(void)
{
	BOOL result = FALSE;
	const UINT32 width = 256, height = 192;
	UINT16* frame = calloc(width * height, 2);
	UINT16* client = calloc(width * height, 2);
	shadowBitmapState* state = shadow_bitmap_new(width, height, 16, 0x3f0000);
	shadowBitmapTile tile = { 0 }, retry = { 0 };
	CHECK(frame && client && state);
	CHECK(shadow_bitmap_stage(state, (BYTE*)frame, PIXEL_FORMAT_RGB16, width * 2, nullptr));
	CHECK(drain(state, client, width, FALSE, nullptr) == 12);
	frame[0] = 0x1234;
	frame[width * height - 1] = 0x5678;
	CHECK(shadow_bitmap_stage(state, (BYTE*)frame, PIXEL_FORMAT_RGB16, width * 2, nullptr));
	CHECK(shadow_bitmap_next(state, FALSE, &tile));
	/* An unsubmitted/failed write must not advance the cache. */
	CHECK(shadow_bitmap_next(state, FALSE, &retry) && retry.index == tile.index);
	CHECK(deliver(state, client, width, &tile));
	for (UINT32 generation = 0; generation < 200; generation++)
	{
		frame[width * height - 1] = (UINT16)generation;
		CHECK(shadow_bitmap_stage(state, (BYTE*)frame, PIXEL_FORMAT_RGB16, width * 2, nullptr));
	}
	CHECK(drain(state, client, width, FALSE, nullptr) == 1);
	CHECK(memcmp(frame, client, width * height * 2U) == 0);
	/* A transient change superseded before sending should produce no update. */
	frame[100] = 1;
	CHECK(shadow_bitmap_stage(state, (BYTE*)frame, PIXEL_FORMAT_RGB16, width * 2, nullptr));
	frame[100] = 0;
	CHECK(shadow_bitmap_stage(state, (BYTE*)frame, PIXEL_FORMAT_RGB16, width * 2, nullptr));
	CHECK(!shadow_bitmap_pending(state));
	result = TRUE;
out:
	shadow_bitmap_free(state);
	free(frame);
	free(client);
	return result;
}

static BOOL test_edges_refresh_and_quantization(void)
{
	BOOL result = FALSE;
	const UINT32 width = 130, height = 67;
	UINT16* frame = calloc(width * height, 2);
	UINT16* client = calloc(width * height, 2);
	BYTE* rgb = calloc(width * height, 4);
	shadowBitmapState* state = shadow_bitmap_new(width, height, 16, 0x3f0000);
	REGION16 refresh;
	region16_init(&refresh);
	const RECTANGLE_16 corner = { 129, 66, 130, 67 };
	CHECK(frame && client && state && rgb);
	for (UINT32 n = 0; n < width * height; n++)
		frame[n] = (UINT16)(n * 31U);
	CHECK(shadow_bitmap_stage(state, (BYTE*)frame, PIXEL_FORMAT_RGB16, width * 2, nullptr));
	CHECK(drain(state, client, width, TRUE, nullptr) == 6);
	CHECK(memcmp(frame, client, width * height * 2U) == 0);
	CHECK(region16_union_rect(&refresh, &refresh, &corner));
	client[width * height - 1] ^= 0xffff;
	CHECK(shadow_bitmap_stage(state, (BYTE*)frame, PIXEL_FORMAT_RGB16, width * 2, &refresh));
	CHECK(drain(state, client, width, TRUE, nullptr) == 1);
	CHECK(memcmp(frame, client, width * height * 2U) == 0);
	CHECK(shadow_bitmap_stage(state, rgb, PIXEL_FORMAT_BGRX32, width * 4, nullptr));
	CHECK(drain(state, client, width, FALSE, nullptr) > 0);
	for (UINT32 n = 0; n < width * height; n++)
	{
		rgb[n * 4] = 1;
		rgb[n * 4 + 1] = 1;
		rgb[n * 4 + 2] = 1;
		rgb[n * 4 + 3] = 255;
	}
	CHECK(shadow_bitmap_stage(state, rgb, PIXEL_FORMAT_BGRX32, width * 4, nullptr));
	CHECK(!shadow_bitmap_pending(state));
	CHECK(!shadow_bitmap_size_matches(state, height, width, 16, 0x3f0000));
	CHECK(!shadow_bitmap_size_matches(state, width, height, 32, 0x3f0000));
	CHECK(!shadow_bitmap_size_matches(state, width, height, 16, 0x3f80));
	CHECK(!shadow_bitmap_new(0, 100, 16, 0x3f0000));
	CHECK(!shadow_bitmap_new(65535, 65535, 16, 0x3f0000));
	result = TRUE;
out:
	region16_uninit(&refresh);
	shadow_bitmap_free(state);
	free(frame);
	free(client);
	free(rgb);
	return result;
}

static BOOL test_movement(void)
{
	BOOL result = FALSE;
	const UINT32 width = 256, height = 192;
	UINT16* frame = calloc(width * height, 2);
	UINT16* previous = calloc(width * height, 2);
	UINT16* client = calloc(width * height, 2);
	shadowBitmapState* state = shadow_bitmap_new(width, height, 16, 0x3f0000);
	const INT32 moves[][2] = { { 0, 13 }, { 0, -16 }, { 7, 11 }, { -9, -5 } };
	UINT32 copies = 0;
	UINT32 random = 12345;
	CHECK(frame && previous && client && state);
	for (UINT32 n = 0; n < width * height; n++)
	{
		random = random * 1664525U + 1013904223U;
		frame[n] = (UINT16)(random >> 16);
	}
	CHECK(shadow_bitmap_stage(state, (BYTE*)frame, PIXEL_FORMAT_RGB16, width * 2, nullptr));
	CHECK(drain(state, client, width, TRUE, &copies) == 12);
	CHECK(copies == 0); /* No copy may reference a framebuffer not yet established. */
	for (size_t move = 0; move < ARRAYSIZE(moves); move++)
	{
		memcpy(previous, frame, width * height * 2U);
		for (UINT32 y = 0; y < height; y++)
		{
			for (UINT32 x = 0; x < width; x++)
			{
				const INT32 sx = (INT32)x + moves[move][0];
				const INT32 sy = (INT32)y + moves[move][1];
				frame[y * width + x] =
				    ((sx >= 0) && (sy >= 0) && (sx < (INT32)width) && (sy < (INT32)height))
				        ? previous[(UINT32)sy * width + (UINT32)sx]
				        : 0;
			}
		}
		copies = 0;
		CHECK(shadow_bitmap_stage(state, (BYTE*)frame, PIXEL_FORMAT_RGB16, width * 2, nullptr));
		UINT64 bitmapPixels = 0;
		UINT32 operations = 0;
		shadowBitmapTile tile = { 0 };
		while (shadow_bitmap_next(state, TRUE, &tile))
		{
			CHECK(++operations < 100);
			if (tile.copy)
				copies++;
			else
				bitmapPixels += tile.width * tile.height;
			CHECK(deliver(state, client, width, &tile));
		}
		CHECK(operations > 0 && copies > 0);
		CHECK(shadow_bitmap_last_copy_candidate_probes(state) <=
		      SHADOW_BITMAP_COPY_CANDIDATE_LIMIT);
		if (move == 0)
		{
			CHECK(bitmapPixels <= width * 13U);
			printf("13-pixel scroll: %u copy orders, %llu bitmap pixels (full frame: %u)\n",
			       copies, (unsigned long long)bitmapPixels, width * height);
		}
		CHECK(memcmp(frame, client, width * height * 2U) == 0);
	}
	/* Unrelated changed pixels must fall back to lossless bitmap updates. */
	for (UINT32 n = 0; n < width * height; n++)
		frame[n] ^= 0x5a5a;
	CHECK(shadow_bitmap_stage(state, (BYTE*)frame, PIXEL_FORMAT_RGB16, width * 2, nullptr));
	CHECK(drain(state, client, width, TRUE, nullptr) > 0);
	CHECK(memcmp(frame, client, width * height * 2U) == 0);
	result = TRUE;
out:
	shadow_bitmap_free(state);
	free(frame);
	free(previous);
	free(client);
	return result;
}

static BOOL test_partial_replacement(void)
{
	BOOL result = FALSE;
	const UINT32 width = 130, height = 131;
	UINT16* frame = calloc(width * height, 2);
	UINT16* client = calloc(width * height, 2);
	shadowBitmapState* state = shadow_bitmap_new(width, height, 16, 0x3f0000);
	CHECK(frame && client && state);
	for (UINT32 generation = 1; generation < 100; generation++)
	{
		/* Simulate a producer replacing pending content faster than a stalled client drains it. */
		for (UINT32 y = 0; y < height; y++)
		{
			for (UINT32 x = 0; x < width; x++)
				frame[y * width + x] = (UINT16)((x + y + generation) % 19U);
		}
		CHECK(shadow_bitmap_stage(state, (BYTE*)frame, PIXEL_FORMAT_RGB16, width * 2U, nullptr));
		shadowBitmapTile tile = { 0 };
		if (shadow_bitmap_next(state, TRUE, &tile))
			CHECK(deliver(state, client, width, &tile));
	}
	CHECK(drain(state, client, width, TRUE, nullptr) > 0);
	CHECK(memcmp(frame, client, width * height * 2U) == 0);
	result = TRUE;
out:
	shadow_bitmap_free(state);
	free(frame);
	free(client);
	return result;
}

static BOOL test_resolution_matrix(void)
{
	const UINT32 sizes[][2] = { {800,600}, {1024,768}, {1280,720}, {1280,800},
	    {1440,900}, {1680,1050}, {1920,1080}, {2560,1600}, {2880,1800}, {801,601} };
	for (size_t n = 0; n < ARRAYSIZE(sizes); n++)
	{
		BOOL result = FALSE;
		const UINT32 width = sizes[n][0], height = sizes[n][1];
		UINT16* frame = calloc((size_t)width * height, 2);
		UINT16* client = calloc((size_t)width * height, 2);
		shadowBitmapState* state = shadow_bitmap_new(width, height, 16, 0x3f0000);
		CHECK(frame && client && state);
		for (UINT32 i = 0; i < width * height; i++)
			frame[i] = (UINT16)(i * 31U);
		CHECK(shadow_bitmap_stage(state, (BYTE*)frame, PIXEL_FORMAT_RGB16, width * 2U, nullptr));
		CHECK(drain(state, client, width, FALSE, nullptr) > 0);
		CHECK(memcmp(frame, client, (size_t)width * height * 2U) == 0);
		frame[width * height - 1] ^= 0xffff;
		CHECK(shadow_bitmap_stage(state, (BYTE*)frame, PIXEL_FORMAT_RGB16, width * 2U, nullptr));
		CHECK(drain(state, client, width, FALSE, nullptr) == 1);
		CHECK(memcmp(frame, client, (size_t)width * height * 2U) == 0);
		result = TRUE;
	out:
		shadow_bitmap_free(state);
		free(frame);
		free(client);
		if (!result)
			return FALSE;
	}
	return TRUE;
}

static BOOL test_color_capabilities(void)
{
	BOOL result = FALSE;
	rdpSettings* settings = freerdp_settings_new(0);
	CHECK(settings);
	CHECK(freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, FALSE));
	CHECK(freerdp_settings_set_uint32(settings, FreeRDP_MultifragMaxRequestSize, 16384));
	const UINT32 depths[] = { 8, 15, 16, 24, 32 };
	for (size_t n = 0; n < ARRAYSIZE(depths); n++)
	{
		CHECK(shadow_bitmap_color_depth(depths[n]) ==
		      ((depths[n] == 8 || depths[n] == 24) ? 16U : depths[n]));
		CHECK(freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, depths[n]));
		CHECK(shadow_bitmap_supported(settings, FALSE) ==
		      ((depths[n] == 16) || (depths[n] == 32)));
		CHECK(!shadow_bitmap_supported(settings, TRUE));
	}
	CHECK(freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32));
	CHECK(freerdp_settings_set_uint32(settings, FreeRDP_MultifragMaxRequestSize, 16414));
	CHECK(shadow_bitmap_supported(settings, FALSE));
	CHECK(freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 16));
	CHECK(freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, TRUE));
	CHECK(!shadow_bitmap_supported(settings, FALSE));
	CHECK(freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, FALSE));
	CHECK(freerdp_settings_set_uint32(settings, FreeRDP_MultifragMaxRequestSize, 256));
	CHECK(!shadow_bitmap_supported(settings, FALSE));
	UINT32 tileWidth = 0, tileHeight = 0;
	CHECK(shadow_bitmap_tile_size(32, 0x3f80, &tileWidth, &tileHeight));
	CHECK(tileWidth == 64 && tileHeight == 62);
	CHECK(30U + (tileWidth * tileHeight * 4U) + 128U <= 0x3f80);
	CHECK(shadow_bitmap_tile_size(16, 4096, &tileWidth, &tileHeight));
	CHECK(30U + (tileWidth * tileHeight * 3U) + 128U <= 4096);
	CHECK(!shadow_bitmap_tile_size(32, 256, &tileWidth, &tileHeight));
	result = TRUE;
out:
	freerdp_settings_free(settings);
	return result;
}

static BOOL test_32bit_newest_fullscreen(void)
{
	BOOL result = FALSE;
	const UINT32 width = 1920, height = 1080;
	UINT32 tileWidth = 0, tileHeight = 0;
	UINT32* frame = nullptr;
	shadowBitmapState* state = nullptr;
	CHECK(shadow_bitmap_tile_size(32, 0x3f80, &tileWidth, &tileHeight));
	const UINT32 tileCount = ((width + tileWidth - 1U) / tileWidth) *
	                         ((height + tileHeight - 1U) / tileHeight);
	frame = calloc((size_t)width * height, sizeof(UINT32));
	state = shadow_bitmap_new(width, height, 32, 0x3f80);
	CHECK(frame && state);
	for (UINT32 generation = 1; generation <= 32; generation++)
	{
		const UINT32 pixel = 0xff000000U | generation | (generation << 8U) |
		                     (generation << 16U);
		for (size_t index = 0; index < (size_t)width * height; index++)
			frame[index] = pixel;
		CHECK(shadow_bitmap_stage(state, (BYTE*)frame, PIXEL_FORMAT_BGRX32, width * 4U,
		                          nullptr));
		/* Model a saturated sender making only occasional progress while every captured
		 * full-screen image supersedes the previous pending generation. */
		if ((generation % 4U) == 0)
		{
			shadowBitmapTile tile = { 0 };
			if (shadow_bitmap_next(state, FALSE, &tile))
				shadow_bitmap_commit(state, &tile);
		}
	}
	UINT32 operations = 0;
	shadowBitmapTile tile = { 0 };
	while (shadow_bitmap_next(state, FALSE, &tile))
	{
		shadow_bitmap_commit(state, &tile);
		CHECK(++operations <= tileCount);
	}
	CHECK(shadow_bitmap_stage(state, (BYTE*)frame, PIXEL_FORMAT_BGRX32, width * 4U, nullptr));
	CHECK(!shadow_bitmap_pending(state));
	result = TRUE;
out:
	shadow_bitmap_free(state);
	free(frame);
	return result;
}

static BOOL test_padded_color_edges(void)
{
	BOOL result = FALSE;
	BYTE pixels[5 * 3 * 4], packed[64 * 64 * 4], compressed[64 * 64 * 4];
	BYTE decoded[64 * 64 * 4];
	BITMAP_PLANAR_CONTEXT* planar = freerdp_bitmap_planar_context_new(0, 64, 64);
	BITMAP_INTERLEAVED_CONTEXT* codec = bitmap_interleaved_context_new(TRUE);
	CHECK(codec && planar);
	memset(pixels, 0x78, sizeof(pixels));
	CHECK(shadow_bitmap_pack(packed, pixels, 20, 4, 2, 1, 1));
	CHECK(memcmp(packed, pixels + sizeof(pixels) - 4, 4) == 0);
	for (size_t n = 4; n < sizeof(packed); n++)
		CHECK(packed[n] == 0);
	CHECK(!shadow_bitmap_pack(packed, pixels, 20, 4, 2, 2, 1));
	const UINT32 depths[] = {15, 16, 24};
	for (size_t n = 0; n < ARRAYSIZE(depths); n++)
	{
		UINT32 length = sizeof(compressed);
		CHECK(interleaved_compress(codec, compressed, &length, 4, 1, packed,
		                           PIXEL_FORMAT_BGRX32, 256, 0, 0, nullptr, depths[n]));
		CHECK(interleaved_decompress(codec, compressed, length, 4, 1, depths[n], decoded,
		                             PIXEL_FORMAT_BGRX32, 256, 0, 0, 4, 1, nullptr));
		/* Quantization varies with depth, but the actual edge pixel must survive. */
		CHECK(decoded[0] >= 0x70 && decoded[0] <= 0x7f);
		CHECK(decoded[4] == 0 && decoded[5] == 0 && decoded[6] == 0);
	}
	UINT32 length = sizeof(compressed);
	CHECK(freerdp_bitmap_compress_planar(planar, packed, PIXEL_FORMAT_BGRX32, 4, 1, 256,
	                                     compressed, &length));
	CHECK(freerdp_bitmap_decompress_planar(planar, compressed, length, 4, 1, decoded,
	                                       PIXEL_FORMAT_BGRX32, 256, 0, 0, 4, 1, FALSE));
	CHECK(memcmp(decoded, pixels, 3) == 0);
	CHECK(decoded[4] == 0 && decoded[5] == 0 && decoded[6] == 0);
	result = TRUE;
out:
	freerdp_bitmap_planar_context_free(planar);
	bitmap_interleaved_context_free(codec);
	return result;
}

int main(void)
{
	if (!test_sparse_and_latest() || !test_edges_refresh_and_quantization() || !test_movement() ||
	    !test_partial_replacement() || !test_resolution_matrix() || !test_color_capabilities() ||
	    !test_32bit_newest_fullscreen() || !test_padded_color_edges())
		return 1;
	puts(
	    "Sparse/32-bit newest state, refresh, edge bitmap round trips, and ScrBlt replay passed");
	return 0;
}
