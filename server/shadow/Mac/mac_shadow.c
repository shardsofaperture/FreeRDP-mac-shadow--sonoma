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

#include <winpr/crt.h>
#include <winpr/synch.h>
#include <winpr/input.h>
#include <winpr/sysinfo.h>

#include <math.h>

#include <freerdp/server/server-common.h>
#include <freerdp/codec/color.h>
#include <freerdp/codec/region.h>
#include <freerdp/log.h>

#include "mac_shadow.h"

#define TAG SERVER_TAG("shadow.mac")

static macShadowSubsystem* g_Subsystem = nullptr;

static BOOL mac_shadow_input_synchronize_event(rdpShadowSubsystem* subsystem,
                                               rdpShadowClient* client, UINT32 flags)
{
	if (!subsystem || !client)
		return FALSE;

	return TRUE;
}

static BOOL mac_shadow_input_keyboard_event(rdpShadowSubsystem* subsystem, rdpShadowClient* client,
                                            UINT16 flags, UINT8 code)
{
	DWORD vkcode;
	DWORD keycode;
	BOOL extended;
	CGEventRef kbdEvent;
	CGEventSourceRef source;
	extended = (flags & KBD_FLAGS_EXTENDED) ? TRUE : FALSE;

	if (!subsystem || !client)
		return FALSE;

	if (extended)
		code |= KBDEXT;

	vkcode = GetVirtualKeyCodeFromVirtualScanCode(code, 4);

	if (extended)
		vkcode |= KBDEXT;

	keycode = GetKeycodeFromVirtualKeyCode(vkcode, WINPR_KEYCODE_TYPE_APPLE);

	source = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);

	if (flags & KBD_FLAGS_DOWN)
	{
		kbdEvent = CGEventCreateKeyboardEvent(source, (CGKeyCode)keycode, TRUE);
		CGEventPost(kCGHIDEventTap, kbdEvent);
		CFRelease(kbdEvent);
	}
	else if (flags & KBD_FLAGS_RELEASE)
	{
		kbdEvent = CGEventCreateKeyboardEvent(source, (CGKeyCode)keycode, FALSE);
		CGEventPost(kCGHIDEventTap, kbdEvent);
		CFRelease(kbdEvent);
	}

	CFRelease(source);
	return TRUE;
}

static BOOL mac_shadow_input_unicode_keyboard_event(rdpShadowSubsystem* subsystem,
                                                    rdpShadowClient* client, UINT16 flags,
                                                    UINT16 code)
{
	if (!subsystem || !client)
		return FALSE;

	return TRUE;
}

static BOOL mac_shadow_input_mouse_event(rdpShadowSubsystem* subsystem, rdpShadowClient* client,
                                         UINT16 flags, UINT16 x, UINT16 y)
{
	macShadowSubsystem* mac = (macShadowSubsystem*)subsystem;
	UINT32 scrollX = 0;
	UINT32 scrollY = 0;
	CGWheelCount wheelCount = 2;

	if (!subsystem || !client)
		return FALSE;

	if (flags & PTR_FLAGS_WHEEL)
	{
		scrollY = flags & WheelRotationMask;

		if (flags & PTR_FLAGS_WHEEL_NEGATIVE)
		{
			scrollY = -(flags & WheelRotationMask) / 392;
		}
		else
		{
			scrollY = (flags & WheelRotationMask) / 120;
		}

		CGEventSourceRef source = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
		CGEventRef scroll = CGEventCreateScrollWheelEvent(source, kCGScrollEventUnitLine,
		                                                  wheelCount, scrollY, scrollX);
		CGEventPost(kCGHIDEventTap, scroll);
		CFRelease(scroll);
		CFRelease(source);
	}
	else
	{
		CGEventSourceRef source = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
		CGEventType mouseType = kCGEventNull;
		CGMouseButton mouseButton = kCGMouseButtonLeft;

		if (flags & PTR_FLAGS_MOVE)
		{
			if (mac->mouseDownLeft)
				mouseType = kCGEventLeftMouseDragged;
			else if (mac->mouseDownRight)
				mouseType = kCGEventRightMouseDragged;
			else if (mac->mouseDownOther)
				mouseType = kCGEventOtherMouseDragged;
			else
				mouseType = kCGEventMouseMoved;

			CGEventRef move =
			    CGEventCreateMouseEvent(source, mouseType, CGPointMake(x, y), mouseButton);
			CGEventPost(kCGHIDEventTap, move);
			CFRelease(move);
		}

		if (flags & PTR_FLAGS_BUTTON1)
		{
			mouseButton = kCGMouseButtonLeft;

			if (flags & PTR_FLAGS_DOWN)
			{
				mouseType = kCGEventLeftMouseDown;
				mac->mouseDownLeft = TRUE;
			}
			else
			{
				mouseType = kCGEventLeftMouseUp;
				mac->mouseDownLeft = FALSE;
			}
		}
		else if (flags & PTR_FLAGS_BUTTON2)
		{
			mouseButton = kCGMouseButtonRight;

			if (flags & PTR_FLAGS_DOWN)
			{
				mouseType = kCGEventRightMouseDown;
				mac->mouseDownRight = TRUE;
			}
			else
			{
				mouseType = kCGEventRightMouseUp;
				mac->mouseDownRight = FALSE;
			}
		}
		else if (flags & PTR_FLAGS_BUTTON3)
		{
			mouseButton = kCGMouseButtonCenter;

			if (flags & PTR_FLAGS_DOWN)
			{
				mouseType = kCGEventOtherMouseDown;
				mac->mouseDownOther = TRUE;
			}
			else
			{
				mouseType = kCGEventOtherMouseUp;
				mac->mouseDownOther = FALSE;
			}
		}

		CGEventRef mouseEvent =
		    CGEventCreateMouseEvent(source, mouseType, CGPointMake(x, y), mouseButton);
		CGEventPost(kCGHIDEventTap, mouseEvent);
		CFRelease(mouseEvent);
		CFRelease(source);
	}

	return TRUE;
}

static BOOL mac_shadow_input_extended_mouse_event(rdpShadowSubsystem* subsystem,
                                                  rdpShadowClient* client, UINT16 flags, UINT16 x,
                                                  UINT16 y)
{
	if (!subsystem || !client)
		return FALSE;

	return TRUE;
}

static int mac_shadow_detect_monitors(macShadowSubsystem* subsystem)
{
	size_t wide, high;
	MONITOR_DEF* monitor;
	CGDirectDisplayID displayId;
	displayId = CGMainDisplayID();
	CGDisplayModeRef mode = CGDisplayCopyDisplayMode(displayId);
	subsystem->pixelWidth = CGDisplayModeGetPixelWidth(mode);
	subsystem->pixelHeight = CGDisplayModeGetPixelHeight(mode);
	wide = CGDisplayPixelsWide(displayId);
	high = CGDisplayPixelsHigh(displayId);
	CGDisplayModeRelease(mode);
	subsystem->retina = ((subsystem->pixelWidth / wide) == 2) ? TRUE : FALSE;

	if (subsystem->retina)
	{
		subsystem->width = wide;
		subsystem->height = high;
	}
	else
	{
		subsystem->width = subsystem->pixelWidth;
		subsystem->height = subsystem->pixelHeight;
	}

	subsystem->common.numMonitors = 1;
	monitor = &(subsystem->common.monitors[0]);
	monitor->left = 0;
	monitor->top = 0;
	monitor->right = subsystem->width - 1;
	monitor->bottom = subsystem->height - 1;
	monitor->flags = 1;
	return 1;
}

static int mac_shadow_capture_start(macShadowSubsystem* subsystem)
{
	CGError err;
	err = CGDisplayStreamStart(subsystem->stream);

	if (err != kCGErrorSuccess)
	{
		WLog_ERR(TAG, "CGDisplayStreamStart failed with status %" PRId32, (INT32)err);
		return -1;
	}

	return 1;
}

static int mac_shadow_capture_stop(macShadowSubsystem* subsystem)
{
	CGError err;
	err = CGDisplayStreamStop(subsystem->stream);

	if (err != kCGErrorSuccess)
		return -1;

	return 1;
}

static int mac_shadow_capture_get_dirty_region(macShadowSubsystem* subsystem,
                                               CGDisplayStreamUpdateRef updateRef)
{
	size_t numRects = 0;
	const CGRect* rects = nullptr;
	rdpShadowSurface* surface = nullptr;

	if (!subsystem || !subsystem->common.server || !subsystem->common.server->surface || !updateRef)
		return -1;

	surface = subsystem->common.server->surface;
	if ((surface->width <= 0) || (surface->height <= 0) || (surface->width > UINT16_MAX) ||
	    (surface->height > UINT16_MAX))
		return -1;

	rects = CGDisplayStreamUpdateGetRects(updateRef, kCGDisplayStreamUpdateDirtyRects, &numRects);

	if (!rects || (numRects == 0))
	{
		WLog_DBG(TAG, "Display stream update contains no dirty rectangles");
		return -1;
	}

	for (size_t index = 0; index < numRects; index++)
	{
		RECTANGLE_16 invalidRect = WINPR_C_ARRAY_INIT;
		const CGFloat scale = subsystem->retina ? 2.0 : 1.0;
		const CGRect rect = rects[index];
		double left = floor(CGRectGetMinX(rect) / scale);
		double top = floor(CGRectGetMinY(rect) / scale);
		double right = ceil(CGRectGetMaxX(rect) / scale);
		double bottom = ceil(CGRectGetMaxY(rect) / scale);

		if (!isfinite(left) || !isfinite(top) || !isfinite(right) || !isfinite(bottom))
			continue;

		left = fmax(0.0, fmin(left, surface->width));
		top = fmax(0.0, fmin(top, surface->height));
		right = fmax(0.0, fmin(right, surface->width));
		bottom = fmax(0.0, fmin(bottom, surface->height));

		if ((right <= left) || (bottom <= top))
			continue;

		invalidRect.left = (UINT16)left;
		invalidRect.top = (UINT16)top;
		invalidRect.right = (UINT16)right;
		invalidRect.bottom = (UINT16)bottom;

		if (!region16_union_rect(&(surface->invalidRegion), &(surface->invalidRegion),
		                         &invalidRect))
		{
			WLog_ERR(TAG, "Failed to add display stream dirty rectangle to invalid region");
			region16_clear(&(surface->invalidRegion));
			return -1;
		}
	}

	if (region16_is_empty(&(surface->invalidRegion)))
	{
		WLog_DBG(TAG, "Display stream update has no dirty rectangles inside the framebuffer");
		return -1;
	}

	return 1;
}

static int freerdp_image_copy_from_retina(BYTE* pDstData, DWORD DstFormat, int nDstStep, int nXDst,
                                          int nYDst, int nWidth, int nHeight, BYTE* pSrcData,
                                          int nSrcStep, int nXSrc, int nYSrc)
{
	BYTE* pSrcPixel;
	BYTE* pDstPixel;
	int nSrcPad;
	int nDstPad;
	int srcBitsPerPixel;
	int srcBytesPerPixel;
	int dstBitsPerPixel;
	int dstBytesPerPixel;
	srcBitsPerPixel = 24;
	srcBytesPerPixel = 8;

	if (nSrcStep < 0)
		nSrcStep = srcBytesPerPixel * nWidth;

	dstBitsPerPixel = FreeRDPGetBitsPerPixel(DstFormat);
	dstBytesPerPixel = FreeRDPGetBytesPerPixel(DstFormat);
	if (!pDstData || !pSrcData || (nWidth <= 0) || (nHeight <= 0) || (nXDst < 0) || (nYDst < 0) ||
	    (nXSrc < 0) || (nYSrc < 0) || (nSrcStep <= 0) || (srcBytesPerPixel * nWidth > nSrcStep) ||
	    (dstBytesPerPixel <= 0))
		return -1;

	if (nDstStep < 0)
		nDstStep = dstBytesPerPixel * nWidth;

	nSrcPad = (nSrcStep - (nWidth * srcBytesPerPixel));
	nDstPad = (nDstStep - (nWidth * dstBytesPerPixel));
	pSrcPixel = &pSrcData[(nYSrc * nSrcStep) + (nXSrc * 4)];
	pDstPixel = &pDstData[(nYDst * nDstStep) + (nXDst * 4)];

	for (int y = 0; y < nHeight; y++)
	{
		for (int x = 0; x < nWidth; x++)
		{
			UINT32 R, G, B;
			UINT32 color;
			/* simple box filter scaling, could be improved with better algorithm */
			B = pSrcPixel[0] + pSrcPixel[4] + pSrcPixel[nSrcStep + 0] + pSrcPixel[nSrcStep + 4];
			G = pSrcPixel[1] + pSrcPixel[5] + pSrcPixel[nSrcStep + 1] + pSrcPixel[nSrcStep + 5];
			R = pSrcPixel[2] + pSrcPixel[6] + pSrcPixel[nSrcStep + 2] + pSrcPixel[nSrcStep + 6];
			pSrcPixel += 8;
			color = FreeRDPGetColor(DstFormat, R >> 2, G >> 2, B >> 2, 0xFF);
			FreeRDPWriteColor(pDstPixel, DstFormat, color);
			pDstPixel += dstBytesPerPixel;
		}

		pSrcPixel = &pSrcPixel[nSrcPad + nSrcStep];
		pDstPixel = &pDstPixel[nDstPad];
	}

	return 1;
}

static void (^mac_capture_stream_handler)(
    CGDisplayStreamFrameStatus, uint64_t, IOSurfaceRef,
    CGDisplayStreamUpdateRef) = ^(CGDisplayStreamFrameStatus status, uint64_t displayTime,
                                  IOSurfaceRef frameSurface, CGDisplayStreamUpdateRef updateRef) {
  int x, y;
  int count;
  int width;
  int height;
  size_t srcStep;
  size_t srcWidth;
  size_t srcHeight;
  BOOL empty;
  BOOL surfaceLocked = FALSE;
  BOOL surfaceRegionLocked = FALSE;
  BOOL publish = FALSE;
  kern_return_t rc;
  BYTE* pSrcData = nullptr;
  RECTANGLE_16 surfaceRect;
  const RECTANGLE_16* extents;
  macShadowSubsystem* subsystem = g_Subsystem;
  rdpShadowServer* server = nullptr;
  rdpShadowSurface* surface = nullptr;

  if (!subsystem || !subsystem->common.server || !subsystem->common.server->surface)
	  return;

  server = subsystem->common.server;
  surface = server->surface;

  if (status != kCGDisplayStreamFrameStatusFrameComplete)
  {
	  switch (status)
	  {
		  case kCGDisplayStreamFrameStatusFrameIdle:
		  case kCGDisplayStreamFrameStatusStopped:
		  case kCGDisplayStreamFrameStatusFrameBlank:
		  default:
			  return;
	  }
  }

  if (!frameSurface || !updateRef)
	  return;

  EnterCriticalSection(&(surface->lock));
  surfaceRegionLocked = TRUE;
  if (mac_shadow_capture_get_dirty_region(subsystem, updateRef) < 0)
	  goto cleanup;

  if ((surface->width > UINT16_MAX) || (surface->height > UINT16_MAX))
  {
	  WLog_ERR(TAG, "Shadow surface dimensions exceed the region coordinate range");
	  goto cleanup;
  }

  surfaceRect.left = surfaceRect.top = 0;
  surfaceRect.right = (UINT16)surface->width;
  surfaceRect.bottom = (UINT16)surface->height;
  if (!region16_intersect_rect(&(surface->invalidRegion), &(surface->invalidRegion), &surfaceRect))
  {
	  WLog_ERR(TAG, "Failed to clamp invalid region to the shadow surface");
	  goto cleanup;
  }
  empty = region16_is_empty(&(surface->invalidRegion));

  if (!empty)
  {
	  extents = region16_extents(&(surface->invalidRegion));
	  x = extents->left;
	  y = extents->top;
	  width = extents->right - extents->left;
	  height = extents->bottom - extents->top;
	  rc = IOSurfaceLock(frameSurface, kIOSurfaceLockReadOnly, nullptr);
	  if (rc != kIOReturnSuccess)
	  {
		  WLog_ERR(TAG, "IOSurfaceLock failed with status 0x%08" PRIx32, (UINT32)rc);
		  goto cleanup;
	  }
	  surfaceLocked = TRUE;

	  pSrcData = (BYTE*)IOSurfaceGetBaseAddress(frameSurface);
	  srcStep = IOSurfaceGetBytesPerRow(frameSurface);
	  srcWidth = IOSurfaceGetWidth(frameSurface);
	  srcHeight = IOSurfaceGetHeight(frameSurface);
	  if (!pSrcData || (srcStep == 0) || (srcStep > INT_MAX) || (srcWidth == 0) ||
		  (srcHeight == 0) || (srcWidth > (SIZE_MAX / 4)) || (srcStep < (srcWidth * 4)))
	  {
		  WLog_ERR(TAG, "IOSurface has invalid storage dimensions, base address, or row stride");
		  goto cleanup;
	  }

	  if (subsystem->retina)
	  {
		  if (((size_t)extents->right * 2 > srcWidth) || ((size_t)extents->bottom * 2 > srcHeight))
		  {
			  WLog_ERR(TAG, "Retina dirty region exceeds the IOSurface bounds");
			  goto cleanup;
		  }
	  }
	  else if (((size_t)extents->right > srcWidth) || ((size_t)extents->bottom > srcHeight))
	  {
		  WLog_ERR(TAG, "Dirty region exceeds the IOSurface bounds");
		  goto cleanup;
	  }

	  if (subsystem->retina)
	  {
		  if (freerdp_image_copy_from_retina(surface->data, surface->format, surface->scanline, x,
			                                 y, width, height, pSrcData, (int)srcStep, x * 2,
			                                 y * 2) < 0)
		  {
			  WLog_ERR(TAG, "Failed to copy Retina IOSurface pixels");
			  goto cleanup;
		  }
	  }
	  else
	  {
		  if (!freerdp_image_copy_no_overlap(surface->data, surface->format, surface->scanline, x,
			                                 y, width, height, pSrcData, PIXEL_FORMAT_BGRX32,
			                                 (UINT32)srcStep, x, y, nullptr, FREERDP_FLIP_NONE))
		  {
			  WLog_ERR(TAG, "Failed to copy IOSurface pixels");
			  goto cleanup;
		  }
	  }

	  rc = IOSurfaceUnlock(frameSurface, kIOSurfaceLockReadOnly, nullptr);
	  surfaceLocked = FALSE;
	  if (rc != kIOReturnSuccess)
	  {
		  WLog_ERR(TAG, "IOSurfaceUnlock failed with status 0x%08" PRIx32, (UINT32)rc);
		  goto cleanup;
	  }

	  LeaveCriticalSection(&(surface->lock));
	  surfaceRegionLocked = FALSE;
	  publish = TRUE;

	  ArrayList_Lock(server->clients);
	  count = ArrayList_Count(server->clients);
	  shadow_subsystem_frame_update(&subsystem->common);

	  if (count == 1)
	  {
		  rdpShadowClient* client;
		  client = (rdpShadowClient*)ArrayList_GetItem(server->clients, 0);

		  if (client)
		  {
			  subsystem->common.captureFrameRate = shadow_encoder_preferred_fps(client->encoder);
		  }
	  }

	  ArrayList_Unlock(server->clients);

	  EnterCriticalSection(&(surface->lock));
	  surfaceRegionLocked = TRUE;
	  region16_clear(&(surface->invalidRegion));
  }

cleanup:
  if (surfaceLocked)
  {
	  rc = IOSurfaceUnlock(frameSurface, kIOSurfaceLockReadOnly, nullptr);
	  if (rc != kIOReturnSuccess)
		  WLog_ERR(TAG, "IOSurfaceUnlock during cleanup failed with status 0x%08" PRIx32,
			       (UINT32)rc);
  }

  if (surfaceRegionLocked)
  {
	  if (!publish)
		  region16_clear(&(surface->invalidRegion));
	  LeaveCriticalSection(&(surface->lock));
  }
};

static int mac_shadow_capture_init(macShadowSubsystem* subsystem)
{
	void* keys[2];
	void* values[2];
	CFDictionaryRef opts;
	CGDirectDisplayID displayId;
	displayId = CGMainDisplayID();
	subsystem->captureQueue = dispatch_queue_create("mac.shadow.capture", nullptr);
	if (!subsystem->captureQueue)
	{
		WLog_ERR(TAG, "Failed to create display capture queue");
		return -1;
	}

	keys[0] = (void*)kCGDisplayStreamShowCursor;
	values[0] = (void*)kCFBooleanFalse;
	opts = CFDictionaryCreate(kCFAllocatorDefault, (const void**)keys, (const void**)values, 1,
	                          nullptr, nullptr);
	if (!opts)
	{
		WLog_ERR(TAG, "Failed to create display stream options");
		return -1;
	}

	subsystem->stream = CGDisplayStreamCreateWithDispatchQueue(
	    displayId, subsystem->pixelWidth, subsystem->pixelHeight, 'BGRA', opts,
	    subsystem->captureQueue, mac_capture_stream_handler);
	CFRelease(opts);
	if (!subsystem->stream)
	{
		WLog_ERR(TAG, "Failed to create CGDisplayStream");
		return -1;
	}

	return 1;
}

static int mac_shadow_screen_grab(macShadowSubsystem* subsystem)
{
	return 1;
}

static int mac_shadow_subsystem_process_message(macShadowSubsystem* subsystem, wMessage* message)
{
	switch (message->id)
	{
		case SHADOW_MSG_IN_REFRESH_REQUEST_ID:
			if (!subsystem->captureQueue)
				return -1;

			/*
			 * CGDisplayStream callbacks publish from captureQueue. Serialize refresh
			 * publication on that queue as well so updateEvent never has two producers.
			 * Do not hold surface->lock here: clients acquire it while consuming the event.
			 */
			dispatch_sync(subsystem->captureQueue, ^{
			  shadow_subsystem_frame_update((rdpShadowSubsystem*)subsystem);
			});
			break;

		default:
			WLog_ERR(TAG, "Unknown message id: %" PRIu32 "", message->id);
			break;
	}

	if (message->Free)
		message->Free(message);

	return 1;
}

static DWORD WINAPI mac_shadow_subsystem_thread(LPVOID arg)
{
	macShadowSubsystem* subsystem = (macShadowSubsystem*)arg;
	DWORD status;
	DWORD nCount;
	UINT64 cTime;
	DWORD dwTimeout;
	DWORD dwInterval;
	UINT64 frameTime;
	HANDLE events[32];
	wMessage message;
	wMessagePipe* MsgPipe;
	MsgPipe = subsystem->common.MsgPipe;
	nCount = 0;
	events[nCount++] = MessageQueue_Event(MsgPipe->In);
	subsystem->common.captureFrameRate = 16;
	dwInterval = 1000 / subsystem->common.captureFrameRate;
	frameTime = GetTickCount64() + dwInterval;

	while (1)
	{
		cTime = GetTickCount64();
		dwTimeout = (cTime > frameTime) ? 0 : frameTime - cTime;
		status = WaitForMultipleObjects(nCount, events, FALSE, dwTimeout);

		if (WaitForSingleObject(MessageQueue_Event(MsgPipe->In), 0) == WAIT_OBJECT_0)
		{
			if (MessageQueue_Peek(MsgPipe->In, &message, TRUE))
			{
				if (message.id == WMQ_QUIT)
					break;

				mac_shadow_subsystem_process_message(subsystem, &message);
			}
		}

		if ((status == WAIT_TIMEOUT) || (GetTickCount64() > frameTime))
		{
			mac_shadow_screen_grab(subsystem);
			dwInterval = 1000 / subsystem->common.captureFrameRate;
			frameTime += dwInterval;
		}
	}

	ExitThread(0);
	return 0;
}

static UINT32 mac_shadow_enum_monitors(MONITOR_DEF* monitors, UINT32 maxMonitors)
{
	int index;
	size_t wide, high;
	UINT32 numMonitors = 0;
	MONITOR_DEF* monitor;
	CGDirectDisplayID displayId;
	displayId = CGMainDisplayID();
	CGDisplayModeRef mode = CGDisplayCopyDisplayMode(displayId);
	wide = CGDisplayPixelsWide(displayId);
	high = CGDisplayPixelsHigh(displayId);
	CGDisplayModeRelease(mode);
	index = 0;
	numMonitors = 1;
	monitor = &monitors[index];
	monitor->left = 0;
	monitor->top = 0;
	monitor->right = (int)wide - 1;
	monitor->bottom = (int)high - 1;
	monitor->flags = 1;
	return numMonitors;
}

static int mac_shadow_subsystem_init(rdpShadowSubsystem* rdpsubsystem)
{
	macShadowSubsystem* subsystem = (macShadowSubsystem*)rdpsubsystem;
	g_Subsystem = subsystem;

	if (mac_shadow_detect_monitors(subsystem) < 0)
		return -1;

	return mac_shadow_capture_init(subsystem);
}

static int mac_shadow_subsystem_uninit(rdpShadowSubsystem* rdpsubsystem)
{
	macShadowSubsystem* subsystem = (macShadowSubsystem*)rdpsubsystem;
	if (!subsystem)
		return -1;

	return 1;
}

static int mac_shadow_subsystem_start(rdpShadowSubsystem* rdpsubsystem)
{
	macShadowSubsystem* subsystem = (macShadowSubsystem*)rdpsubsystem;
	HANDLE thread;

	if (!subsystem)
		return -1;

	if (mac_shadow_capture_start(subsystem) < 0)
		return -1;

	if (!(thread =
	          CreateThread(nullptr, 0, mac_shadow_subsystem_thread, (void*)subsystem, 0, nullptr)))
	{
		WLog_ERR(TAG, "Failed to create thread");
		return -1;
	}

	return 1;
}

static int mac_shadow_subsystem_stop(rdpShadowSubsystem* subsystem)
{
	if (!subsystem)
		return -1;

	return 1;
}

static void mac_shadow_subsystem_free(rdpShadowSubsystem* subsystem)
{
	if (!subsystem)
		return;

	mac_shadow_subsystem_uninit(subsystem);
	free(subsystem);
}

static rdpShadowSubsystem* mac_shadow_subsystem_new(void)
{
	macShadowSubsystem* subsystem = calloc(1, sizeof(macShadowSubsystem));

	if (!subsystem)
		return nullptr;

	subsystem->common.SynchronizeEvent = mac_shadow_input_synchronize_event;
	subsystem->common.KeyboardEvent = mac_shadow_input_keyboard_event;
	subsystem->common.UnicodeKeyboardEvent = mac_shadow_input_unicode_keyboard_event;
	subsystem->common.MouseEvent = mac_shadow_input_mouse_event;
	subsystem->common.ExtendedMouseEvent = mac_shadow_input_extended_mouse_event;
	return &subsystem->common;
}

FREERDP_API const char* ShadowSubsystemName(void)
{
	return "Mac";
}

FREERDP_API int ShadowSubsystemEntry(RDP_SHADOW_ENTRY_POINTS* pEntryPoints)
{
	char name[] = "mac shadow subsystem";
	char* arg[] = { name };

	freerdp_server_warn_unmaintained(ARRAYSIZE(arg), arg);
	pEntryPoints->New = mac_shadow_subsystem_new;
	pEntryPoints->Free = mac_shadow_subsystem_free;
	pEntryPoints->Init = mac_shadow_subsystem_init;
	pEntryPoints->Uninit = mac_shadow_subsystem_uninit;
	pEntryPoints->Start = mac_shadow_subsystem_start;
	pEntryPoints->Stop = mac_shadow_subsystem_stop;
	pEntryPoints->EnumMonitors = mac_shadow_enum_monitors;
	return 1;
}
