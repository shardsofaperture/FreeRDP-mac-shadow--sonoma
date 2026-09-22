/** FreeRDP macOS clipboard regressions. Apache License 2.0. */
/* No listener, display, input injection, or system pasteboard modification. */
/* Compile the real server parser into this test to exercise its private PDU dispatch. */
#define cliprdr_server_context_new test_cliprdr_server_context_new
#define cliprdr_server_context_free test_cliprdr_server_context_free
#include "../../../channels/cliprdr/server/cliprdr_main.c"
#undef TAG
#include "../Mac/mac_shadow_clipboard.m"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #c); exit(1); } } while (0)
static UINT requests, acknowledgements, formatLists;
static UINT32 lastFormat;
static UINT32 capabilityVersion, capabilityFlags;
static UINT64 fakeClock;
static UINT64 test_clock(WINPR_ATTR_UNUSED void* context)
{
	return fakeClock;
}
static UINT send_request(WINPR_ATTR_UNUSED CliprdrServerContext* context,
                         const CLIPRDR_FORMAT_DATA_REQUEST* request)
{
	CHECK(request->common.msgType == CB_FORMAT_DATA_REQUEST);
	CHECK(request->common.dataLen == 4);
	requests++;
	lastFormat = request->requestedFormatId;
	return CHANNEL_RC_OK;
}
static UINT acknowledge(WINPR_ATTR_UNUSED CliprdrServerContext* context,
                        const CLIPRDR_FORMAT_LIST_RESPONSE* response)
{
	CHECK(response->common.msgFlags == CB_RESPONSE_OK);
	acknowledgements++;
	return CHANNEL_RC_OK;
}
static UINT ready(WINPR_ATTR_UNUSED CliprdrServerContext* context,
                  WINPR_ATTR_UNUSED const CLIPRDR_MONITOR_READY* message)
{
	return CHANNEL_RC_OK;
}
static UINT send_capabilities(WINPR_ATTR_UNUSED CliprdrServerContext* context,
                              const CLIPRDR_CAPABILITIES* capabilities)
{
	CHECK(capabilities->common.msgType == CB_CLIP_CAPS);
	CHECK(capabilities->cCapabilitiesSets == 1);
	const CLIPRDR_GENERAL_CAPABILITY_SET* general =
	    (const CLIPRDR_GENERAL_CAPABILITY_SET*)capabilities->capabilitySets;
	capabilityVersion = general->version;
	capabilityFlags = general->generalFlags;
	return CHANNEL_RC_OK;
}
static UINT send_format_list(WINPR_ATTR_UNUSED CliprdrServerContext* context,
                             const CLIPRDR_FORMAT_LIST* list)
{
	formatLists++;
	CHECK(list->common.msgType == CB_FORMAT_LIST);
	CHECK(list->common.dataLen == list->numFormats * 36);
	CHECK(list->numFormats == 1 && list->formats[0].formatId == CF_TEXT);
	return CHANNEL_RC_OK;
}
static UINT receive_list(CliprdrServerContext* context, CLIPRDR_FORMAT_LIST* list)
{
	list->common.msgType = CB_FORMAT_LIST;
	wStream* packet = cliprdr_packet_format_list_new(list, context->useLongFormatNames, FALSE);
	CHECK(packet);
	/* The common serializer seals the packet when the sender writes it. */
	Stream_SealLength(packet);
	list->common.msgType = CB_FORMAT_LIST;
	list->common.dataLen = (UINT32)Stream_Length(packet) - 8;
	CHECK(Stream_SetPosition(packet, 8));
	UINT rc = cliprdr_server_receive_pdu(context, packet, &list->common);
	Stream_Free(packet, TRUE);
	return rc;
}

int main(void)
{
	@autoreleasepool {
		CliprdrServerContext* context = cliprdr_server_context_new(NULL);
		CHECK(context);
		MacShadowClipboard state = { 0 };
		state.queue = dispatch_queue_create("test.mac.clipboard", DISPATCH_QUEUE_SERIAL);
		state.clock = test_clock;
		context->ServerCapabilities = send_capabilities;
		psCliprdrServerFormatDataRequest original = context->ServerFormatDataRequest;
		context->MonitorReady = ready;
		clipboard_register_callbacks(context, &state);
		CHECK(context->ServerFormatDataRequest == original);
		CHECK(context->ClientFormatDataRequest == client_format_data_request);
		CHECK(!context->useLongFormatNames); /* legacy short names, including no-capabilities clients */
		CHECK(!context->streamFileClipEnabled && !context->canLockClipData);
		CHECK(!context->useLegacyPduTrailer); /* modern framing remains the default */
		UINT32 logicalDataLen = UINT32_MAX, channelPduBytes = UINT32_MAX;
		BOOL trailerAppended = TRUE;
		wStream* modernMonitor = cliprdr_packet_new(CB_MONITOR_READY, 0, 0);
		CHECK(modernMonitor);
		CHECK(cliprdr_server_prepare_packet(context, modernMonitor, &logicalDataLen,
		                                    &channelPduBytes, &trailerAppended) == CHANNEL_RC_OK);
		CHECK(logicalDataLen == 0 && channelPduBytes == 8 && !trailerAppended);
		Stream_Free(modernMonitor, TRUE);
		context->ServerFormatDataRequest = send_request;
		context->ServerFormatListResponse = acknowledge;
		CLIPRDR_MONITOR_READY monitor = { .common.msgType = CB_MONITOR_READY };
		CHECK(context->MonitorReady(context, &monitor) == CHANNEL_RC_OK);
		CHECK(state.monitorReady); /* no ClientCapabilities required */
		CLIPRDR_GENERAL_CAPABILITY_SET v2 = { CB_CAPSTYPE_GENERAL, CB_CAPSTYPE_GENERAL_LEN,
		                                      CB_CAPS_VERSION_2, CB_USE_LONG_FORMAT_NAMES };
		CLIPRDR_CAPABILITIES offered = { .common = { .msgType = CB_CLIP_CAPS },
		                                 .cCapabilitiesSets = 1,
		                                 .capabilitySets = (CLIPRDR_CAPABILITY_SET*)&v2 };
		CHECK(context->ServerCapabilities(context, &offered) == CHANNEL_RC_OK);
		CHECK(capabilityVersion == CB_CAPS_VERSION_2 && capabilityFlags == CB_USE_LONG_FORMAT_NAMES);
		/* The RDP 5.2/Win98 dialect is connection-local: no capabilities and
		 * Monitor Ready only, leaving the client to originate its Format List. */
		state.legacyRdp52 = TRUE;
		context->useLegacyPduTrailer = TRUE;
		wStream* legacyMonitor = cliprdr_packet_new(CB_MONITOR_READY, 0, 0);
		CHECK(legacyMonitor);
		CHECK(cliprdr_server_prepare_packet(context, legacyMonitor, &logicalDataLen,
		                                    &channelPduBytes, &trailerAppended) == CHANNEL_RC_OK);
		CHECK(logicalDataLen == 0 && channelPduBytes == 12 && trailerAppended);
		CHECK(Stream_GetPosition(legacyMonitor) == 12);
		const BYTE expectedLegacyMonitor[] = { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
		CHECK(memcmp(Stream_Buffer(legacyMonitor), expectedLegacyMonitor,
		             sizeof(expectedLegacyMonitor)) == 0);
		Stream_SealLength(legacyMonitor);
		CHECK(cliprdr_server_has_legacy_trailer(legacyMonitor, logicalDataLen,
		                                        channelPduBytes));
		Stream_Free(legacyMonitor, TRUE);
		context->ServerFormatList = send_format_list;
		CHECK(context->MonitorReady(context, &monitor) == CHANNEL_RC_OK);
		CHECK(formatLists == 0);
		/* Later local clipboard changes use the legacy short-name encoding. */
		CHECK(format_list(context) == CHANNEL_RC_OK);
		CHECK(formatLists == 1);
		CLIPRDR_FORMAT legacyFormat = { CF_TEXT, NULL };
		CLIPRDR_FORMAT_LIST legacyList = { .common = { .msgType = CB_FORMAT_LIST },
		                                 .numFormats = 1, .formats = &legacyFormat };
		wStream* legacyPacket = cliprdr_packet_format_list_new(&legacyList, FALSE, FALSE);
		CHECK(legacyPacket && Stream_Length(legacyPacket) == 44);
		CHECK(cliprdr_server_prepare_packet(context, legacyPacket, &logicalDataLen,
		                                    &channelPduBytes, &trailerAppended) == CHANNEL_RC_OK);
		CHECK(logicalDataLen == 36 && channelPduBytes == 48 && trailerAppended);
		const BYTE expectedLegacyPacket[] = { 2, 0, 0, 0, 36, 0, 0, 0, 1, 0, 0, 0,
		                                      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		                                      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		                                      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
		CHECK(memcmp(Stream_Buffer(legacyPacket), expectedLegacyPacket,
		             sizeof(expectedLegacyPacket)) == 0);
		Stream_Free(legacyPacket, TRUE);
		state.legacyRdp52 = FALSE;
		context->useLegacyPduTrailer = FALSE;
		/* The exact legacy zero trailer is excluded only from the local parser view;
		 * it remains present in the received channel PDU and is never sent differently. */
		const BYTE caps[] = { 7, 0, 0, 0, 16, 0, 0, 0, 1, 0, 0, 0, 1, 0, 12, 0,
		                      2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
		wStream capBuffer = { 0 };
		wStream* capStream = Stream_StaticConstInit(&capBuffer, caps, sizeof(caps));
		CLIPRDR_HEADER capHeader = { .msgType = CB_CLIP_CAPS, .dataLen = 16 };
		context->useLegacyPduTrailer = TRUE;
		CHECK(cliprdr_server_strip_legacy_trailer(context, capStream, capHeader.dataLen,
		                                           sizeof(caps)));
		CHECK(Stream_Length(capStream) == 24);
		CHECK(Stream_SetPosition(capStream, 8));
		CHECK(Stream_GetRemainingLength(capStream) == 16);
		CHECK(cliprdr_server_receive_pdu(context, capStream, &capHeader) == CHANNEL_RC_OK);
		CHECK(Stream_GetRemainingLength(capStream) == 0);
		context->useLegacyPduTrailer = FALSE;
		CHECK(state.monitorReady);

		CLIPRDR_FORMAT formats[] = { { CF_TEXT, NULL }, { CF_UNICODETEXT, NULL } };
		CLIPRDR_FORMAT_LIST list = { .numFormats = 2, .formats = formats };
		CHECK(receive_list(context, &list) == CHANNEL_RC_OK);
		CHECK(requests == 1 && lastFormat == CF_UNICODETEXT);
		/* A missing response must not leave the request slot occupied forever.  Because
		 * cliprdr has no response ID, the first response after the deadline is consumed
		 * as late data before a newer pending format is requested. */
		fakeClock = state.requestDeadlineMs + 1;
		clipboard_check_request_timeout(&state);
		CHECK(state.requestQuarantined && state.requestTimeouts == 1);
		/* Overlapping lists must not change the encoding of the outstanding response. */
		CLIPRDR_FORMAT android = { 0xC123, "text/plain" };
		list.numFormats = 1; list.formats = &android;
		CHECK(receive_list(context, &list) == CHANNEL_RC_OK);
		CHECK(requests == 1 && state.requestedTextFormat == CF_UNICODETEXT);
		CLIPRDR_FORMAT_DATA_RESPONSE failed = { .common.msgFlags = CB_RESPONSE_FAIL };
		CHECK(context->ClientFormatDataResponse(context, &failed) == CHANNEL_RC_OK);
		CHECK(requests == 2 && lastFormat == 0xC123 && state.requestedTextFormat == CF_MAX);
		CHECK(state.lateResponses == 1 && !state.requestQuarantined);
		CHECK(context->ClientFormatDataResponse(context, &failed) == CHANNEL_RC_OK);
		CLIPRDR_FORMAT oversizedFormat = { 0xC125, "text/plain" };
		list.numFormats = 1;
		list.formats = &oversizedFormat;
		CHECK(receive_list(context, &list) == CHANNEL_RC_OK);
		CHECK(requests == 3 && lastFormat == 0xC125);
		const BYTE oneByte[] = { 'x' };
		CLIPRDR_FORMAT_DATA_RESPONSE oversized = {
			.common = { .msgFlags = CB_RESPONSE_OK,
			            .dataLen = MAC_SHADOW_CLIPBOARD_MAX_TEXT_BYTES + 1U },
			.requestedFormatData = oneByte
		};
		CHECK(context->ClientFormatDataResponse(context, &oversized) == CHANNEL_RC_OK);
		CHECK(requests == 3 && !state.requestedClientFormat);
		CLIPRDR_FORMAT image = { 0xC124, "image/png" };
		list.formats = &image;
		CHECK(receive_list(context, &list) == CHANNEL_RC_OK);
		list.numFormats = 0;
		CHECK(receive_list(context, &list) == CHANNEL_RC_OK);
		CHECK(requests == 3 && acknowledgements == 5);
		CHECK(clipboard_text_format(&image) == 0);

		const BYTE unicode[] = { 'A', 0, 0xE9, 0, 0, 0, 'X', 0 };
		CHECK([decode_client_text(unicode, sizeof(unicode), CF_UNICODETEXT) isEqualToString:@"Aé"]);
		CHECK(decode_client_text(unicode, 3, CF_UNICODETEXT) == nil);
		const BYTE ansi[] = { 0xE9, 0 };
		const BYTE oem[] = { 0x82, 0 };
		const BYTE utf8[] = { 0xC3, 0xA9, 0 };
		CHECK([decode_client_text(ansi, sizeof(ansi), CF_TEXT) isEqualToString:@"é"]);
		CHECK([decode_client_text(oem, sizeof(oem), CF_OEMTEXT) isEqualToString:@"é"]);
		CHECK([decode_client_text(utf8, sizeof(utf8), CF_MAX) isEqualToString:@"é"]);
		CHECK([clipboard_normalize_from_rdp(@"a\r\nb\rc") isEqualToString:@"a\nb\nc"]);
		NSData* encoded = [clipboard_normalize_to_rdp(@"a\nb") dataUsingEncoding:NSUTF16LittleEndianStringEncoding];
		const BYTE expected[] = { 'a', 0, '\r', 0, '\n', 0, 'b', 0, 0, 0 };
		CHECK(encoded.length == sizeof(expected) && memcmp(encoded.bytes, expected, sizeof(expected)) == 0);
		context->custom = NULL;
		cliprdr_server_context_free(context);
		state.queue = nil;
	}
	return 0;
}
