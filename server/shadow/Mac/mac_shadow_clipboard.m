#import <AppKit/AppKit.h>

#include <freerdp/channels/cliprdr.h>
#include <freerdp/log.h>
#include <winpr/sysinfo.h>

#include "mac_shadow_clipboard.h"

#define TAG SERVER_TAG("shadow.mac.cliprdr")
#define WLog_DEBUG(_tag, ...) WLog_Print_tag((_tag), WLOG_DEBUG, __VA_ARGS__)
#define MAC_SHADOW_CLIPBOARD_RESPONSE_TIMEOUT_MS 5000U
#define MAC_SHADOW_CLIPBOARD_MAX_TEXT_BYTES (8U * 1024U * 1024U)

typedef UINT64 (*MacShadowClipboardClock)(void* context);

typedef struct
{
	rdpShadowClient* client;
	dispatch_queue_t queue;
	dispatch_source_t timer;
	NSInteger changeCount;
	UINT32 requestedClientFormat;
	UINT32 requestedTextFormat;
	UINT32 pendingClientFormat;
	UINT32 pendingTextFormat;
	BOOL monitorReady;
	BOOL legacyRdp52;
	BOOL stopped;
	BOOL requestQuarantined;
	UINT64 requestDeadlineMs;
	UINT32 requestTimeouts;
	UINT32 lateResponses;
	MacShadowClipboardClock clock;
	void* clockContext;
	psCliprdrServerCapabilities sendServerCapabilities;
	psCliprdrMonitorReady sendMonitorReady;
} MacShadowClipboard;

static NSStringEncoding clipboard_encoding(UINT32 formatId)
{
	if (formatId == CF_MAX)
		return NSUTF8StringEncoding;
	if (formatId == CF_TEXT)
		return NSWindowsCP1252StringEncoding;
	if (formatId == CF_OEMTEXT)
		return CFStringConvertEncodingToNSStringEncoding(kCFStringEncodingDOSLatinUS);
	return NSUTF16LittleEndianStringEncoding;
}

static NSString* clipboard_normalize_from_rdp(NSString* text)
{
	text = [text stringByReplacingOccurrencesOfString:@"\r\n" withString:@"\n"];
	return [text stringByReplacingOccurrencesOfString:@"\r" withString:@"\n"];
}

static NSString* clipboard_normalize_to_rdp(NSString* text)
{
	return [[clipboard_normalize_from_rdp(text)
	         stringByReplacingOccurrencesOfString:@"\n" withString:@"\r\n"]
	        stringByAppendingString:@"\0"];
}

static UINT format_list(CliprdrServerContext* context)
{
	MacShadowClipboard* state = (MacShadowClipboard*)context->custom;
	/* RDP 5.2's cliprdr only understands the original text offer.  In
	 * particular, it does not complete the v2-capability/Unicode-first exchange.
	 * Keep that dialect scoped to its connection fingerprint; every other client
	 * keeps the richer Unicode-first list. */
	CLIPRDR_FORMAT legacyFormats[] = { { CF_TEXT, nullptr } };
	CLIPRDR_FORMAT modernFormats[] = { { CF_UNICODETEXT, nullptr },
		                              { CF_TEXT, nullptr },
		                              { CF_OEMTEXT, nullptr } };
	CLIPRDR_FORMAT* formats = (state && state->legacyRdp52) ? legacyFormats : modernFormats;
	const UINT32 numFormats = (state && state->legacyRdp52) ? ARRAYSIZE(legacyFormats)
	                                                        : ARRAYSIZE(modernFormats);
	CLIPRDR_FORMAT_LIST list = { .common = { .msgType = CB_FORMAT_LIST,
	                                        .msgFlags = 0,
	                                        .dataLen = numFormats * 36 },
	                         .numFormats = numFormats,
	                         .formats = formats };
	WLog_DEBUG(TAG, "Sending server clipboard Format List (%s)",
	          (state && state->legacyRdp52) ? "RDP 5.2 ANSI text" : "Unicode, ANSI, OEM text");
	return context->ServerFormatList(context, &list);
}

static UINT format_list_response(CliprdrServerContext* context, BOOL ok)
{
	CLIPRDR_FORMAT_LIST_RESPONSE response = {
		.common = { .msgType = CB_FORMAT_LIST_RESPONSE,
		            .msgFlags = ok ? CB_RESPONSE_OK : CB_RESPONSE_FAIL,
		            .dataLen = 0 }
	};
	WLog_DEBUG(TAG, "Sending clipboard Format List Response (%s)", ok ? "OK" : "FAIL");
	return context->ServerFormatListResponse(context, &response);
}

static UINT server_capabilities(CliprdrServerContext* context,
	                            const CLIPRDR_CAPABILITIES* capabilities)
{
	MacShadowClipboard* state = (MacShadowClipboard*)context->custom;
	if (!state || !state->sendServerCapabilities)
		return ERROR_INVALID_STATE;

	const CLIPRDR_GENERAL_CAPABILITY_SET* general =
	    (capabilities && capabilities->cCapabilitiesSets == 1)
	        ? (const CLIPRDR_GENERAL_CAPABILITY_SET*)capabilities->capabilitySets
	        : nullptr;
	const CLIPRDR_CAPABILITIES* offer = capabilities;
	WLog_DEBUG(TAG,
	          "Sending Server Clipboard Capabilities (type=0x%04" PRIx16
	          ", flags=0x%04" PRIx16 ", dataLen=%" PRIu32 ", sets=%" PRIu32
	          ", capType=0x%04" PRIx16 ", capLen=%" PRIu16 ", version=%" PRIu32
	          ", generalFlags=0x%08" PRIx32 ")",
	          (UINT16)(offer ? offer->common.msgType : 0),
	          (UINT16)(offer ? offer->common.msgFlags : 0),
	          offer ? offer->common.dataLen : 0,
	          offer ? offer->cCapabilitiesSets : 0,
	          (UINT16)(general ? general->capabilitySetType : 0),
	          (UINT16)(general ? general->capabilitySetLength : 0),
	          general ? general->version : 0,
	          general ? general->generalFlags : 0);
	return state->sendServerCapabilities(context, offer);
}

static UINT monitor_ready(CliprdrServerContext* context, const CLIPRDR_MONITOR_READY* ready)
{
	MacShadowClipboard* state = (MacShadowClipboard*)context->custom;
	if (!state || !state->sendMonitorReady)
		return ERROR_INVALID_STATE;
	WLog_DEBUG(TAG,
	          "Sending clipboard Monitor Ready (type=0x%04" PRIx16
	          ", flags=0x%04" PRIx16 ", dataLen=%" PRIu32 ")",
	          (UINT16)(ready ? ready->common.msgType : 0),
	          (UINT16)(ready ? ready->common.msgFlags : 0),
	          ready ? ready->common.dataLen : 0);
	const UINT rc = state->sendMonitorReady(context, ready);
	dispatch_sync(state->queue, ^{
	  state->monitorReady = (rc == CHANNEL_RC_OK);
	});
	return rc;
}

static UINT client_capabilities(CliprdrServerContext* context,
	                            const CLIPRDR_CAPABILITIES* capabilities)
{
	MacShadowClipboard* state = (MacShadowClipboard*)context->custom;
	if (!state)
		return CHANNEL_RC_OK;

	const CLIPRDR_GENERAL_CAPABILITY_SET* general =
	    (capabilities && capabilities->cCapabilitiesSets > 0)
	        ? (const CLIPRDR_GENERAL_CAPABILITY_SET*)capabilities->capabilitySets
	        : nullptr;
	WLog_DEBUG(TAG,
	          "Received Client Clipboard Capabilities (sets=%" PRIu32
	          ", version=%" PRIu32 ", generalFlags=0x%08" PRIx32 ")",
	          capabilities ? capabilities->cCapabilitiesSets : 0,
	          general ? general->version : 0, general ? general->generalFlags : 0);
	return CHANNEL_RC_OK;
}

/* Registered IDs are client-local: retain the wire ID separately from its encoding. */
static UINT32 clipboard_text_format(const CLIPRDR_FORMAT* format)
{
	if ((format->formatId == CF_UNICODETEXT) || (format->formatId == CF_TEXT) ||
	    (format->formatId == CF_OEMTEXT))
		return format->formatId;
	if (format->formatName &&
	    ((strcmp(format->formatName, "UTF8_STRING") == 0) ||
	     (strcmp(format->formatName, "text/plain") == 0) ||
	     (strcmp(format->formatName, "text/plain;charset=utf-8") == 0)))
		return CF_MAX; /* internal UTF-8 encoding marker, never sent as a format ID */
	return 0;
}

static UINT64 clipboard_now_ms(const MacShadowClipboard* state)
{
	return (state && state->clock) ? state->clock(state->clockContext) : GetTickCount64();
}

static void clipboard_check_request_timeout(MacShadowClipboard* state)
{
	if (!state || state->requestQuarantined || !state->requestedClientFormat ||
	    !state->requestDeadlineMs)
		return;
	if (clipboard_now_ms(state) >= state->requestDeadlineMs)
	{
		/* There is no request ID in cliprdr's response.  Keep the request marked
		 * until one response arrives, then consume that response as late data. */
		state->requestQuarantined = TRUE;
		state->requestTimeouts++;
		WLog_WARN(TAG,
		          "Client clipboard response timed out after %u ms; quarantining one late response",
		          MAC_SHADOW_CLIPBOARD_RESPONSE_TIMEOUT_MS);
	}
}

static void clipboard_clear_request(MacShadowClipboard* state)
{
	state->requestedClientFormat = 0;
	state->requestedTextFormat = 0;
	state->requestDeadlineMs = 0;
}

/* At most one request in flight: data responses have no format ID. */
static UINT request_client_text(CliprdrServerContext* context, MacShadowClipboard* state)
{
	if (state->requestedClientFormat || state->requestQuarantined || !state->pendingClientFormat)
		return CHANNEL_RC_OK;
	CLIPRDR_FORMAT_DATA_REQUEST request = {
		.common = { .msgType = CB_FORMAT_DATA_REQUEST, .dataLen = 4 },
		.requestedFormatId = state->pendingClientFormat
	};
	state->requestedClientFormat = state->pendingClientFormat;
	state->requestedTextFormat = state->pendingTextFormat;
	state->pendingClientFormat = 0;
	state->requestDeadlineMs = clipboard_now_ms(state) + MAC_SHADOW_CLIPBOARD_RESPONSE_TIMEOUT_MS;
	WLog_DEBUG(TAG, "Requesting client clipboard format=%" PRIu32,
	          request.requestedFormatId);
	const UINT rc = context->ServerFormatDataRequest(context, &request);
	if (rc != CHANNEL_RC_OK)
		clipboard_clear_request(state);
	return rc;
}

static UINT client_format_list(CliprdrServerContext* context, const CLIPRDR_FORMAT_LIST* list)
{
	MacShadowClipboard* state = (MacShadowClipboard*)context->custom;
	if (!state)
		return CHANNEL_RC_OK;
	__block UINT rc = CHANNEL_RC_OK;
	dispatch_sync(state->queue, ^{
	  if (state->stopped)
		  return;
	  clipboard_check_request_timeout(state);
	  UINT32 format = 0, textFormat = 0;
	  for (UINT32 x = 0; x < list->numFormats; x++)
	  {
		  const CLIPRDR_FORMAT* entry = &list->formats[x];
		  const UINT32 candidate = clipboard_text_format(entry);
		  WLog_DEBUG(TAG, "Client clipboard format[%" PRIu32 "]: id=%" PRIu32 ", name=%s",
		            x, entry->formatId, entry->formatName ? entry->formatName : "(standard)");
		  if (candidate && (!textFormat || (candidate == CF_UNICODETEXT)))
		  {
			  format = entry->formatId;
			  textFormat = candidate;
		  }
	  }
	WLog_DEBUG(TAG, "Received client clipboard Format List (%" PRIu32
	            " formats, selected text format=%" PRIu32 ")", list->numFormats, format);
	  /* A valid list is acknowledged even when it contains no supported text. */
	  rc = format_list_response(context, TRUE);
	  state->pendingClientFormat = format;
	  state->pendingTextFormat = textFormat;
	  if (rc == CHANNEL_RC_OK)
		  rc = request_client_text(context, state);
	});
	return rc;
}

static NSString* decode_client_text(const BYTE* bytes, size_t length, UINT32 formatId)
{
	if (!bytes)
		return nil;
	if (formatId == CF_UNICODETEXT)
	{
		if ((length % sizeof(unichar)) != 0)
			return nil;
		for (size_t x = 0; x + 1 < length; x += 2)
		{
			if ((bytes[x] == 0) && (bytes[x + 1] == 0))
			{
				length = x;
				break;
			}
		}
	}
	else
	{
		const void* terminator = memchr(bytes, '\0', length);
		if (terminator)
			length = (const BYTE*)terminator - bytes;
	}
	return [[NSString alloc] initWithBytes:bytes length:length
	                              encoding:clipboard_encoding(formatId)];
}

static UINT client_format_data_response(CliprdrServerContext* context,
	                                    const CLIPRDR_FORMAT_DATA_RESPONSE* response)
{
	MacShadowClipboard* state = (MacShadowClipboard*)context->custom;
	if (!state)
		return CHANNEL_RC_OK;

	__block UINT rc = CHANNEL_RC_OK;
	dispatch_sync(state->queue, ^{
	  if (state->stopped)
		  return;
	  clipboard_check_request_timeout(state);
	  const UINT32 format = state->requestedTextFormat;
	  const BOOL requested = state->requestedClientFormat != 0;
	  if (state->requestQuarantined)
	  {
		  state->requestQuarantined = FALSE;
		  state->lateResponses++;
		  clipboard_clear_request(state);
		  WLog_WARN(TAG, "Discarded one late client clipboard response after timeout");
		  rc = request_client_text(context, state);
		  return;
	  }
	  clipboard_clear_request(state);
	  if ((response->common.msgFlags & CB_RESPONSE_FAIL) ||
	      !response->requestedFormatData || !requested ||
	      (response->common.dataLen > MAC_SHADOW_CLIPBOARD_MAX_TEXT_BYTES))
	  {
		  WLog_WARN(TAG, "Client clipboard Format Data Response failed or was unsolicited");
		  rc = request_client_text(context, state);
		  return;
	  }
	  @autoreleasepool {
		  NSString* text = decode_client_text(response->requestedFormatData,
		                                      response->common.dataLen, format);
		  if (!text)
		  {
			  WLog_ERR(TAG,
			           "Invalid client clipboard text payload (format=%" PRIu32
			           ", bytes=%" PRIu32 ")",
			           format, response->common.dataLen);
			  rc = request_client_text(context, state);
			  return;
		  }
		  text = clipboard_normalize_from_rdp(text);
		  NSPasteboard* pb = [NSPasteboard generalPasteboard];
		  [pb clearContents];
		  if (![pb setString:text forType:NSPasteboardTypeString])
		  {
			  rc = request_client_text(context, state);
			  return;
		  }
		  state->changeCount = pb.changeCount;
		  WLog_DEBUG(TAG,
		            "Applied client clipboard data to macOS pasteboard "
		            "(format=%" PRIu32 ", bytes=%" PRIu32 ")",
		            format, response->common.dataLen);
		  rc = request_client_text(context, state);
	  }
	});
	return rc;
}

static UINT client_format_list_response(CliprdrServerContext* context,
	                                    const CLIPRDR_FORMAT_LIST_RESPONSE* response)
{
	WINPR_UNUSED(context);
	WLog_DEBUG(TAG, "Received client clipboard Format List Response (%s)",
	          (response->common.msgFlags & CB_RESPONSE_OK) ? "OK" : "FAIL");
	return CHANNEL_RC_OK;
}

static UINT client_format_data_request(CliprdrServerContext* context,
	                                   const CLIPRDR_FORMAT_DATA_REQUEST* request)
{
	MacShadowClipboard* state = (MacShadowClipboard*)context->custom;
	if (!state)
		return CHANNEL_RC_OK;

	__block UINT rc = CHANNEL_RC_OK;
	dispatch_sync(state->queue, ^{
	  if (state->stopped)
		  return;
	  CLIPRDR_FORMAT_DATA_RESPONSE response = {
		  .common = { .msgType = CB_FORMAT_DATA_RESPONSE,
		              .msgFlags = CB_RESPONSE_FAIL,
		              .dataLen = 0 },
		  .requestedFormatData = nullptr
	  };
	  @autoreleasepool {
		  WLog_DEBUG(TAG, "Client requested server clipboard format=%" PRIu32,
		            request->requestedFormatId);
		  NSString* text = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
		  NSData* data = nil;
		  if (text && ((request->requestedFormatId == CF_UNICODETEXT) ||
		               (request->requestedFormatId == CF_TEXT) ||
		               (request->requestedFormatId == CF_OEMTEXT)))
		  {
			  data = [clipboard_normalize_to_rdp(text)
			      dataUsingEncoding:clipboard_encoding(request->requestedFormatId)
			 allowLossyConversion:(request->requestedFormatId != CF_UNICODETEXT)];
		  }
		  if (data && (data.length <= UINT32_MAX) &&
		      (data.length <= MAC_SHADOW_CLIPBOARD_MAX_TEXT_BYTES))
		  {
			  response.common.msgFlags = CB_RESPONSE_OK;
			  response.common.dataLen = (UINT32)data.length;
			  response.requestedFormatData = data.bytes;
		  }
	  rc = context->ServerFormatDataResponse(context, &response);
		  WLog_DEBUG(TAG,
		            "Sent server clipboard Format Data Response (%s, format=%" PRIu32
		            ", bytes=%" PRIu32 ")",
		            (response.common.msgFlags & CB_RESPONSE_OK) ? "OK" : "FAIL",
		            request->requestedFormatId, response.common.dataLen);
	  }
	});
	return rc;
}

static void poll_pasteboard(void* arg)
{
	MacShadowClipboard* state = (MacShadowClipboard*)arg;
	if (!state || state->stopped)
		return;
	clipboard_check_request_timeout(state);
	if (!state->monitorReady || !state->client || !state->client->cliprdr)
		return;
	@autoreleasepool {
		NSPasteboard* pb = [NSPasteboard generalPasteboard];
		const NSInteger changed = pb.changeCount;
		if ((changed != state->changeCount) &&
		    ([pb stringForType:NSPasteboardTypeString] != nil))
		{
			state->changeCount = changed;
			(void)format_list(state->client->cliprdr);
		}
	}
}

static void clipboard_register_callbacks(CliprdrServerContext* context, MacShadowClipboard* state)
{
	state->sendServerCapabilities = context->ServerCapabilities;
	state->sendMonitorReady = context->MonitorReady;
	context->custom = state;
	context->ServerCapabilities = server_capabilities;
	context->MonitorReady = monitor_ready;
	context->ClientCapabilities = client_capabilities;
	context->ClientFormatList = client_format_list;
	context->ClientFormatListResponse = client_format_list_response;
	context->ClientFormatDataResponse = client_format_data_response;
	/* ServerFormatDataRequest is the outbound sender. Never replace it with a receiver. */
	context->ClientFormatDataRequest = client_format_data_request;
}

int mac_shadow_clipboard_init(rdpShadowClient* client, BOOL legacyWin98)
{
	if (!client)
		return -1;
	WLog_INFO(TAG, "Clipboard redirection enabled");
	CliprdrServerContext* context = client->cliprdr = cliprdr_server_context_new(client->vcm);
	if (!context)
		return -1;
	MacShadowClipboard* state = calloc(1, sizeof(*state));
	if (!state)
	{
		cliprdr_server_context_free(context);
		client->cliprdr = nullptr;
		return -1;
	}
	state->client = client;
	state->legacyRdp52 = legacyWin98;
	state->queue = dispatch_queue_create("mac.shadow.clipboard", DISPATCH_QUEUE_SERIAL);
	if (state->legacyRdp52)
	{
		@autoreleasepool {
		  state->changeCount = [NSPasteboard generalPasteboard].changeCount;
		}
		WLog_INFO(TAG,
		          "Legacy Win98 clipboard path selected: RDP 5.2 CF_TEXT with required "
		          "4-byte trailer");
	}
	else
		state->changeCount = -1;
	clipboard_register_callbacks(context, state);
	/* RDP 5.2 follows the pre-capabilities dialect.  Suppress the generic
	 * worker's modern initialization sequence before it can send any PDU. */
	context->autoInitializationSequence = !state->legacyRdp52;
	/* RDP 5.x-era Microsoft implementations and rdesktop put an additional
	 * zero DWORD after every clipboard PDU without including it in dataLen. */
	context->useLegacyPduTrailer = state->legacyRdp52;
	const UINT startStatus = context->Start(context);
	if (startStatus != CHANNEL_RC_OK)
	{
		WLog_ERR(TAG, "Clipboard channel open/start failed with error %" PRIu32,
		         startStatus);
		context->custom = nullptr;
		state->queue = nil;
		free(state);
		cliprdr_server_context_free(context);
		client->cliprdr = nullptr;
		return -1;
	}
	WLog_DEBUG(TAG, "Clipboard channel opened and worker started");
	if (state->legacyRdp52)
	{
		CLIPRDR_MONITOR_READY ready = { .common = { .msgType = CB_MONITOR_READY,
		                                             .msgFlags = 0,
		                                             .dataLen = 0 } };
		const UINT readyStatus = context->MonitorReady(context, &ready);
		if (readyStatus != CHANNEL_RC_OK)
		{
			WLog_ERR(TAG, "Legacy Win98 clipboard Monitor Ready failed with error %" PRIu32,
			         readyStatus);
			context->Stop(context);
			context->custom = nullptr;
			state->queue = nil;
			free(state);
			cliprdr_server_context_free(context);
			client->cliprdr = nullptr;
			return -1;
		}
		WLog_DEBUG(TAG, "Legacy Win98 clipboard initialized; waiting for client Format List");
	}
	state->timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, state->queue);
	dispatch_source_set_timer(state->timer, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC),
	                          NSEC_PER_SEC, NSEC_PER_MSEC * 100);
	dispatch_set_context(state->timer, state);
	dispatch_source_set_event_handler_f(state->timer, poll_pasteboard);
	dispatch_resume(state->timer);
	return 1;
}

void mac_shadow_clipboard_uninit(rdpShadowClient* client)
{
	if (!client || !client->cliprdr)
		return;
	CliprdrServerContext* context = client->cliprdr;
	MacShadowClipboard* state = (MacShadowClipboard*)context->custom;
	if (state)
	{
		dispatch_sync(state->queue, ^{
		  state->stopped = TRUE;
		});
		if (state->timer)
		{
			/* The serial queue is also the timer target.  Cancel, then fence that
			 * queue so its cancellation handler has run before freeing state. */
			dispatch_source_set_cancel_handler(state->timer, ^{});
			dispatch_source_cancel(state->timer);
		}
		dispatch_sync(state->queue, ^{});
	}
	context->Stop(context);
	context->custom = nullptr;
	if (state)
	{
		state->timer = nil;
		state->queue = nil;
		free(state);
	}
	cliprdr_server_context_free(context);
	client->cliprdr = nullptr;
	WLog_DEBUG(TAG, "Clipboard redirection stopped");
}
