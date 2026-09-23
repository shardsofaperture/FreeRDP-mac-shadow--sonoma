/* Experimental Mac shadow accepted-socket send buffer. Apache License, Version 2.0. */
#ifndef FREERDP_SHADOW_SOCKET_CAP_H
#define FREERDP_SHADOW_SOCKET_CAP_H

#include <winpr/wtypes.h>

/* Unset or "0" preserves the kernel default. Other values are KiB. */
BOOL shadow_socket_cap_parse_kib(const char* value, UINT32* requestedBytes);

/* The caller retains socket ownership. effectiveBytes is always read back. */
BOOL shadow_socket_cap_configure(int fd, UINT32 requestedBytes, UINT32* effectiveBytes);

#endif
