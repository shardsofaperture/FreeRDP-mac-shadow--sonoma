/* Mac shadow socket-cap regression. Apache License, Version 2.0. */
#include <stdio.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../shadow_socket_cap.h"

#define CHECK(x)                                                                        \
	do                                                                                  \
	{                                                                                   \
		if (!(x))                                                                    \
		{                                                                            \
			fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #x);      \
			return 1;                                                            \
		}                                                                            \
	} while (0)

static int test_accepted_tcp_cap(UINT32 requestedBytes)
{
	int listener = -1;
	int client = -1;
	int accepted = -1;
	struct sockaddr_in address = { 0 };
	socklen_t length = sizeof(address);
	UINT32 effective = 0;
	int direct = 0;

	listener = socket(AF_INET, SOCK_STREAM, 0);
	CHECK(listener >= 0);
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	CHECK(bind(listener, (struct sockaddr*)&address, sizeof(address)) == 0);
	CHECK(getsockname(listener, (struct sockaddr*)&address, &length) == 0);
	CHECK(listen(listener, 1) == 0);
	client = socket(AF_INET, SOCK_STREAM, 0);
	CHECK(client >= 0);
	CHECK(connect(client, (struct sockaddr*)&address, sizeof(address)) == 0);
	accepted = accept(listener, NULL, NULL);
	CHECK(accepted >= 0);
	CHECK(shadow_socket_cap_configure(accepted, requestedBytes, &effective));
	length = sizeof(direct);
	CHECK(getsockopt(accepted, SOL_SOCKET, SO_SNDBUF, &direct, &length) == 0);
	CHECK(length == sizeof(direct) && effective == (UINT32)direct);
	printf("accepted-tcp requested=%u effective=%u\n", requestedBytes, effective);
	CHECK(effective == requestedBytes);
	CHECK(close(accepted) == 0);
	CHECK(close(client) == 0);
	CHECK(close(listener) == 0);
	return 0;
}

int main(void)
{
	UINT32 requested = 99;
	CHECK(shadow_socket_cap_parse_kib(NULL, &requested) && requested == 0);
	CHECK(shadow_socket_cap_parse_kib("0", &requested) && requested == 0);
	CHECK(shadow_socket_cap_parse_kib("4", &requested) && requested == 4U * 1024U);
	CHECK(shadow_socket_cap_parse_kib("8", &requested) && requested == 8U * 1024U);
	for (UINT32 kib = 16; kib <= 128; kib *= 2)
	{
		char value[4] = { 0 };
		(void)snprintf(value, sizeof(value), "%u", kib);
		CHECK(shadow_socket_cap_parse_kib(value, &requested));
		CHECK(requested == kib * 1024U);
	}
	CHECK(!shadow_socket_cap_parse_kib("", &requested));
	CHECK(!shadow_socket_cap_parse_kib("12", &requested));
	CHECK(!shadow_socket_cap_parse_kib("15", &requested));
	CHECK(!shadow_socket_cap_parse_kib("32KiB", &requested));
	CHECK(!shadow_socket_cap_parse_kib("-16", &requested));
	CHECK(!shadow_socket_cap_parse_kib(" 32", &requested));
	CHECK(!shadow_socket_cap_parse_kib("4294967296", &requested));
	CHECK(!shadow_socket_cap_parse_kib("32", NULL));

	int fds[2] = { -1, -1 };
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
	UINT32 initial = 0;
	UINT32 effective = 0;
	CHECK(shadow_socket_cap_configure(fds[0], 0, &initial));
	CHECK(shadow_socket_cap_configure(fds[0], 0, &effective));
	CHECK(initial == effective); /* The control arm does not set SO_SNDBUF. */
	const UINT32 capsKiB[] = { 4U, 8U, 16U, 32U, 64U, 128U };
	for (size_t i = 0; i < sizeof(capsKiB) / sizeof(capsKiB[0]); i++)
	{
		const UINT32 kib = capsKiB[i];
		CHECK(shadow_socket_cap_configure(fds[0], kib * 1024U, &effective));
		int direct = 0;
		socklen_t length = sizeof(direct);
		CHECK(getsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &direct, &length) == 0);
		CHECK(length == sizeof(direct) && effective == (UINT32)direct);
		printf("socketpair requested=%u effective=%u\n", kib * 1024U, effective);
	}
	CHECK(!shadow_socket_cap_configure(fds[0], 256U * 1024U, &effective));
	CHECK(!shadow_socket_cap_configure(-1, 0, &effective));
	CHECK(!shadow_socket_cap_configure(fds[0], 0, NULL));
	CHECK(close(fds[0]) == 0);
	CHECK(close(fds[1]) == 0);
	CHECK(test_accepted_tcp_cap(4U * 1024U) == 0);
	CHECK(test_accepted_tcp_cap(8U * 1024U) == 0);
	return 0;
}
