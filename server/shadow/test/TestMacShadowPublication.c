/** FreeRDP shadow regression tests. Licensed under the Apache License, Version 2.0. */
/* Exercise the private publication/lifecycle implementation without starting a display stream,
 * opening a listener, changing display modes, capturing audio, or posting input events. */
#include "../Mac/mac_shadow.c"
#include "../shadow_mcevent.h"

#define CHECK(condition)                                                    \
	do                                                                      \
	{                                                                       \
		if (!(condition))                                                   \
		{                                                                   \
			fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); \
			exit(1);                                                        \
		}                                                                   \
	} while (0)

static DWORD WINAPI publish_frame(void* arg)
{
	return mac_shadow_publish_pending(arg, FALSE) ? 0 : 1;
}

static DWORD WINAPI capture_newer(void* arg)
{
	macShadowSubsystem* mac = arg;
	rdpShadowSurface* latest = mac->captureSurface;
	const RECTANGLE_16 corner = { 63, 63, 64, 64 };
	EnterCriticalSection(&latest->lock);
	latest->data[(size_t)63 * latest->scanline + 63 * 4U] = 77;
	CHECK(region16_union_rect(&latest->invalidRegion, &latest->invalidRegion, &corner));
	CHECK(SetEvent(mac->frameEvent));
	LeaveCriticalSection(&latest->lock);
	return 0;
}

static void test_client_sizes(void)
{
	const UINT32 sizes[][2] = { {800,600}, {1024,768}, {1280,720}, {1280,800},
	    {1440,900}, {1680,1050}, {1920,1080}, {2560,1600}, {2880,1800} };
	rdpSettings* settings = freerdp_settings_new(0);
	CHECK(settings);
	CHECK(freerdp_settings_set_string(settings, FreeRDP_ClientHostname, "VaIo"));
	for (size_t n = 0; n < ARRAYSIZE(sizes); n++)
	{
		UINT32 width = 0, height = 0;
		CHECK(freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, sizes[n][0]));
		CHECK(freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, sizes[n][1]));
		mac_shadow_client_size(settings, &width, &height);
		CHECK(width == sizes[n][0] && height == sizes[n][1]);
		CHECK(mac_shadow_valid_client_size(width, height));
	}
	CHECK(freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, 0));
	UINT32 width = 0, height = 0;
	mac_shadow_client_size(settings, &width, &height);
	CHECK(width == 1024 && height == 768);
	CHECK(!mac_shadow_valid_client_size(0, 768));
	CHECK(!mac_shadow_valid_client_size(65535, 65535));
	CHECK(!mac_shadow_valid_client_size(8192, 8192));
	CHECK(mac_shadow_is_vaio_profile(settings));
	CHECK(freerdp_settings_set_string(settings, FreeRDP_ClientHostname, "android-phone"));
	CHECK(!mac_shadow_is_vaio_profile(settings));
	CHECK(freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, 1440));
	CHECK(freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, 900));
	mac_shadow_client_size(settings, &width, &height);
	CHECK(width == 1440 && height == 900);
	freerdp_settings_free(settings);
}

static void test_mouse_button_transitions(void)
{
	macShadowSubsystem mac = { 0 };
	CGEventType type = kCGEventNull;
	CGMouseButton button = kCGMouseButtonLeft;
	BOOL* pressed = nullptr;
	BOOL down = FALSE;
	CHECK(mac_shadow_mouse_button_transition(&mac, PTR_FLAGS_BUTTON1 | PTR_FLAGS_DOWN, &type,
	                                         &button, &pressed, &down));
	CHECK(type == kCGEventLeftMouseDown && button == kCGMouseButtonLeft && down &&
	      pressed == &mac.mouseDownLeft);
	*pressed = down;
	CHECK(!mac_shadow_mouse_button_transition(&mac, PTR_FLAGS_BUTTON1 | PTR_FLAGS_DOWN, &type,
	                                          &button, &pressed, &down));
	CHECK(mac_shadow_mouse_button_transition(&mac, PTR_FLAGS_BUTTON1, &type, &button, &pressed,
	                                         &down));
	CHECK(type == kCGEventLeftMouseUp && !down);
	*pressed = down;
	CHECK(!mac_shadow_mouse_button_transition(&mac, PTR_FLAGS_BUTTON1, &type, &button, &pressed,
	                                          &down));
	CHECK(mac_shadow_mouse_button_transition(&mac, PTR_FLAGS_BUTTON2 | PTR_FLAGS_DOWN, &type,
	                                         &button, &pressed, &down));
	CHECK(type == kCGEventRightMouseDown && button == kCGMouseButtonRight);
	CHECK(!mac_shadow_mouse_button_transition(&mac, PTR_FLAGS_MOVE, &type, &button, &pressed,
	                                          &down));
}

int main(void)
{
	test_client_sizes();
	test_mouse_button_transitions();
	rdpShadowServer server = { 0 };
	macShadowSubsystem* mac = calloc(1, sizeof(*mac));
	CHECK(mac);
	CHECK(InitializeCriticalSectionAndSpinCount(&mac->publicationLock, 4000));
	CHECK(InitializeCriticalSectionAndSpinCount(&mac->connectionLock, 4000));
	mac->stopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
	mac->frameEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
	CHECK(mac->stopEvent && mac->frameEvent);
	mac->common.server = &server;
	server.clients = ArrayList_New(TRUE);
	server.surface = mac_shadow_latest_surface_new(64, 64);
	mac->captureSurface = mac_shadow_latest_surface_new(64, 64);
	mac->common.updateEvent = shadow_multiclient_new();
	mac->common.MsgPipe = MessagePipe_New();
	CHECK(server.clients && server.surface && mac->captureSurface && mac->common.updateEvent &&
	      mac->common.MsgPipe);
	/* A temporarily unavailable mode API must preserve restoration ownership and prevent
	 * a reconnect from replacing the original mode. No display mode is changed here. */
	MAC_SHADOW_CGS_API savedApi = g_CgsApi;
	g_CgsApi.loadAttempted = TRUE;
	g_CgsApi.handle = nullptr;
	mac->privateDisplayModeActive = TRUE;
	mac->connectionDisplayModeActive = TRUE;
	mac->preConnectionPrivateDisplayMode = 42;
	rdpShadowClient reconnect = { 0 };
	reconnect.context.settings = freerdp_settings_new(0);
	CHECK(reconnect.context.settings);
	CHECK(!mac_shadow_client_connect(&mac->common, &reconnect));
	CHECK(mac->privateDisplayModeActive && mac->connectionDisplayModeActive);
	CHECK(mac->preConnectionPrivateDisplayMode == 42 && mac->connectedClients == 0);
	freerdp_settings_free(reconnect.context.settings);
	mac->privateDisplayModeActive = FALSE;
	mac->connectionDisplayModeActive = FALSE;
	mac->preConnectionPrivateDisplayMode = -1;
	g_CgsApi = savedApi;
	mac->captureSurface = mac_shadow_latest_surface_new(64, 64);
	CHECK(mac->captureSurface);
	void* subscriber = shadow_multiclient_get_subscriber(mac->common.updateEvent);
	CHECK(subscriber);
	HANDLE update = shadow_multiclient_getevent(subscriber);
	const RECTANGLE_16 full = { 0, 0, 64, 64 };
	mac->captureSurface->data[0] = 33;
	CHECK(region16_union_rect(&mac->captureSurface->invalidRegion,
	                          &mac->captureSurface->invalidRegion, &full));
	HANDLE publisher = CreateThread(nullptr, 0, publish_frame, mac, 0, nullptr);
	CHECK(publisher);
	CHECK(WaitForSingleObject(update, 2000) == WAIT_OBJECT_0);
	CHECK(server.surface->data[0] == 33);
	/* The subscriber deliberately stalls. Capture must still finish, and the published
	 * surface must not change until the subscriber releases it. */
	HANDLE capture = CreateThread(nullptr, 0, capture_newer, mac, 0, nullptr);
	CHECK(capture);
	CHECK(WaitForSingleObject(capture, 2000) == WAIT_OBJECT_0);
	CHECK(CloseHandle(capture));
	CHECK(server.surface->data[(size_t)63 * server.surface->scanline + 63 * 4U] == 0);
	CHECK(shadow_multiclient_consume(subscriber));
	CHECK(WaitForSingleObject(publisher, 2000) == WAIT_OBJECT_0);
	CHECK(CloseHandle(publisher));
	CHECK(WaitForSingleObject(mac->frameEvent, 0) == WAIT_OBJECT_0);
	publisher = CreateThread(nullptr, 0, publish_frame, mac, 0, nullptr);
	CHECK(publisher);
	CHECK(WaitForSingleObject(update, 2000) == WAIT_OBJECT_0);
	CHECK(server.surface->data[0] == 33);
	CHECK(server.surface->data[(size_t)63 * server.surface->scanline + 63 * 4U] == 77);
	CHECK(shadow_multiclient_consume(subscriber));
	CHECK(WaitForSingleObject(publisher, 2000) == WAIT_OBJECT_0);
	CHECK(CloseHandle(publisher));
	CHECK(WaitForSingleObject(mac->frameEvent, 0) == WAIT_TIMEOUT);
	shadow_multiclient_release_subscriber(subscriber);
	/* Stop joins the message worker before generic teardown can release its queue. */
	for (UINT32 n = 0; n < 25; n++)
	{
		CHECK(mac_shadow_subsystem_start(&mac->common) > 0);
		CHECK(mac_shadow_subsystem_start(&mac->common) > 0);
		CHECK(mac_shadow_subsystem_stop(&mac->common) > 0);
		CHECK(!mac->worker && !mac->captureSurface);
		CHECK(mac_shadow_subsystem_stop(&mac->common) > 0);
	}
	shadow_multiclient_free(mac->common.updateEvent);
	MessagePipe_Free(mac->common.MsgPipe);
	mac->common.updateEvent = nullptr;
	mac->common.MsgPipe = nullptr;
	mac_shadow_subsystem_free(&mac->common);
	mac_shadow_latest_surface_free(server.surface);
	ArrayList_Free(server.clients);
	puts("Capture remains writable during publication; newest damage and 25 worker cycles passed");
	return 0;
}
