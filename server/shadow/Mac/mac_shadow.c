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

#include <errno.h>
#include <dlfcn.h>
#include <float.h>
#include <math.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <freerdp/server/server-common.h>
#include <freerdp/codec/color.h>
#include <freerdp/codec/region.h>
#include <freerdp/log.h>

#include "mac_shadow.h"

#define TAG SERVER_TAG("shadow.mac")

#define MAC_SHADOW_TEST_TONE_FRAMES 882
#define MAC_SHADOW_CGS_MODE_DESCRIPTION_LENGTH 0xD4
#define MAC_SHADOW_CGS_MODE_STORAGE_LENGTH 0xDC
#define MAC_SHADOW_CGS_MAX_MODES 4096

typedef struct
{
	SHADOW_MSG_OUT_AUDIO_OUT_SAMPLES message;
	INT16 samples[MAC_SHADOW_TEST_TONE_FRAMES * 2];
} MAC_SHADOW_TEST_TONE_MESSAGE;

typedef struct
{
	SHADOW_MSG_OUT_AUDIO_OUT_SAMPLES message;
	INT16 samples[];
} MAC_SHADOW_AUDIO_MESSAGE;

typedef union
{
	BYTE raw[MAC_SHADOW_CGS_MODE_STORAGE_LENGTH];
	struct
	{
		UINT32 mode;
		UINT32 flags;
		UINT32 width;
		UINT32 height;
		UINT32 depth;
		UINT32 reserved1[42];
		UINT16 reserved2;
		UINT16 frequency;
		UINT32 reserved3[4];
		float density;
	} value;
} MAC_SHADOW_CGS_MODE;

typedef void (*MAC_SHADOW_CGS_GET_CURRENT_DISPLAY_MODE)(CGDirectDisplayID display, int* mode);
typedef void (*MAC_SHADOW_CGS_GET_NUMBER_OF_DISPLAY_MODES)(CGDirectDisplayID display,
                                                           int* count);
typedef void (*MAC_SHADOW_CGS_GET_DISPLAY_MODE_DESCRIPTION)(CGDirectDisplayID display, int index,
                                                            MAC_SHADOW_CGS_MODE* mode, int length);
typedef void (*MAC_SHADOW_CGS_CONFIGURE_DISPLAY_MODE)(CGDisplayConfigRef config,
                                                      CGDirectDisplayID display, int mode);

typedef struct
{
	BOOL loadAttempted;
	void* handle;
	MAC_SHADOW_CGS_GET_CURRENT_DISPLAY_MODE getCurrentDisplayMode;
	MAC_SHADOW_CGS_GET_NUMBER_OF_DISPLAY_MODES getNumberOfDisplayModes;
	MAC_SHADOW_CGS_GET_DISPLAY_MODE_DESCRIPTION getDisplayModeDescription;
	MAC_SHADOW_CGS_CONFIGURE_DISPLAY_MODE configureDisplayMode;
} MAC_SHADOW_CGS_API;

typedef struct
{
	int mode;
	UINT32 width;
	UINT32 height;
	UINT32 depth;
	UINT16 frequency;
	float density;
	double score;
	double refreshDifference;
	BOOL depthMatch;
	BOOL densityMatch;
} MAC_SHADOW_PRIVATE_MODE_CANDIDATE;

static AUDIO_FORMAT g_MacShadowAudioFormat = { WAVE_FORMAT_PCM, 2, 44100, 176400, 4, 16, 0,
	                                           nullptr };

static macShadowSubsystem* g_Subsystem = nullptr;
static MAC_SHADOW_CGS_API g_CgsApi = WINPR_C_ARRAY_INIT;

extern char** environ;

static int mac_shadow_switch_display_mode(macShadowSubsystem* subsystem, const char* command,
	                                      const char* transition);
static int mac_shadow_switch_to_client_display_mode(macShadowSubsystem* subsystem,
                                                    const rdpSettings* settings);
static int mac_shadow_restore_pre_connection_display_mode(macShadowSubsystem* subsystem,
                                                          const char* transition);
static int mac_shadow_restore_connection_display_mode(macShadowSubsystem* subsystem,
                                                      const char* transition);
static int mac_shadow_use_scaled_client_surface(macShadowSubsystem* subsystem, UINT32 width,
                                                UINT32 height);
static int mac_shadow_capture_init(macShadowSubsystem* subsystem);
static int mac_shadow_capture_start(macShadowSubsystem* subsystem);
static int mac_shadow_capture_release_stream(macShadowSubsystem* subsystem);
static int mac_shadow_test_tone_start(macShadowSubsystem* subsystem);
static void mac_shadow_test_tone_stop(macShadowSubsystem* subsystem);
static int mac_shadow_system_audio_start(macShadowSubsystem* subsystem);
static void mac_shadow_system_audio_stop(macShadowSubsystem* subsystem);

static void mac_shadow_message_free(UINT32 id, SHADOW_MSG_OUT* msg)
{
	WINPR_UNUSED(id);
	free(msg);
}

static void mac_shadow_audio_message_free(UINT32 id, SHADOW_MSG_OUT* msg)
{
	WINPR_UNUSED(id);
	free(msg);
}

static BOOL mac_shadow_audio_client_ready(macShadowSubsystem* subsystem)
{
	BOOL ready = FALSE;
	rdpShadowServer* server = subsystem->common.server;

	if (!server || !server->clients)
		return FALSE;

	ArrayList_Lock(server->clients);
	if (ArrayList_Count(server->clients) == 1)
	{
		rdpShadowClient* client = (rdpShadowClient*)ArrayList_GetItem(server->clients, 0);
		RdpsndServerContext* rdpsnd = client ? client->rdpsnd : nullptr;
		if (rdpsnd && (rdpsnd->num_client_formats > 0))
		{
			if (!subsystem->audioNegotiated && !subsystem->audioUnavailable)
			{
				for (UINT16 index = 0; index < rdpsnd->num_client_formats; index++)
				{
					if (!audio_format_compatible(&g_MacShadowAudioFormat,
					                             &rdpsnd->client_formats[index]))
						continue;

					rdpsnd->src_format = &g_MacShadowAudioFormat;
					if (rdpsnd->SelectFormat(rdpsnd, index) == CHANNEL_RC_OK)
					{
						subsystem->audioNegotiated = TRUE;
						WLog_INFO(TAG, "RDP audio negotiated 44100 Hz stereo PCM");
					}
					break;
				}

				if (!subsystem->audioNegotiated)
				{
					subsystem->audioUnavailable = TRUE;
					WLog_WARN(TAG, "RDP client does not offer 44100 Hz stereo PCM");
				}
			}
			ready = subsystem->audioNegotiated;
		}
	}
	ArrayList_Unlock(server->clients);
	return ready;
}

static void mac_shadow_system_audio_samples(void* context, const INT16* samples, size_t frames)
{
	macShadowSubsystem* subsystem = (macShadowSubsystem*)context;
	MAC_SHADOW_AUDIO_MESSAGE* message = nullptr;
	if (!subsystem || !samples || (frames == 0) ||
	    !mac_shadow_audio_client_ready(subsystem) ||
	    (frames > ((SIZE_MAX - sizeof(MAC_SHADOW_AUDIO_MESSAGE)) / (2 * sizeof(INT16)))))
	{
		return;
	}

	message = (MAC_SHADOW_AUDIO_MESSAGE*)malloc(sizeof(MAC_SHADOW_AUDIO_MESSAGE) +
	                                           (frames * 2 * sizeof(INT16)));
	if (!message)
		return;
	memset(&message->message, 0, sizeof(message->message));
	memcpy(message->samples, samples, frames * 2 * sizeof(INT16));
	message->message.common.Free = mac_shadow_audio_message_free;
	message->message.audio_format = &g_MacShadowAudioFormat;
	message->message.buf = message->samples;
	message->message.nFrames = frames;
	message->message.wTimestamp = (UINT16)(GetTickCount64() & UINT16_MAX);
	(void)shadow_client_boardcast_msg(subsystem->common.server, nullptr,
	                                  SHADOW_MSG_OUT_AUDIO_OUT_SAMPLES_ID,
	                                  (SHADOW_MSG_OUT*)message, nullptr);
}

static int mac_shadow_system_audio_start(macShadowSubsystem* subsystem)
{
	if (!subsystem)
		return -1;
	if (!subsystem->audioCapture)
	{
		subsystem->audioCapture =
		    mac_shadow_audio_new(mac_shadow_system_audio_samples, subsystem);
	}
	if (!subsystem->audioCapture || (mac_shadow_audio_start(subsystem->audioCapture) < 0))
		return -1;
	WLog_INFO(TAG, "macOS system-audio capture started");
	return 1;
}

static void mac_shadow_system_audio_stop(macShadowSubsystem* subsystem)
{
	if (!subsystem || !subsystem->audioCapture)
		return;
	mac_shadow_audio_stop(subsystem->audioCapture);
	WLog_INFO(TAG, "macOS system-audio capture stopped");
}

static void mac_shadow_test_tone_publish(macShadowSubsystem* subsystem)
{
	MAC_SHADOW_TEST_TONE_MESSAGE* tone = nullptr;
	const double phaseStep = (2.0 * M_PI * 440.0) / g_MacShadowAudioFormat.nSamplesPerSec;

	if (!mac_shadow_audio_client_ready(subsystem))
		return;

	tone = (MAC_SHADOW_TEST_TONE_MESSAGE*)calloc(1, sizeof(MAC_SHADOW_TEST_TONE_MESSAGE));
	if (!tone)
		return;

	for (size_t frame = 0; frame < MAC_SHADOW_TEST_TONE_FRAMES; frame++)
	{
		const INT16 sample = (INT16)(sin(subsystem->testTonePhase) * 8192.0);
		tone->samples[frame * 2] = sample;
		tone->samples[(frame * 2) + 1] = sample;
		subsystem->testTonePhase += phaseStep;
		if (subsystem->testTonePhase >= (2.0 * M_PI))
			subsystem->testTonePhase -= 2.0 * M_PI;
	}

	tone->message.common.Free = mac_shadow_audio_message_free;
	tone->message.audio_format = &g_MacShadowAudioFormat;
	tone->message.buf = tone->samples;
	tone->message.nFrames = MAC_SHADOW_TEST_TONE_FRAMES;
	tone->message.wTimestamp = (UINT16)(GetTickCount64() & UINT16_MAX);
	(void)shadow_client_boardcast_msg(subsystem->common.server, nullptr,
	                                  SHADOW_MSG_OUT_AUDIO_OUT_SAMPLES_ID,
	                                  (SHADOW_MSG_OUT*)tone, nullptr);
}

static int mac_shadow_test_tone_start(macShadowSubsystem* subsystem)
{
	if (!subsystem || !subsystem->testToneEnabled)
		return 1;
	if (subsystem->audioTimer)
		return 1;

	if (!subsystem->audioQueue)
		subsystem->audioQueue = dispatch_queue_create("mac.shadow.audio.test", nullptr);
	if (!subsystem->audioQueue)
		return -1;

	subsystem->audioTimer =
	    dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, subsystem->audioQueue);
	if (!subsystem->audioTimer)
		return -1;

	dispatch_source_set_timer(subsystem->audioTimer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC),
	                          20 * NSEC_PER_MSEC, 2 * NSEC_PER_MSEC);
	dispatch_source_set_event_handler(subsystem->audioTimer, ^{
	  mac_shadow_test_tone_publish(subsystem);
	});
	dispatch_resume(subsystem->audioTimer);
	WLog_INFO(TAG, "Enabled the opt-in 440 Hz RDP audio test tone");
	return 1;
}

static void mac_shadow_test_tone_stop(macShadowSubsystem* subsystem)
{
	dispatch_source_t timer = nullptr;
	if (!subsystem || !subsystem->audioTimer)
		return;

	timer = subsystem->audioTimer;
	subsystem->audioTimer = nullptr;
	dispatch_source_cancel(timer);
	dispatch_sync(subsystem->audioQueue, ^{
	});
#if !OS_OBJECT_USE_OBJC
	dispatch_release(timer);
#endif
}

static BOOL mac_shadow_is_win98_compat_profile(const rdpSettings* settings)
{
	WINPR_ASSERT(settings);

	return (freerdp_settings_get_uint32(settings, FreeRDP_RdpVersion) ==
	        RDP_VERSION_5_PLUS) &&
	       (freerdp_settings_get_uint32(settings, FreeRDP_OsMajorType) ==
	        OSMAJORTYPE_WINDOWS) &&
	       (freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth) == 1024) &&
	       (freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight) == 768) &&
	       (freerdp_settings_get_uint32(settings, FreeRDP_ColorDepth) == 16);
}

static void mac_shadow_apply_client_profile(macShadowSubsystem* subsystem,
                                            const rdpSettings* settings)
{
	WINPR_ASSERT(subsystem);
	WINPR_ASSERT(subsystem->common.server);
	WINPR_ASSERT(settings);

	const char* hostname = freerdp_settings_get_string(settings, FreeRDP_ClientHostname);
	const char* product = freerdp_settings_get_string(settings, FreeRDP_ClientProductId);
	const UINT32 width = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
	const UINT32 height = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
	const UINT32 colorDepth = freerdp_settings_get_uint32(settings, FreeRDP_ColorDepth);
	const UINT32 rdpVersion = freerdp_settings_get_uint32(settings, FreeRDP_RdpVersion);
	const UINT32 clientBuild = freerdp_settings_get_uint32(settings, FreeRDP_ClientBuild);
	const UINT32 osMajorType = freerdp_settings_get_uint32(settings, FreeRDP_OsMajorType);
	const UINT32 osMinorType = freerdp_settings_get_uint32(settings, FreeRDP_OsMinorType);
	const BOOL win98Profile = mac_shadow_is_win98_compat_profile(settings);

	if (subsystem->autoClientProfile)
	{
		/* The validated Windows RDP 5 client already has a low-latency local cursor. Other
		 * platforms use the cursor composited into the capture stream; RDC 2.x for Mac does not
		 * reliably render the server's default system-pointer update. */
		subsystem->common.server->ShowMouseCursor =
		    subsystem->configuredShowMouseCursor || !win98Profile;
	}
	else
	{
		subsystem->common.server->ShowMouseCursor = subsystem->configuredShowMouseCursor;
	}

	WLog_INFO(TAG,
	          "Client profile=%s, hostname=%s, product=%s, build=%" PRIu32
	          ", RdpVersion=0x%08" PRIx32 ", os=0x%04" PRIx32 "/0x%04" PRIx32
	          ", requested=%" PRIu32 "x%" PRIu32 "@%" PRIu32
	          ", cursor=%s",
	          win98Profile ? "win98-1024x768x16" : "adaptive", hostname ? hostname : "<unknown>",
	          product ? product : "<unknown>", clientBuild, rdpVersion, osMajorType, osMinorType,
	          width, height, colorDepth,
	          subsystem->common.server->ShowMouseCursor ? "captured" : "client-local");
}

static BOOL mac_shadow_client_connect(rdpShadowSubsystem* subsystem, rdpShadowClient* client)
{
	SHADOW_MSG_OUT_POINTER_ALPHA_UPDATE* msg = nullptr;
	macShadowSubsystem* mac = (macShadowSubsystem*)subsystem;

	if (!subsystem || !subsystem->server || !client)
		return FALSE;

	EnterCriticalSection(&mac->connectionLock);
	if (mac->connectedClients == 0)
	{
		rdpSettings* settings = client->context.settings;
		if (!settings)
		{
			LeaveCriticalSection(&mac->connectionLock);
			return FALSE;
		}

		mac_shadow_apply_client_profile(mac, settings);
		if (mac->connectDisplayCommand &&
		    (mac_shadow_switch_display_mode(mac, mac->connectDisplayCommand,
		                                    "first client connect") < 0))
		{
			mac->common.server->ShowMouseCursor = mac->configuredShowMouseCursor;
			LeaveCriticalSection(&mac->connectionLock);
			return FALSE;
		}
		if (mac->connectDisplayCommand)
			mac->connectionDisplayModeActive = TRUE;
		else if (mac->autoClientProfile &&
		         (mac_shadow_switch_to_client_display_mode(mac, settings) < 0))
		{
			mac->common.server->ShowMouseCursor = mac->configuredShowMouseCursor;
			LeaveCriticalSection(&mac->connectionLock);
			return FALSE;
		}

		if ((mac_shadow_capture_init(mac) < 0) || (mac_shadow_capture_start(mac) < 0))
		{
			(void)mac_shadow_capture_release_stream(mac);
			if (mac_shadow_restore_connection_display_mode(mac,
			                                               "failed first client connect") < 0)
			{
				WLog_ERR(TAG, "Failed to restore the display mode after capture setup failed");
			}
			mac->common.server->ShowMouseCursor = mac->configuredShowMouseCursor;
			LeaveCriticalSection(&mac->connectionLock);
			return FALSE;
		}
		if (mac_shadow_test_tone_start(mac) < 0)
			WLog_WARN(TAG, "Failed to start the opt-in RDP audio test tone");
		if (!mac->testToneEnabled && (mac_shadow_system_audio_start(mac) < 0))
			WLog_WARN(TAG, "Failed to start macOS system-audio capture");

		WLog_INFO(TAG, "Display capture started for the first connected client");
	}
	mac->connectedClients++;
	LeaveCriticalSection(&mac->connectionLock);

	/* When the display stream excludes the pointer, request the client's local system pointer.
	 * The generic shadow client translates this message to SYSPTR_DEFAULT. */
	if (subsystem->server->ShowMouseCursor)
		return TRUE;

	msg = (SHADOW_MSG_OUT_POINTER_ALPHA_UPDATE*)calloc(
	    1, sizeof(SHADOW_MSG_OUT_POINTER_ALPHA_UPDATE));
	if (!msg)
	{
		WLog_WARN(TAG, "Failed to allocate the client-side pointer update");
		return TRUE;
	}

	msg->common.Free = mac_shadow_message_free;
	if (!shadow_client_post_msg(client, nullptr, SHADOW_MSG_OUT_POINTER_ALPHA_UPDATE_ID,
	                            (SHADOW_MSG_OUT*)msg, nullptr))
	{
		free(msg);
		WLog_WARN(TAG, "Failed to post the client-side pointer update");
		return TRUE;
	}

	return TRUE;
}

static void mac_shadow_client_disconnect(rdpShadowSubsystem* subsystem, rdpShadowClient* client)
{
	macShadowSubsystem* mac = (macShadowSubsystem*)subsystem;

	if (!subsystem || !client)
		return;

	EnterCriticalSection(&mac->connectionLock);
	if (mac->connectedClients == 0)
	{
		WLog_WARN(TAG, "Client disconnect received with no connected macOS shadow clients");
	}
	else
	{
		mac->connectedClients--;
		if (mac->connectedClients == 0)
		{
			mac_shadow_test_tone_stop(mac);
			mac_shadow_system_audio_stop(mac);
			mac->audioNegotiated = FALSE;
			mac->audioUnavailable = FALSE;
			if (mac_shadow_capture_release_stream(mac) < 0)
				WLog_ERR(TAG, "Failed to stop display capture after the last client disconnected");
			else
				WLog_INFO(TAG, "Display capture stopped after the last client disconnected");

			if (mac_shadow_restore_connection_display_mode(mac,
			                                               "last client disconnect") < 0)
			{
				WLog_ERR(TAG,
				         "Failed to restore the display mode after the last client disconnected");
			}
			mac->common.server->ShowMouseCursor = mac->configuredShowMouseCursor;
		}
	}
	LeaveCriticalSection(&mac->connectionLock);
}

static CGEventFlags mac_shadow_keyboard_modifier_flag(DWORD vkcode)
{
	switch (vkcode & ~KBDEXT)
	{
		case VK_LSHIFT:
		case VK_RSHIFT:
			return kCGEventFlagMaskShift;

		case VK_LCONTROL:
		case VK_RCONTROL:
			return kCGEventFlagMaskControl;

		case VK_LMENU:
		case VK_RMENU:
			return kCGEventFlagMaskAlternate;

		case VK_LWIN:
		case VK_RWIN:
			return kCGEventFlagMaskCommand;

		default:
			return 0;
	}
}

static BOOL mac_shadow_input_synchronize_event(rdpShadowSubsystem* subsystem,
                                               rdpShadowClient* client, UINT32 flags)
{
	if (!subsystem || !client)
		return FALSE;

	macShadowSubsystem* mac = (macShadowSubsystem*)subsystem;
	mac->keyboardFlags = (flags & KBD_SYNC_CAPS_LOCK) ? kCGEventFlagMaskAlphaShift : 0;

	return TRUE;
}

static BOOL mac_shadow_input_keyboard_event(rdpShadowSubsystem* subsystem, rdpShadowClient* client,
                                            UINT16 flags, UINT8 code)
{
	DWORD vkcode;
	DWORD keycode;
	DWORD scancode;
	CGEventFlags modifierFlag;
	BOOL extended;
	CGEventRef kbdEvent;
	macShadowSubsystem* mac = (macShadowSubsystem*)subsystem;
	extended = (flags & KBD_FLAGS_EXTENDED) ? TRUE : FALSE;

	if (!subsystem || !client || !mac->eventSource)
		return FALSE;

	scancode = code;
	if (extended)
		scancode |= KBDEXT;

	vkcode = GetVirtualKeyCodeFromVirtualScanCode(scancode, WINPR_KBD_TYPE_IBM_ENHANCED);

	if (extended)
		vkcode |= KBDEXT;

	if (vkcode == (VK_RCONTROL | KBDEXT))
		keycode = APPLE_VK_RightControl;
	else
		keycode = GetKeycodeFromVirtualKeyCode(vkcode, WINPR_KEYCODE_TYPE_APPLE);

	if ((vkcode & ~KBDEXT) == VK_CAPITAL)
	{
		if ((flags & KBD_FLAGS_RELEASE) == 0)
			mac->keyboardFlags ^= kCGEventFlagMaskAlphaShift;
	}
	else
	{
		modifierFlag = mac_shadow_keyboard_modifier_flag(vkcode);
		if ((flags & KBD_FLAGS_RELEASE) != 0)
			mac->keyboardFlags &= ~modifierFlag;
		else
			mac->keyboardFlags |= modifierFlag;
	}

	kbdEvent = CGEventCreateKeyboardEvent(mac->eventSource, (CGKeyCode)keycode,
	                                      (flags & KBD_FLAGS_RELEASE) == 0);
	if (!kbdEvent)
		return FALSE;

	CGEventSetFlags(kbdEvent, mac->keyboardFlags);
	CGEventPost(kCGHIDEventTap, kbdEvent);
	CFRelease(kbdEvent);

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

	CGPoint location = CGPointMake(x, y);
	if (mac->scaledClientSurface && (mac->width > 0) && (mac->height > 0) &&
	    (mac->desktopWidth > 0) && (mac->desktopHeight > 0))
	{
		/* CGDisplayStream preserves the physical display's aspect ratio when it renders into an
		 * arbitrary client-sized surface. Undo that letterbox transform for injected input. */
		const double scale = fmin((double)mac->width / mac->desktopWidth,
		                          (double)mac->height / mac->desktopHeight);
		const double contentWidth = mac->desktopWidth * scale;
		const double contentHeight = mac->desktopHeight * scale;
		const double offsetX = ((double)mac->width - contentWidth) / 2.0;
		const double offsetY = ((double)mac->height - contentHeight) / 2.0;
		location.x = fmax(0.0, fmin(((double)x - offsetX) / scale,
		                              (double)mac->desktopWidth - 1.0));
		location.y = fmax(0.0, fmin(((double)y - offsetY) / scale,
		                              (double)mac->desktopHeight - 1.0));
	}

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

			CGEventRef move = CGEventCreateMouseEvent(source, mouseType, location, mouseButton);
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

		CGEventRef mouseEvent = CGEventCreateMouseEvent(source, mouseType, location, mouseButton);
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
	MONITOR_DEF* virtualScreen;
	CGDirectDisplayID displayId;
	displayId = CGMainDisplayID();
	CGDisplayModeRef mode = CGDisplayCopyDisplayMode(displayId);
	if (!mode)
	{
		WLog_ERR(TAG, "Failed to query the main display mode");
		return -1;
	}

	subsystem->pixelWidth = CGDisplayModeGetPixelWidth(mode);
	subsystem->pixelHeight = CGDisplayModeGetPixelHeight(mode);
	wide = CGDisplayPixelsWide(displayId);
	high = CGDisplayPixelsHigh(displayId);
	CGDisplayModeRelease(mode);
	if ((wide == 0) || (high == 0) || (subsystem->pixelWidth <= 0) ||
	    (subsystem->pixelHeight <= 0))
	{
		WLog_ERR(TAG, "Main display reported invalid dimensions");
		return -1;
	}

	subsystem->retina = ((subsystem->pixelWidth / wide) == 2) ? TRUE : FALSE;

	if (subsystem->retina)
	{
		subsystem->desktopWidth = wide;
		subsystem->desktopHeight = high;
	}
	else
	{
		subsystem->desktopWidth = subsystem->pixelWidth;
		subsystem->desktopHeight = subsystem->pixelHeight;
	}

	if (subsystem->scaledClientSurface)
	{
		subsystem->width = subsystem->scaledClientWidth;
		subsystem->height = subsystem->scaledClientHeight;
		subsystem->pixelWidth = subsystem->scaledClientWidth;
		subsystem->pixelHeight = subsystem->scaledClientHeight;
		subsystem->retina = FALSE;
	}
	else
	{
		subsystem->width = subsystem->desktopWidth;
		subsystem->height = subsystem->desktopHeight;
	}

	subsystem->common.numMonitors = 1;
	monitor = &(subsystem->common.monitors[0]);
	monitor->left = 0;
	monitor->top = 0;
	monitor->right = subsystem->width - 1;
	monitor->bottom = subsystem->height - 1;
	monitor->flags = 1;
	virtualScreen = &subsystem->common.virtualScreen;
	*virtualScreen = *monitor;
	return 1;
}

static int mac_shadow_capture_start(macShadowSubsystem* subsystem)
{
	CGError err;
	if (!subsystem || !subsystem->stream)
		return -1;

	if (subsystem->captureRunning)
		return 1;

	err = CGDisplayStreamStart(subsystem->stream);

	if (err != kCGErrorSuccess)
	{
		WLog_ERR(TAG, "CGDisplayStreamStart failed with status %" PRId32, (INT32)err);
		return -1;
	}

	subsystem->captureRunning = TRUE;
	return 1;
}

static int mac_shadow_capture_stop(macShadowSubsystem* subsystem)
{
	CGError err;
	if (!subsystem || !subsystem->stream)
		return -1;

	if (!subsystem->captureRunning)
		return 1;

	err = CGDisplayStreamStop(subsystem->stream);

	if (err != kCGErrorSuccess)
	{
		WLog_ERR(TAG, "CGDisplayStreamStop failed with status %" PRId32, (INT32)err);
		return -1;
	}

	subsystem->captureRunning = FALSE;
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
  BOOL forceFullFrame = FALSE;
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
  if ((surface->width > UINT16_MAX) || (surface->height > UINT16_MAX))
  {
	  WLog_ERR(TAG, "Shadow surface dimensions exceed the region coordinate range");
	  goto cleanup;
  }

  surfaceRect.left = surfaceRect.top = 0;
  surfaceRect.right = (UINT16)surface->width;
  surfaceRect.bottom = (UINT16)surface->height;
	forceFullFrame = subsystem->captureNeedsFullFrame;
	if (forceFullFrame)
	{
		region16_clear(&(surface->invalidRegion));
		if (!region16_union_rect(&(surface->invalidRegion), &(surface->invalidRegion),
		                         &surfaceRect))
		{
			WLog_ERR(TAG, "Failed to request the complete first frame after a resize");
			goto cleanup;
		}
	}
	else if (mac_shadow_capture_get_dirty_region(subsystem, updateRef) < 0)
	{
		goto cleanup;
	}

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

	  if (!subsystem->retina && !forceFullFrame)
	  {
		  /* Core Graphics damage can cover an entire composited window for a tiny pixel change.
		   * Compare against the previous framebuffer so legacy bitmap clients only receive the
		   * pixels that actually changed. The X11 shadow backend uses the same comparator. */
		  RECTANGLE_16 changedRect = WINPR_C_ARRAY_INIT;
		  const BYTE* pOldData =
		      &surface->data[((size_t)y * surface->scanline) + ((size_t)x * 4)];
		  const BYTE* pNewData = &pSrcData[((size_t)y * srcStep) + ((size_t)x * 4)];
		  const int changed = shadow_capture_compare_with_format(
		      pOldData, surface->format, surface->scanline, width, height, pNewData,
		      PIXEL_FORMAT_BGRX32, (UINT32)srcStep, &changedRect);

		  if (changed < 0)
		  {
			  WLog_ERR(TAG, "Failed to compare captured pixels with the shadow surface");
			  goto cleanup;
		  }

		  if (changed == 0)
		  {
			  region16_clear(&(surface->invalidRegion));
			  goto cleanup;
		  }

		  changedRect.left += (UINT16)x;
		  changedRect.top += (UINT16)y;
		  changedRect.right += (UINT16)x;
		  changedRect.bottom += (UINT16)y;
		  region16_clear(&(surface->invalidRegion));
		  if (!region16_union_rect(&(surface->invalidRegion), &(surface->invalidRegion),
		                           &changedRect))
		  {
			  WLog_ERR(TAG, "Failed to record changed framebuffer pixels");
			  goto cleanup;
		  }

		  extents = region16_extents(&(surface->invalidRegion));
		  x = extents->left;
		  y = extents->top;
		  width = extents->right - extents->left;
		  height = extents->bottom - extents->top;
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
	  subsystem->captureNeedsFullFrame = FALSE;

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
	const void* keys[2];
	const void* values[2];
	size_t optionCount = 1;
	CFDictionaryRef opts;
	CGDirectDisplayID displayId;
	displayId = CGMainDisplayID();
	if (subsystem->stream)
	{
		WLog_ERR(TAG, "Display stream is already initialized");
		return -1;
	}

	if (!subsystem->captureQueue)
		subsystem->captureQueue = dispatch_queue_create("mac.shadow.capture", nullptr);
	if (!subsystem->captureQueue)
	{
		WLog_ERR(TAG, "Failed to create display capture queue");
		return -1;
	}

	keys[0] = kCGDisplayStreamShowCursor;
	values[0] = subsystem->common.server->ShowMouseCursor ? kCFBooleanTrue : kCFBooleanFalse;
	if (subsystem->scaledClientSurface)
	{
		keys[optionCount] = kCGDisplayStreamPreserveAspectRatio;
		values[optionCount] = kCFBooleanTrue;
		optionCount++;
	}
	opts = CFDictionaryCreate(kCFAllocatorDefault, keys, values, optionCount,
	                          &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
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
	subsystem->captureNeedsFullFrame = TRUE;

	return 1;
}

static int mac_shadow_capture_release_stream(macShadowSubsystem* subsystem)
{
	if (!subsystem)
		return -1;
	if (!subsystem->stream)
		return 1;

	if (mac_shadow_capture_stop(subsystem) < 0)
		return -1;

	/* CGDisplayStream callbacks run on this serial queue. Drain callbacks from the old stream
	 * before resizing its destination surface or releasing the stream. */
	dispatch_sync(subsystem->captureQueue, ^{
	});
	CFRelease(subsystem->stream);
	subsystem->stream = nullptr;
	return 1;
}

static int mac_shadow_run_display_command(const char* command, const char* transition)
{
	int rc;
	int status = 0;
	pid_t pid = 0;
	char* const argv[] = { (char*)command, nullptr };

	if (!command || !transition || (command[0] != '/') || (access(command, X_OK) != 0))
	{
		WLog_ERR(TAG, "The %s display command must be an executable absolute path: %s", transition,
		         command ? command : "(null)");
		return -1;
	}

	WLog_INFO(TAG, "Running %s display command: %s", transition, command);
	rc = posix_spawn(&pid, command, nullptr, nullptr, argv, environ);
	if (rc != 0)
	{
		WLog_ERR(TAG, "Failed to start %s display command '%s': %s", transition, command,
		         strerror(rc));
		return -1;
	}

	do
	{
		rc = waitpid(pid, &status, 0);
	} while ((rc < 0) && (errno == EINTR));

	if (rc < 0)
	{
		WLog_ERR(TAG, "Failed to wait for %s display command '%s': %s", transition, command,
		         strerror(errno));
		return -1;
	}

	if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0))
	{
		if (WIFEXITED(status))
			WLog_ERR(TAG, "%s display command exited with status %d: %s", transition,
			         WEXITSTATUS(status), command);
		else
			WLog_ERR(TAG, "%s display command terminated abnormally: %s", transition, command);
		return -1;
	}

	return 1;
}

static BOOL mac_shadow_load_private_symbol(void* handle, const char* name, void* target,
                                           size_t targetSize)
{
	void* symbol = dlsym(handle, name);
	if (!symbol || (targetSize != sizeof(symbol)))
		return FALSE;

	memcpy(target, &symbol, targetSize);
	return TRUE;
}

static BOOL mac_shadow_load_cgs_api(void)
{
	if (g_CgsApi.loadAttempted)
		return g_CgsApi.handle != nullptr;

	g_CgsApi.loadAttempted = TRUE;
	void* handle = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight",
	                      RTLD_LAZY | RTLD_LOCAL);
	if (!handle)
	{
		WLog_WARN(TAG, "The optional Sonoma scaled-display API is unavailable: %s", dlerror());
		return FALSE;
	}

	if (!mac_shadow_load_private_symbol(handle, "CGSGetCurrentDisplayMode",
	                                    &g_CgsApi.getCurrentDisplayMode,
	                                    sizeof(g_CgsApi.getCurrentDisplayMode)) ||
	    !mac_shadow_load_private_symbol(handle, "CGSGetNumberOfDisplayModes",
	                                    &g_CgsApi.getNumberOfDisplayModes,
	                                    sizeof(g_CgsApi.getNumberOfDisplayModes)) ||
	    !mac_shadow_load_private_symbol(handle, "CGSGetDisplayModeDescriptionOfLength",
	                                    &g_CgsApi.getDisplayModeDescription,
	                                    sizeof(g_CgsApi.getDisplayModeDescription)) ||
	    !mac_shadow_load_private_symbol(handle, "CGSConfigureDisplayMode",
	                                    &g_CgsApi.configureDisplayMode,
	                                    sizeof(g_CgsApi.configureDisplayMode)))
	{
		WLog_WARN(TAG, "The optional Sonoma scaled-display API has incomplete symbols");
		dlclose(handle);
		g_CgsApi.getCurrentDisplayMode = nullptr;
		g_CgsApi.getNumberOfDisplayModes = nullptr;
		g_CgsApi.getDisplayModeDescription = nullptr;
		g_CgsApi.configureDisplayMode = nullptr;
		return FALSE;
	}

	g_CgsApi.handle = handle;
	return TRUE;
}

static BOOL mac_shadow_get_private_display_mode(CGDirectDisplayID displayId, int index,
                                                MAC_SHADOW_CGS_MODE* mode)
{
	if (!mode || !mac_shadow_load_cgs_api())
		return FALSE;

	ZeroMemory(mode, sizeof(*mode));
	g_CgsApi.getDisplayModeDescription(displayId, index, mode,
	                                   MAC_SHADOW_CGS_MODE_DESCRIPTION_LENGTH);
	return (mode->value.width > 0) && (mode->value.height > 0);
}

static BOOL mac_shadow_get_current_private_display_mode(CGDirectDisplayID displayId, int* index,
                                                        MAC_SHADOW_CGS_MODE* mode)
{
	if (!index || !mac_shadow_load_cgs_api())
		return FALSE;

	*index = -1;
	g_CgsApi.getCurrentDisplayMode(displayId, index);
	if (*index < 0)
		return FALSE;
	return !mode || mac_shadow_get_private_display_mode(displayId, *index, mode);
}

static int mac_shadow_compare_private_mode_candidates(const void* left, const void* right)
{
	const MAC_SHADOW_PRIVATE_MODE_CANDIDATE* lhs =
	    (const MAC_SHADOW_PRIVATE_MODE_CANDIDATE*)left;
	const MAC_SHADOW_PRIVATE_MODE_CANDIDATE* rhs =
	    (const MAC_SHADOW_PRIVATE_MODE_CANDIDATE*)right;

	if (lhs->score < rhs->score)
		return -1;
	if (lhs->score > rhs->score)
		return 1;
	if (lhs->depthMatch != rhs->depthMatch)
		return lhs->depthMatch ? -1 : 1;
	if (lhs->densityMatch != rhs->densityMatch)
		return lhs->densityMatch ? -1 : 1;
	if (lhs->refreshDifference < rhs->refreshDifference)
		return -1;
	if (lhs->refreshDifference > rhs->refreshDifference)
		return 1;
	if (lhs->frequency != rhs->frequency)
		return lhs->frequency > rhs->frequency ? -1 : 1;
	if (lhs->depth != rhs->depth)
		return lhs->depth > rhs->depth ? -1 : 1;
	if (lhs->mode < rhs->mode)
		return -1;
	if (lhs->mode > rhs->mode)
		return 1;
	return 0;
}

static MAC_SHADOW_PRIVATE_MODE_CANDIDATE* mac_shadow_find_private_display_modes(
    CGDirectDisplayID displayId, UINT32 width, UINT32 height, BOOL exactOnly, size_t* count,
    int* currentMode)
{
	WINPR_ASSERT(count);
	WINPR_ASSERT(currentMode);
	*count = 0;
	*currentMode = -1;

	MAC_SHADOW_CGS_MODE current = WINPR_C_ARRAY_INIT;
	if (!mac_shadow_get_current_private_display_mode(displayId, currentMode, &current))
		return nullptr;

	int numberOfModes = 0;
	g_CgsApi.getNumberOfDisplayModes(displayId, &numberOfModes);
	if ((numberOfModes <= 0) || (numberOfModes > MAC_SHADOW_CGS_MAX_MODES))
	{
		WLog_WARN(TAG, "Sonoma scaled-display API returned invalid mode count %d", numberOfModes);
		return nullptr;
	}

	MAC_SHADOW_PRIVATE_MODE_CANDIDATE* candidates =
	    (MAC_SHADOW_PRIVATE_MODE_CANDIDATE*)calloc(
	        (size_t)numberOfModes, sizeof(MAC_SHADOW_PRIVATE_MODE_CANDIDATE));
	if (!candidates)
		return nullptr;

	const double requestedAspect = (double)width / height;
	for (int index = 0; index < numberOfModes; index++)
	{
		MAC_SHADOW_CGS_MODE mode = WINPR_C_ARRAY_INIT;
		if (!mac_shadow_get_private_display_mode(displayId, index, &mode))
			continue;
		if (exactOnly && ((mode.value.width != width) || (mode.value.height != height)))
			continue;
		/* A fallback may downscale, but must not enlarge a smaller physical framebuffer. */
		if (!exactOnly && ((mode.value.width < width) || (mode.value.height < height)))
			continue;

		MAC_SHADOW_PRIVATE_MODE_CANDIDATE* candidate = &candidates[*count];
		candidate->mode = (int)mode.value.mode;
		candidate->width = mode.value.width;
		candidate->height = mode.value.height;
		candidate->depth = mode.value.depth;
		candidate->frequency = mode.value.frequency;
		candidate->density = mode.value.density;
		candidate->depthMatch = mode.value.depth == current.value.depth;
		candidate->densityMatch = fabs(mode.value.density - current.value.density) <= FLT_EPSILON;
		candidate->refreshDifference =
		    ((mode.value.frequency == 0) || (current.value.frequency == 0))
		        ? 0.0
		        : fabs((double)mode.value.frequency - current.value.frequency);
		const double candidateAspect = (double)mode.value.width / mode.value.height;
		const double sizeDifference = fabs(log((double)mode.value.width / width)) +
		                              fabs(log((double)mode.value.height / height));
		candidate->score =
		    (20.0 * fabs(candidateAspect - requestedAspect)) + sizeDifference;
		(*count)++;
	}

	if (*count == 0)
	{
		free(candidates);
		return nullptr;
	}

	qsort(candidates, *count, sizeof(*candidates),
	      mac_shadow_compare_private_mode_candidates);
	return candidates;
}

static CGError mac_shadow_set_private_display_mode(CGDirectDisplayID displayId, int mode)
{
	if (!mac_shadow_load_cgs_api())
		return kCGErrorFailure;

	CGDisplayConfigRef config = nullptr;
	CGError error = CGBeginDisplayConfiguration(&config);
	if (error != kCGErrorSuccess)
		return error;

	g_CgsApi.configureDisplayMode(config, displayId, mode);
	return CGCompleteDisplayConfiguration(config, kCGConfigureForSession);
}

static BOOL mac_shadow_display_mode_is_retina(CGDisplayModeRef mode)
{
	if (!mode)
		return FALSE;

	const size_t width = CGDisplayModeGetWidth(mode);
	const size_t height = CGDisplayModeGetHeight(mode);
	if ((width == 0) || (height == 0))
		return FALSE;

	return (CGDisplayModeGetPixelWidth(mode) >= (width * 2)) &&
	       (CGDisplayModeGetPixelHeight(mode) >= (height * 2));
}

static BOOL mac_shadow_display_modes_equal(CGDisplayModeRef lhs, CGDisplayModeRef rhs)
{
	if (!lhs || !rhs)
		return FALSE;

	return (CGDisplayModeGetIODisplayModeID(lhs) == CGDisplayModeGetIODisplayModeID(rhs)) &&
	       (CGDisplayModeGetPixelWidth(lhs) == CGDisplayModeGetPixelWidth(rhs)) &&
	       (CGDisplayModeGetPixelHeight(lhs) == CGDisplayModeGetPixelHeight(rhs));
}

static CGDisplayModeRef mac_shadow_find_display_mode(CGDirectDisplayID displayId,
                                                     CGDisplayModeRef current, size_t width,
                                                     size_t height, BOOL exactOnly)
{
	const void* keys[] = { kCGDisplayShowDuplicateLowResolutionModes };
	const void* values[] = { kCFBooleanTrue };
	CFDictionaryRef options = CFDictionaryCreate(
	    kCFAllocatorDefault, keys, values, ARRAYSIZE(keys), &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	if (!options)
		return nullptr;

	CFArrayRef modes = CGDisplayCopyAllDisplayModes(displayId, options);
	CFRelease(options);
	if (!modes)
		return nullptr;

	CGDisplayModeRef best = nullptr;
	BOOL bestRetinaMatch = FALSE;
	double bestRefreshDifference = DBL_MAX;
	double bestScore = DBL_MAX;
	const BOOL currentRetina = mac_shadow_display_mode_is_retina(current);
	const double currentRefresh = current ? CGDisplayModeGetRefreshRate(current) : 0.0;
	const double requestedAspect = (double)width / height;

	for (CFIndex x = 0; x < CFArrayGetCount(modes); x++)
	{
		CGDisplayModeRef candidate =
		    (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, x);
		if (!candidate || !CGDisplayModeIsUsableForDesktopGUI(candidate))
		{
			continue;
		}

		const size_t candidateWidth = CGDisplayModeGetWidth(candidate);
		const size_t candidateHeight = CGDisplayModeGetHeight(candidate);
		if ((candidateWidth == 0) || (candidateHeight == 0))
			continue;
		if (exactOnly && ((candidateWidth != width) || (candidateHeight != height)))
			continue;
		/* A fallback may downscale, but never choose a physical mode that would require
		 * upscaling its framebuffer to satisfy the client. */
		if (!exactOnly && ((candidateWidth < width) || (candidateHeight < height)))
			continue;

		const BOOL retinaMatch = mac_shadow_display_mode_is_retina(candidate) == currentRetina;
		const double candidateRefresh = CGDisplayModeGetRefreshRate(candidate);
		const double refreshDifference =
		    ((currentRefresh <= 0.0) || (candidateRefresh <= 0.0))
		        ? 0.0
		        : fabs(candidateRefresh - currentRefresh);
		const double candidateAspect = (double)candidateWidth / candidateHeight;
		const double sizeDifference = fabs(log((double)candidateWidth / width)) +
		                              fabs(log((double)candidateHeight / height));
		const double score = (20.0 * fabs(candidateAspect - requestedAspect)) + sizeDifference;
		const BOOL scoreMatches = fabs(score - bestScore) <= DBL_EPSILON;
		if (!best || (score < bestScore) ||
		    (scoreMatches && retinaMatch && !bestRetinaMatch) ||
		    (scoreMatches && (retinaMatch == bestRetinaMatch) &&
		     (refreshDifference < bestRefreshDifference)))
		{
			if (best)
				CGDisplayModeRelease(best);
			best = (CGDisplayModeRef)CFRetain(candidate);
			bestRetinaMatch = retinaMatch;
			bestRefreshDifference = refreshDifference;
			bestScore = score;
		}
	}

	CFRelease(modes);
	return best;
}

static int mac_shadow_reconfigure_surface(macShadowSubsystem* subsystem, const char* transition)
{
	rdpShadowSurface* surface = nullptr;

	if (mac_shadow_detect_monitors(subsystem) < 0)
		return -1;

	if (!shadow_screen_resize(subsystem->common.server->screen))
	{
		WLog_ERR(TAG, "Failed to resize the shadow screen after %s", transition);
		return -1;
	}

	/* shadow_surface_resize preserves realloc'd bytes even when the row stride changes. Clear
	 * that old layout and force the new display stream's first complete frame; otherwise a
	 * legacy bitmap client can briefly render wrapped copies of the previous desktop. */
	surface = subsystem->common.server->surface;
	if (surface && surface->data && (surface->scanline > 0) &&
	    (surface->height <= (SIZE_MAX / surface->scanline)))
	{
		EnterCriticalSection(&surface->lock);
		memset(surface->data, 0, (size_t)surface->scanline * surface->height);
		region16_clear(&surface->invalidRegion);
		LeaveCriticalSection(&surface->lock);
	}
	subsystem->captureNeedsFullFrame = TRUE;

	WLog_INFO(TAG, "Shadow surface reconfigured to %dx%d after %s", subsystem->width,
	          subsystem->height, transition);
	return 1;
}

static int mac_shadow_use_scaled_client_surface(macShadowSubsystem* subsystem, UINT32 width,
                                                UINT32 height)
{
	WINPR_ASSERT(subsystem);
	const size_t sourceWidth = CGDisplayPixelsWide(CGMainDisplayID());
	const size_t sourceHeight = CGDisplayPixelsHigh(CGMainDisplayID());

	if ((width == 0) || (height == 0) || (width > INT_MAX) || (height > INT_MAX) ||
	    (width > UINT16_MAX) || (height > UINT16_MAX))
	{
		WLog_WARN(TAG, "Cannot create scaled RDP surface for invalid size %" PRIu32 "x%" PRIu32,
		          width, height);
		return 1;
	}
	if ((sourceWidth > 0) && (sourceHeight > 0) &&
	    ((width > sourceWidth) || (height > sourceHeight)))
	{
		WLog_WARN(TAG,
		          "Client request %" PRIu32 "x%" PRIu32
		          " is larger than main display %" PRIuz "x%" PRIuz
		          "; refusing to upscale the capture surface",
		          width, height, sourceWidth, sourceHeight);
		return 1;
	}

	subsystem->scaledClientSurface = TRUE;
	subsystem->scaledClientWidth = (int)width;
	subsystem->scaledClientHeight = (int)height;
	if (mac_shadow_reconfigure_surface(subsystem, "scaled client-resolution fallback") < 0)
	{
		subsystem->scaledClientSurface = FALSE;
		subsystem->scaledClientWidth = 0;
		subsystem->scaledClientHeight = 0;
		(void)mac_shadow_reconfigure_surface(subsystem, "failed scaled-surface rollback");
		return -1;
	}

	WLog_INFO(TAG,
	          "Using scaled RDP surface %" PRIu32 "x%" PRIu32
	          " for main display %dx%d; aspect ratio is preserved and mouse input is mapped",
	          width, height, subsystem->desktopWidth, subsystem->desktopHeight);
	return 1;
}

static int mac_shadow_switch_to_client_display_mode(macShadowSubsystem* subsystem,
                                                    const rdpSettings* settings)
{
	WINPR_ASSERT(subsystem);
	WINPR_ASSERT(settings);

	const UINT32 requestedWidth =
	    freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
	const UINT32 requestedHeight =
	    freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
	if ((requestedWidth == 0) || (requestedHeight == 0))
	{
		WLog_WARN(TAG, "Client advertised an invalid desktop size %" PRIu32 "x%" PRIu32,
		          requestedWidth, requestedHeight);
		return 1;
	}

	const CGDirectDisplayID displayId = CGMainDisplayID();
	CGDisplayModeRef current = CGDisplayCopyDisplayMode(displayId);
	if (!current)
	{
		WLog_WARN(TAG, "Could not read the current display mode; keeping the existing resolution");
		return 1;
	}

	CGDisplayModeRef target =
	    mac_shadow_find_display_mode(displayId, current, requestedWidth, requestedHeight, TRUE);
	if (target && mac_shadow_display_modes_equal(current, target))
	{
		WLog_INFO(TAG, "Main display already matches client request %" PRIu32 "x%" PRIu32,
		          requestedWidth, requestedHeight);
		CGDisplayModeRelease(target);
		CGDisplayModeRelease(current);
		return 1;
	}

	if (target)
	{
		if (mac_shadow_capture_release_stream(subsystem) < 0)
		{
			CGDisplayModeRelease(target);
			CGDisplayModeRelease(current);
			return -1;
		}

		WLog_INFO(TAG,
		          "Switching main display from %" PRIuz "x%" PRIuz " to %" PRIuz "x%" PRIuz
		          " for client request %" PRIu32 "x%" PRIu32,
		          CGDisplayModeGetWidth(current), CGDisplayModeGetHeight(current),
		          CGDisplayModeGetWidth(target), CGDisplayModeGetHeight(target), requestedWidth,
		          requestedHeight);
		const CGError error = CGDisplaySetDisplayMode(displayId, target, nullptr);
		CGDisplayModeRelease(target);
		if (error == kCGErrorSuccess)
		{
			subsystem->preConnectionDisplayMode = current;
			subsystem->connectionDisplayModeActive = TRUE;
			subsystem->scaledClientSurface = FALSE;
			subsystem->scaledClientWidth = 0;
			subsystem->scaledClientHeight = 0;
			if (mac_shadow_reconfigure_surface(subsystem,
			                                   "automatic client resolution switch") < 0)
			{
				(void)mac_shadow_restore_pre_connection_display_mode(
				    subsystem, "failed automatic client resolution switch");
				return -1;
			}
			return 1;
		}

		WLog_WARN(TAG,
		          "Public CGDisplaySetDisplayMode failed with status %" PRId32
		          "; trying Sonoma's complete display-mode list",
		          (INT32)error);
	}

	/* Sonoma's public CoreGraphics list omits scaled modes such as 1440x900. RDC 2.x advertises
	 * 1280x800, so consult the complete list only after the public exact-mode path above fails. */
	size_t candidateCount = 0;
	int originalPrivateMode = -1;
	BOOL scaledFallback = FALSE;
	MAC_SHADOW_PRIVATE_MODE_CANDIDATE* candidates = mac_shadow_find_private_display_modes(
	    displayId, requestedWidth, requestedHeight, TRUE, &candidateCount, &originalPrivateMode);
	if (!candidates)
	{
		candidates = mac_shadow_find_private_display_modes(displayId, requestedWidth,
		                                                    requestedHeight, FALSE,
		                                                    &candidateCount,
		                                                    &originalPrivateMode);
		scaledFallback = candidates != nullptr;
	}

	if (!candidates)
	{
		WLog_WARN(TAG,
		          "No activatable display mode can supply client request %" PRIu32 "x%" PRIu32
		          "; using a scaled RDP surface over %" PRIuz "x%" PRIuz,
		          requestedWidth, requestedHeight, CGDisplayModeGetWidth(current),
		          CGDisplayModeGetHeight(current));
		CGDisplayModeRelease(current);
		return mac_shadow_use_scaled_client_surface(subsystem, requestedWidth, requestedHeight);
	}

	if (mac_shadow_capture_release_stream(subsystem) < 0)
	{
		free(candidates);
		CGDisplayModeRelease(current);
		return -1;
	}

	const size_t originalWidth = CGDisplayPixelsWide(displayId);
	const size_t originalHeight = CGDisplayPixelsHigh(displayId);
	const MAC_SHADOW_PRIVATE_MODE_CANDIDATE* selected = nullptr;
	for (size_t index = 0; index < candidateCount; index++)
	{
		const MAC_SHADOW_PRIVATE_MODE_CANDIDATE* candidate = &candidates[index];
		if (candidate->mode == originalPrivateMode)
		{
			selected = candidate;
			break;
		}

		WLog_INFO(TAG,
		          "Trying Sonoma display mode %d (%" PRIu32 "x%" PRIu32 "@%" PRIu16
		          ", depth=%" PRIu32 ") for client %" PRIu32 "x%" PRIu32,
		          candidate->mode, candidate->width, candidate->height, candidate->frequency,
		          candidate->depth, requestedWidth, requestedHeight);
		const CGError error = mac_shadow_set_private_display_mode(displayId, candidate->mode);
		const size_t actualWidth = CGDisplayPixelsWide(displayId);
		const size_t actualHeight = CGDisplayPixelsHigh(displayId);
		if ((error == kCGErrorSuccess) && (actualWidth == candidate->width) &&
		    (actualHeight == candidate->height))
		{
			selected = candidate;
			break;
		}

		WLog_WARN(TAG,
		          "Sonoma mode %d did not activate (status=%" PRId32 ", actual=%" PRIuz "x%" PRIuz
		          "); trying the next compatible mode",
		          candidate->mode, (INT32)error, actualWidth, actualHeight);
		if ((actualWidth != originalWidth) || (actualHeight != originalHeight))
			(void)mac_shadow_set_private_display_mode(displayId, originalPrivateMode);
	}

	if (!selected)
	{
		WLog_WARN(TAG,
		          "All compatible Sonoma display modes failed for client %" PRIu32 "x%" PRIu32
		          "; using the current display as the scaled source",
		          requestedWidth, requestedHeight);
		free(candidates);
		CGDisplayModeRelease(current);
		return mac_shadow_use_scaled_client_surface(subsystem, requestedWidth, requestedHeight);
	}

	const BOOL changedMode = selected->mode != originalPrivateMode;
	const UINT32 selectedWidth = selected->width;
	const UINT32 selectedHeight = selected->height;
	const int selectedMode = selected->mode;
	free(candidates);

	if (changedMode)
	{
		subsystem->preConnectionPrivateDisplayMode = originalPrivateMode;
		subsystem->privateDisplayModeActive = TRUE;
		subsystem->connectionDisplayModeActive = TRUE;
		WLog_WARN(TAG,
		          "%s client request %" PRIu32 "x%" PRIu32
		          "; switched main display from %" PRIuz "x%" PRIuz " to source %" PRIu32
		          "x%" PRIu32 " (Sonoma mode %d)",
		          scaledFallback ? "No exact mode matches" : "Matched", requestedWidth,
		          requestedHeight, originalWidth, originalHeight, selectedWidth, selectedHeight,
		          selectedMode);
	}
	else if (scaledFallback)
	{
		WLog_WARN(TAG,
		          "No exact mode matches client request %" PRIu32 "x%" PRIu32
		          "; current %" PRIu32 "x%" PRIu32 " mode is the closest activatable source",
		          requestedWidth, requestedHeight, selectedWidth, selectedHeight);
	}
	CGDisplayModeRelease(current);

	if (scaledFallback)
	{
		if (mac_shadow_use_scaled_client_surface(subsystem, requestedWidth, requestedHeight) < 0)
		{
			(void)mac_shadow_restore_pre_connection_display_mode(
			    subsystem, "failed closest-mode scaled surface");
			return -1;
		}
		return 1;
	}

	subsystem->scaledClientSurface = FALSE;
	subsystem->scaledClientWidth = 0;
	subsystem->scaledClientHeight = 0;
	if (mac_shadow_reconfigure_surface(subsystem, "automatic Sonoma client resolution switch") <
	    0)
	{
		(void)mac_shadow_restore_pre_connection_display_mode(
		    subsystem, "failed automatic Sonoma client resolution switch");
		return -1;
	}
	return 1;
}

static int mac_shadow_restore_pre_connection_display_mode(macShadowSubsystem* subsystem,
                                                          const char* transition)
{
	WINPR_ASSERT(subsystem);
	WINPR_ASSERT(transition);

	if (subsystem->privateDisplayModeActive)
	{
		if (mac_shadow_capture_release_stream(subsystem) < 0)
			return -1;

		const CGDirectDisplayID displayId = CGMainDisplayID();
		int currentMode = -1;
		CGError error = kCGErrorSuccess;
		if (!mac_shadow_get_current_private_display_mode(displayId, &currentMode, nullptr) ||
		    (currentMode != subsystem->preConnectionPrivateDisplayMode))
		{
			error = mac_shadow_set_private_display_mode(
			    displayId, subsystem->preConnectionPrivateDisplayMode);
		}
		int restoredMode = -1;
		if ((error != kCGErrorSuccess) ||
		    !mac_shadow_get_current_private_display_mode(displayId, &restoredMode, nullptr) ||
		    (restoredMode != subsystem->preConnectionPrivateDisplayMode))
		{
			WLog_ERR(TAG,
			         "Failed to restore Sonoma display mode %d while %s (status=%" PRId32
			         ", actual mode=%d)",
			         subsystem->preConnectionPrivateDisplayMode, transition, (INT32)error,
			         restoredMode);
			return -1;
		}

		subsystem->preConnectionPrivateDisplayMode = -1;
		subsystem->privateDisplayModeActive = FALSE;
		subsystem->connectionDisplayModeActive = FALSE;
		subsystem->scaledClientSurface = FALSE;
		subsystem->scaledClientWidth = 0;
		subsystem->scaledClientHeight = 0;
		return mac_shadow_reconfigure_surface(subsystem, transition);
	}

	if (!subsystem->preConnectionDisplayMode)
	{
		if (!subsystem->scaledClientSurface)
			return 1;

		subsystem->scaledClientSurface = FALSE;
		subsystem->scaledClientWidth = 0;
		subsystem->scaledClientHeight = 0;
		return mac_shadow_reconfigure_surface(subsystem, transition);
	}

	if (mac_shadow_capture_release_stream(subsystem) < 0)
		return -1;

	const CGDirectDisplayID displayId = CGMainDisplayID();
	CGDisplayModeRef current = CGDisplayCopyDisplayMode(displayId);
	CGError error = kCGErrorSuccess;
	if (!current ||
	    !mac_shadow_display_modes_equal(current, subsystem->preConnectionDisplayMode))
	{
		error = CGDisplaySetDisplayMode(displayId, subsystem->preConnectionDisplayMode, nullptr);
	}
	if (current)
		CGDisplayModeRelease(current);
	if (error != kCGErrorSuccess)
	{
		WLog_ERR(TAG, "CGDisplaySetDisplayMode failed while restoring %s with status %" PRId32,
		         transition, (INT32)error);
		return -1;
	}

	CGDisplayModeRelease(subsystem->preConnectionDisplayMode);
	subsystem->preConnectionDisplayMode = nullptr;
	subsystem->connectionDisplayModeActive = FALSE;
	subsystem->scaledClientSurface = FALSE;
	subsystem->scaledClientWidth = 0;
	subsystem->scaledClientHeight = 0;
	return mac_shadow_reconfigure_surface(subsystem, transition);
}

static int mac_shadow_restore_connection_display_mode(macShadowSubsystem* subsystem,
                                                      const char* transition)
{
	WINPR_ASSERT(subsystem);
	WINPR_ASSERT(transition);

	if (subsystem->disconnectDisplayCommand)
	{
		if (!subsystem->connectionDisplayModeActive)
			return 1;

		const int status = mac_shadow_switch_display_mode(
		    subsystem, subsystem->disconnectDisplayCommand, transition);
		if (status > 0)
			subsystem->connectionDisplayModeActive = FALSE;
		return status;
	}

	return mac_shadow_restore_pre_connection_display_mode(subsystem, transition);
}

static int mac_shadow_switch_display_mode(macShadowSubsystem* subsystem, const char* command,
	                                      const char* transition)
{
	BOOL commandSucceeded = FALSE;

	if (!subsystem || !subsystem->common.server || !subsystem->common.server->screen)
		return -1;

	if (mac_shadow_capture_release_stream(subsystem) < 0)
		return -1;

	commandSucceeded = mac_shadow_run_display_command(command, transition) > 0;
	if (mac_shadow_reconfigure_surface(subsystem, transition) < 0)
		return -1;
	return commandSucceeded ? 1 : -1;
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
	const char* testTone = getenv("FREERDP_MAC_SHADOW_TEST_TONE");
	const char* autoClientProfile = getenv("FREERDP_MAC_SHADOW_AUTO_CLIENT_PROFILE");
	g_Subsystem = subsystem;
	subsystem->testToneEnabled = testTone && (strcmp(testTone, "0") != 0);
	subsystem->autoClientProfile =
	    autoClientProfile && (strcmp(autoClientProfile, "0") != 0);
	subsystem->configuredShowMouseCursor = subsystem->common.server->ShowMouseCursor;
	subsystem->connectDisplayCommand = getenv("FREERDP_MAC_SHADOW_CONNECT_DISPLAY_COMMAND");
	subsystem->disconnectDisplayCommand = getenv("FREERDP_MAC_SHADOW_DISCONNECT_DISPLAY_COMMAND");
	if (subsystem->connectDisplayCommand && (subsystem->connectDisplayCommand[0] == '\0'))
		subsystem->connectDisplayCommand = nullptr;
	if (subsystem->disconnectDisplayCommand && (subsystem->disconnectDisplayCommand[0] == '\0'))
		subsystem->disconnectDisplayCommand = nullptr;

	if ((subsystem->connectDisplayCommand == nullptr) !=
	    (subsystem->disconnectDisplayCommand == nullptr))
	{
		WLog_ERR(TAG,
		         "Both FREERDP_MAC_SHADOW_CONNECT_DISPLAY_COMMAND and "
		         "FREERDP_MAC_SHADOW_DISCONNECT_DISPLAY_COMMAND must be configured together");
		return -1;
	}

	if (subsystem->autoClientProfile)
	{
		WLog_INFO(TAG,
		          "Automatic client profiles are enabled (requested resolution and per-client "
		          "cursor policy)");
	}
	if (subsystem->connectDisplayCommand)
	{
		WLog_INFO(TAG,
		          "External display-mode commands are enabled and override native automatic "
		          "resolution switching");
	}

	if (mac_shadow_detect_monitors(subsystem) < 0)
		return -1;

	return 1;
}

static int mac_shadow_subsystem_uninit(rdpShadowSubsystem* rdpsubsystem)
{
	macShadowSubsystem* subsystem = (macShadowSubsystem*)rdpsubsystem;
	if (!subsystem)
		return -1;

	int status = mac_shadow_capture_release_stream(subsystem);
	if (mac_shadow_restore_connection_display_mode(subsystem, "server shutdown") < 0)
		status = -1;
	subsystem->common.server->ShowMouseCursor = subsystem->configuredShowMouseCursor;
	return status;
}

static int mac_shadow_subsystem_start(rdpShadowSubsystem* rdpsubsystem)
{
	macShadowSubsystem* subsystem = (macShadowSubsystem*)rdpsubsystem;
	HANDLE thread;

	if (!subsystem)
		return -1;

	if (!(thread =
	          CreateThread(nullptr, 0, mac_shadow_subsystem_thread, (void*)subsystem, 0, nullptr)))
	{
		WLog_ERR(TAG, "Failed to create thread");
		return -1;
	}

	WLog_INFO(TAG, "Display capture is idle and will start when the first client connects");

	return 1;
}

static int mac_shadow_subsystem_stop(rdpShadowSubsystem* rdpsubsystem)
{
	macShadowSubsystem* subsystem = (macShadowSubsystem*)rdpsubsystem;
	int status;

	if (!subsystem)
		return -1;

	EnterCriticalSection(&subsystem->connectionLock);
	mac_shadow_test_tone_stop(subsystem);
	mac_shadow_system_audio_stop(subsystem);
	status = mac_shadow_capture_release_stream(subsystem);
	if (mac_shadow_restore_connection_display_mode(subsystem, "server stop") < 0)
		status = -1;
	subsystem->common.server->ShowMouseCursor = subsystem->configuredShowMouseCursor;
	LeaveCriticalSection(&subsystem->connectionLock);
	return status;
}

static void mac_shadow_subsystem_free(rdpShadowSubsystem* subsystem)
{
	if (!subsystem)
		return;

	macShadowSubsystem* mac = (macShadowSubsystem*)subsystem;
	mac_shadow_test_tone_stop(mac);
	mac_shadow_subsystem_uninit(subsystem);
	mac_shadow_audio_free(mac->audioCapture);
	mac->audioCapture = nullptr;
	if (mac->eventSource)
		CFRelease(mac->eventSource);
	DeleteCriticalSection(&mac->connectionLock);

	free(subsystem);
}

static rdpShadowSubsystem* mac_shadow_subsystem_new(void)
{
	macShadowSubsystem* subsystem = calloc(1, sizeof(macShadowSubsystem));

	if (!subsystem)
		return nullptr;
	subsystem->preConnectionPrivateDisplayMode = -1;

	subsystem->eventSource = CGEventSourceCreate(kCGEventSourceStatePrivate);
	if (!subsystem->eventSource)
	{
		free(subsystem);
		return nullptr;
	}
	if (!InitializeCriticalSectionAndSpinCount(&subsystem->connectionLock, 4000))
	{
		CFRelease(subsystem->eventSource);
		free(subsystem);
		return nullptr;
	}

	subsystem->common.SynchronizeEvent = mac_shadow_input_synchronize_event;
	subsystem->common.ClientConnect = mac_shadow_client_connect;
	subsystem->common.ClientDisconnect = mac_shadow_client_disconnect;
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
