/* Experimental Mac shadow accepted-socket send buffer. Apache License, Version 2.0. */
#include <freerdp/config.h>

#include <string.h>
#include <sys/socket.h>

#include "shadow_socket_cap.h"

BOOL shadow_socket_cap_parse_kib(const char* value, UINT32* requestedBytes)
{
	if (!requestedBytes)
		return FALSE;
	if (!value || strcmp(value, "0") == 0)
		*requestedBytes = 0;
	else if (strcmp(value, "4") == 0)
		*requestedBytes = 4U * 1024U;
	else if (strcmp(value, "8") == 0)
		*requestedBytes = 8U * 1024U;
	else if (strcmp(value, "16") == 0)
		*requestedBytes = 16U * 1024U;
	else if (strcmp(value, "32") == 0)
		*requestedBytes = 32U * 1024U;
	else if (strcmp(value, "64") == 0)
		*requestedBytes = 64U * 1024U;
	else if (strcmp(value, "128") == 0)
		*requestedBytes = 128U * 1024U;
	else
		return FALSE;
	return TRUE;
}

BOOL shadow_socket_cap_configure(int fd, UINT32 requestedBytes, UINT32* effectiveBytes)
{
	if (fd < 0 || !effectiveBytes || requestedBytes > 128U * 1024U)
		return FALSE;
	if (requestedBytes > 0)
	{
		const int requested = (int)requestedBytes;
		if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &requested, sizeof(requested)) != 0)
			return FALSE;
	}
	int effective = 0;
	socklen_t length = sizeof(effective);
	if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &effective, &length) != 0 ||
	    length != sizeof(effective) || effective <= 0)
		return FALSE;
	*effectiveBytes = (UINT32)effective;
	return TRUE;
}
