/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 *
 * Copyright 2014 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FREERDP_SERVER_SHADOW_ENCODER_H
#define FREERDP_SERVER_SHADOW_ENCODER_H

#include <winpr/crt.h>
#include <winpr/stream.h>

#include <freerdp/freerdp.h>
#include <freerdp/codecs.h>

#include <freerdp/server/shadow.h>
#include "shadow_bitmap.h"
#if defined(__APPLE__)
#include "shadow_pacer.h"
#endif

struct rdp_shadow_encoder
{
	rdpShadowClient* client;
	rdpShadowServer* server;

	UINT32 width;
	UINT32 height;
	UINT32 codecs;

	BYTE** grid;
	UINT32 gridWidth;
	UINT32 gridHeight;
	BYTE* gridBuffer;
	UINT32 maxTileWidth;
	UINT32 maxTileHeight;

	wStream* bs;

	RFX_CONTEXT* rfx;
	NSC_CONTEXT* nsc;
	BITMAP_PLANAR_CONTEXT* planar;
	BITMAP_INTERLEAVED_CONTEXT* interleaved;
	H264_CONTEXT* h264;
	PROGRESSIVE_CONTEXT* progressive;
#if defined(WITH_GFX_AV1)
	FREERDP_AV1_CONTEXT* av1;
#endif
	UINT32 fps;
	UINT32 maxFps;
	BOOL frameAck;
	UINT32 frameId;
	UINT32 lastAckframeId;
	UINT32 queueDepth;
	BOOL bitmapFallback;
	shadowBitmapState* bitmapState;
#if defined(__APPLE__)
	shadowPacer bitmapPacer;
	int bitmapPacerSocketFd; /* Owned duplicate of the transport socket. */
	UINT32 bitmapSocketCapRequest;
	UINT32 bitmapSocketCapEffective;
	BOOL bitmapPacerDiagnostics;
	BOOL bitmapCoverageEnabled;
	UINT64 bitmapCoveragePublicationId;
	UINT64 bitmapCoverageStartedMs;
	UINT32 bitmapCoverageEarlyCount;
	UINT32 bitmapCoverageEarlyMinY;
	UINT32 bitmapCoverageEarlyMaxY;
	UINT32 bitmapCoverageEarlyLastY;
	UINT32 bitmapCoverageEarlySamples[4];
	BOOL bitmapCoverageEarlyReported;
	UINT64 bitmapPacerReportMs;
	UINT64 bitmapPacerBytes;
	UINT64 bitmapPacerOps;
	UINT64 bitmapPacerPauses;
	UINT64 bitmapPacerBurstEntries;
	UINT64 bitmapPacerReportedStopMs;
	UINT64 bitmapPacerQualifiedActive;
	UINT64 bitmapPacerQualifiedCooldown;
	UINT64 bitmapPacerQualifiedPressure;
	UINT64 bitmapPacerQualifiedSocket;
	UINT64 bitmapPacerQualifiedMotion;
	UINT64 bitmapPacerLastFreshPixels;
	UINT64 bitmapPacerLastDesktopPixels;
	UINT64 bitmapPacerBurstBytes;
	UINT64 bitmapPacerBurstDeferrals;
	UINT64 bitmapPacerDamagePixels;
	UINT64 bitmapPacerDamageTotalPixels;
	UINT64 bitmapPacerBlockedMs;
	UINT64 bitmapPacerBlockedSinceMs;
	UINT32 bitmapPacerMaxQueued;
	UINT64 bitmapPacerPublications;
	UINT64 bitmapPacerPublicationId;
	UINT64 bitmapPacerLifetimeBurstEntries;
	UINT64 bitmapPacerLastRefreshPixels;
	UINT32 bitmapPacerLastDecision;
	UINT64 bitmapPacerEstimatedBytes;
	UINT64 bitmapPacerAdmittedBytes;
	UINT64 bitmapPacerPayloadBytes;
	UINT64 bitmapPacerChargeFallbacks;
	UINT64 bitmapPacerFirstSubmitMs;
	UINT64 bitmapPacerLastSubmitMs;
	UINT32 bitmapPacerMinTileY;
	UINT32 bitmapPacerMaxTileY;
#endif
};

#ifdef __cplusplus
extern "C"
{
#endif

	WINPR_ATTR_NODISCARD int shadow_encoder_reset(rdpShadowEncoder* encoder);
	WINPR_ATTR_NODISCARD int shadow_encoder_prepare(rdpShadowEncoder* encoder, UINT32 codecs);
	WINPR_ATTR_NODISCARD UINT32 shadow_encoder_create_frame_id(rdpShadowEncoder* encoder);

	void shadow_encoder_free(rdpShadowEncoder* encoder);

	WINPR_ATTR_MALLOC(shadow_encoder_free, 1)
	WINPR_ATTR_NODISCARD
	rdpShadowEncoder* shadow_encoder_new(rdpShadowClient* client);

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_SERVER_SHADOW_ENCODER_H */
