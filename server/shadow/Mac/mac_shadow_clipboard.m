#import <AppKit/AppKit.h>

#include <freerdp/channels/cliprdr.h>
#include <freerdp/log.h>

#include "mac_shadow_clipboard.h"

#define TAG SERVER_TAG("shadow.mac.cliprdr")

typedef struct
{
	rdpShadowClient* client;
	dispatch_queue_t queue;
	dispatch_source_t timer;
	NSInteger changeCount;
	BOOL stopped;
} MacShadowClipboard;

static UINT format_list(CliprdrServerContext* context)
{
	CLIPRDR_FORMAT format = { CF_UNICODETEXT, nullptr };
	CLIPRDR_FORMAT_LIST list = { .common = { .msgType = CB_FORMAT_LIST }, .numFormats = 1,
		.formats = &format };
	return context->ServerFormatList(context, &list);
}

static UINT format_list_response(CliprdrServerContext* context, BOOL ok)
{
	CLIPRDR_FORMAT_LIST_RESPONSE response = { .common = { .msgType = CB_FORMAT_LIST_RESPONSE,
		.msgFlags = ok ? CB_RESPONSE_OK : CB_RESPONSE_FAIL } };
	return context->ServerFormatListResponse(context, &response);
}

static UINT monitor_ready(CliprdrServerContext* context,
	WINPR_ATTR_UNUSED const CLIPRDR_MONITOR_READY* ready)
{
	CLIPRDR_GENERAL_CAPABILITY_SET general = { CB_CAPSTYPE_GENERAL, CB_CAPSTYPE_GENERAL_LEN,
		CB_CAPS_VERSION_2, CB_USE_LONG_FORMAT_NAMES };
	CLIPRDR_CAPABILITIES caps = { .cCapabilitiesSets = 1,
		.capabilitySets = (CLIPRDR_CAPABILITY_SET*)&general };
	const UINT rc = context->ServerCapabilities(context, &caps);
	return (rc == CHANNEL_RC_OK) ? format_list(context) : rc;
}

static UINT client_format_list(CliprdrServerContext* context, const CLIPRDR_FORMAT_LIST* list)
{
	UINT32 format = 0;
	for (UINT32 x = 0; x < list->numFormats; x++)
	{
		const UINT32 id = list->formats[x].formatId;
		if (id == CF_UNICODETEXT) { format = id; break; }
		if ((format == 0) && ((id == CF_TEXT) || (id == CF_OEMTEXT))) format = id;
	}
	const UINT rc = format_list_response(context, format != 0);
	if ((rc != CHANNEL_RC_OK) || (format == 0)) return rc;
	CLIPRDR_FORMAT_DATA_REQUEST request = { .common = { .msgType = CB_FORMAT_DATA_REQUEST },
		.requestedFormatId = format };
	return context->ServerFormatDataRequest(context, &request);
}

static UINT client_format_data_response(CliprdrServerContext* context,
	const CLIPRDR_FORMAT_DATA_RESPONSE* response)
{
	MacShadowClipboard* state = (MacShadowClipboard*)context->custom;
	if (!state || state->stopped || (response->common.msgFlags & CB_RESPONSE_FAIL) ||
	    !response->requestedFormatData) return CHANNEL_RC_OK;
	@autoreleasepool {
		NSString* text = [[NSString alloc] initWithBytes:response->requestedFormatData
			length:response->common.dataLen encoding:NSUTF16LittleEndianStringEncoding];
		if (!text) text = [[NSString alloc] initWithBytes:response->requestedFormatData
			length:response->common.dataLen encoding:NSUTF8StringEncoding];
		if (!text) return ERROR_INVALID_DATA;
		NSPasteboard* pb = [NSPasteboard generalPasteboard];
		[pb clearContents]; [pb setString:text forType:NSPasteboardTypeString];
		state->changeCount = pb.changeCount;
	}
	return CHANNEL_RC_OK;
}

static UINT server_format_data_request(CliprdrServerContext* context,
	const CLIPRDR_FORMAT_DATA_REQUEST* request)
{
	CLIPRDR_FORMAT_DATA_RESPONSE response = { .common = { .msgType = CB_FORMAT_DATA_RESPONSE,
		.msgFlags = CB_RESPONSE_FAIL } };
	@autoreleasepool {
		NSString* text = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
		NSData* data = nil;
		if (text && (request->requestedFormatId == CF_UNICODETEXT))
			data = [[text stringByAppendingString:@"\0"] dataUsingEncoding:NSUTF16LittleEndianStringEncoding];
		else if (text && ((request->requestedFormatId == CF_TEXT) || (request->requestedFormatId == CF_OEMTEXT)))
			data = [[text stringByAppendingString:@"\0"] dataUsingEncoding:NSUTF8StringEncoding];
		if (data) { response.common.msgFlags = CB_RESPONSE_OK; response.common.dataLen = (UINT32)data.length; response.requestedFormatData = data.bytes; }
		return context->ServerFormatDataResponse(context, &response);
	}
}

static void poll_pasteboard(void* arg)
{
	MacShadowClipboard* state = (MacShadowClipboard*)arg;
	if (!state || state->stopped || !state->client || !state->client->cliprdr) return;
	@autoreleasepool {
		const NSInteger changed = [NSPasteboard generalPasteboard].changeCount;
		if (changed != state->changeCount) { state->changeCount = changed; (void)format_list(state->client->cliprdr); }
	}
}

int mac_shadow_clipboard_init(rdpShadowClient* client)
{
	if (!client) return -1;
	CliprdrServerContext* context = client->cliprdr = cliprdr_server_context_new(client->vcm);
	if (!context) return -1;
	MacShadowClipboard* state = calloc(1, sizeof(*state));
	if (!state) { cliprdr_server_context_free(context); client->cliprdr = nullptr; return -1; }
	state->client = client; state->queue = dispatch_queue_create("mac.shadow.clipboard", DISPATCH_QUEUE_SERIAL);
	@autoreleasepool { state->changeCount = [NSPasteboard generalPasteboard].changeCount; }
	context->custom = state; context->MonitorReady = monitor_ready;
	context->ClientFormatList = client_format_list; context->ClientFormatDataResponse = client_format_data_response;
	context->ServerFormatDataRequest = server_format_data_request;
	if (context->Start(context) != CHANNEL_RC_OK) { free(state); cliprdr_server_context_free(context); client->cliprdr = nullptr; return -1; }
	state->timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, state->queue);
	dispatch_source_set_timer(state->timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), NSEC_PER_SEC, NSEC_PER_MSEC * 100);
	dispatch_set_context(state->timer, state); dispatch_source_set_event_handler_f(state->timer, poll_pasteboard); dispatch_resume(state->timer);
	return 1;
}

void mac_shadow_clipboard_uninit(rdpShadowClient* client)
{
	if (!client || !client->cliprdr) return;
	CliprdrServerContext* context = client->cliprdr; MacShadowClipboard* state = (MacShadowClipboard*)context->custom;
	if (state) { state->stopped = TRUE; if (state->timer) dispatch_source_cancel(state->timer); if (state->queue) dispatch_sync(state->queue, ^{}); free(state); }
	context->custom = nullptr; context->Stop(context); cliprdr_server_context_free(context); client->cliprdr = nullptr;
}
