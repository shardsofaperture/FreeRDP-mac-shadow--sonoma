/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 *
 * Copyright 2011-2014 Marc-Andre Moreau <marcandre.moreau@gmail.com>
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

#ifndef FREERDP_SERVER_SHADOW_MAC_SHADOW_H
#define FREERDP_SERVER_SHADOW_MAC_SHADOW_H

#include <freerdp/server/shadow.h>

typedef struct mac_shadow_subsystem macShadowSubsystem;

#include <winpr/crt.h>
#include <winpr/synch.h>
#include <winpr/thread.h>
#include <winpr/stream.h>
#include <winpr/collections.h>

#include <stdatomic.h>

#include <dispatch/dispatch.h>
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurface.h>
#include <CoreVideo/CoreVideo.h>
#include <CoreGraphics/CoreGraphics.h>

#include "mac_shadow_audio.h"

struct mac_shadow_subsystem
{
	rdpShadowSubsystem common;

	int width;
	int height;
	BOOL retina;
	int pixelWidth;
	int pixelHeight;
	int desktopWidth;
	int desktopHeight;
	BOOL scaledClientSurface;
	int scaledClientWidth;
	int scaledClientHeight;
	BOOL mouseDownLeft;
	BOOL mouseDownRight;
	BOOL mouseDownOther;
	CGEventSourceRef eventSource;
	CGEventFlags keyboardFlags;
	CRITICAL_SECTION connectionLock;
	size_t connectedClients;
	BOOL autoClientProfile;
	BOOL configuredShowMouseCursor;
	BOOL connectionDisplayModeActive;
	CGDisplayModeRef preConnectionDisplayMode;
	BOOL privateDisplayModeActive;
	int preConnectionPrivateDisplayMode;
	const char* connectDisplayCommand;
	const char* disconnectDisplayCommand;
	BOOL captureRunning;
	BOOL captureNeedsFullFrame;
	CGDisplayStreamRef stream;
	dispatch_queue_t captureQueue;
	/* Capture owns a mutable latest surface; the worker publishes into server->surface. */
	rdpShadowSurface* captureSurface;
	CRITICAL_SECTION publicationLock;
	HANDLE worker;
	HANDLE stopEvent;
	HANDLE frameEvent;
	BOOL publishedFrame;
	/* Guarded by captureSurface->lock, with publicationLock held while the
	 * worker reads or releases the surface. Ordinary capture damage has one
	 * first-event deadline; later frames never extend it. */
	UINT32 aggregationMs;
	UINT64 captureFirstPendingMs;
	UINT64 windowCaptureEvents;
	UINT64 periodCaptureEvents;
	UINT64 periodPublications;
	UINT64 periodCoalescedEvents;
	UINT64 periodSuppressedEvents;
	UINT64 periodWindowArea;
	UINT64 periodBypassPublications;
	UINT64 publicationId;
	UINT64 lastPublicationMs;
	UINT64 periodCadenceSumMs;
	UINT64 periodCadenceCount;
	UINT64 periodCadenceMinMs;
	UINT64 periodCadenceMaxMs;
	UINT64 lastWindowArea;
	UINT64 lastWindowEvents;
	UINT64 aggregationReportMs;
	BOOL audioNegotiated;
	BOOL audioUnavailable;
	_Atomic UINT32 audioGeneration;
	dispatch_queue_t audioStartupQueue;
	MacShadowAudioCapture* audioCapture;
};

#ifdef __cplusplus
extern "C"
{
#endif

#ifdef __cplusplus
}
#endif

#endif /* FREERDP_SERVER_SHADOW_MAC_SHADOW_H */
