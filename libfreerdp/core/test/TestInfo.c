/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * RDP Client Info unit tests
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

#include <freerdp/peer.h>

#include "../info.h"

struct info_test_case
{
	UINT32 rdpVersion;
	UINT16 cbClientAddress;
	BOOL terminated;
	BOOL expected;
};

static const struct info_test_case info_test_cases[] = {
	{ RDP_VERSION_5_PLUS, 64, TRUE, TRUE },
	{ RDP_VERSION_5_PLUS, 78, TRUE, FALSE },
	{ RDP_VERSION_5_PLUS, 80, TRUE, TRUE },
	{ RDP_VERSION_5_PLUS, 80, FALSE, FALSE },
	{ RDP_VERSION_5_PLUS, 82, TRUE, FALSE },
	{ RDP_VERSION_10_0, 80, TRUE, TRUE },
};

static wStream* info_test_packet(rdpRdp* rdp, UINT16 cbClientAddress, BOOL terminated)
{
	static const char address[] = "127.0.0.1";
	wStream* s = Stream_New(nullptr, 512);
	if (!s)
		return nullptr;

	if (!Stream_SetPosition(s, RDP_PACKET_HEADER_MAX_LENGTH))
	{
		(void)fprintf(stderr, "failed to reserve packet header\n");
		goto fail;
	}
	if (!rdp_write_security_header(rdp, s, SEC_INFO_PKT))
	{
		(void)fprintf(stderr, "failed to write security header\n");
		goto fail;
	}
	if (!Stream_EnsureRemainingCapacity(s, 18ULL + 5 * sizeof(WCHAR) + 4ULL +
	                                           cbClientAddress + 2ULL + sizeof(WCHAR)))
	{
		(void)fprintf(stderr, "failed to reserve packet body\n");
		goto fail;
	}

	Stream_Write_UINT32(s, 0);            /* CodePage */
	Stream_Write_UINT32(s, INFO_UNICODE); /* flags */
	Stream_Write_UINT16(s, 0);            /* cbDomain */
	Stream_Write_UINT16(s, 0);            /* cbUserName */
	Stream_Write_UINT16(s, 0);            /* cbPassword */
	Stream_Write_UINT16(s, 0);            /* cbAlternateShell */
	Stream_Write_UINT16(s, 0);            /* cbWorkingDir */
	Stream_Zero(s, 5 * sizeof(WCHAR));     /* mandatory null terminators */

	Stream_Write_UINT16(s, ADDRESS_FAMILY_INET);
	Stream_Write_UINT16(s, cbClientAddress);
	for (size_t x = 0; x < sizeof(address) - 1; x++)
		Stream_Write_UINT16(s, address[x]);
	const size_t padding = cbClientAddress - (sizeof(address) - 1) * sizeof(WCHAR);
	if (terminated)
		Stream_Zero(s, padding);
	else
	{
		for (size_t x = 0; x < padding / sizeof(WCHAR); x++)
			Stream_Write_UINT16(s, 'x');
	}

	Stream_Write_UINT16(s, sizeof(WCHAR)); /* cbClientDir */
	Stream_Write_UINT16(s, 0);             /* clientDir */

	const size_t length = Stream_GetPosition(s);
	if (!Stream_SetPosition(s, 0))
		goto fail;

	rdp->settings->ServerMode = FALSE;
	if (!rdp_write_header(rdp, s, length, MCS_GLOBAL_CHANNEL_ID, SEC_INFO_PKT))
	{
		(void)fprintf(stderr, "failed to write packet header\n");
		goto fail;
	}
	rdp->settings->ServerMode = TRUE;

	if (!Stream_SetPosition(s, length))
		goto fail;
	Stream_SealLength(s);
	if (!Stream_SetPosition(s, 0))
		goto fail;
	return s;

fail:
	Stream_Free(s, TRUE);
	return nullptr;
}

static BOOL info_test_run_case(const struct info_test_case* test)
{
	BOOL rc = FALSE;
	freerdp_peer* client = calloc(1, sizeof(freerdp_peer));
	if (!client)
		return FALSE;

	client->sockfd = -1;
	client->ContextSize = sizeof(rdpContext);
	if (!freerdp_peer_context_new(client))
		goto fail;

	rdpRdp* rdp = client->context->rdp;
	WINPR_ASSERT(rdp);
	rdp->mcs->userId = MCS_BASE_CHANNEL_ID;
	rdp->settings->RdpVersion = test->rdpVersion;
	rdp->settings->UseRdpSecurityLayer = FALSE;
	rdp->state = CONNECTION_STATE_SECURE_SETTINGS_EXCHANGE;

	wStream* s = info_test_packet(rdp, test->cbClientAddress, test->terminated);
	if (!s)
		goto fail;

	const BOOL actual = rdp_recv_client_info(rdp, s);
	Stream_Free(s, TRUE);
	if (actual != test->expected)
	{
		(void)fprintf(stderr,
		              "RDP version 0x%08" PRIx32 ", cbClientAddress=%" PRIu16
		              ", terminated=%" PRId32 ": expected %" PRId32 ", got %" PRId32 "\n",
		              test->rdpVersion, test->cbClientAddress, test->terminated, test->expected,
		              actual);
		goto fail;
	}

	if (actual &&
	    (strcmp(freerdp_settings_get_string(rdp->settings, FreeRDP_ClientAddress),
	            "127.0.0.1") != 0))
	{
		(void)fprintf(stderr, "parsed client address does not match\n");
		goto fail;
	}

	rc = TRUE;
fail:
	freerdp_peer_context_free(client);
	free(client);
	return rc;
}

int TestInfo(WINPR_ATTR_UNUSED int argc, WINPR_ATTR_UNUSED char* argv[])
{
	for (size_t x = 0; x < ARRAYSIZE(info_test_cases); x++)
	{
		if (!info_test_run_case(&info_test_cases[x]))
			return -1;
	}
	return 0;
}
