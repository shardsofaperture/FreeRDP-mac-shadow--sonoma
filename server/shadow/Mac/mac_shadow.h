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
	BOOL testToneEnabled;
	double testTonePhase;
	dispatch_queue_t audioQueue;
	dispatch_source_t audioTimer;
	BOOL audioNegotiated;
	BOOL audioUnavailable;
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
