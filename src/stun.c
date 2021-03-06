/* Adapted from:
 * https://github.com/0xFireWolf/STUNExternalIP/blob/master/STUNExternalIP.c
 *
 * Copyright (c) 2017 FireWolf
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "features.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "ishoal.h"

const char stun_host[] = "ishoal.ink";
const uint16_t stun_port = 3478;

// MARK: === PRIVATE DATA STRUCTURE ===

// RFC 5389 Section 6 STUN Message Structure
struct STUNMessageHeader {
	// Message Type (Binding Request / Response)
	unsigned short type;
	// Payload length of this message
	unsigned short length;
	// Magic Cookie
	unsigned int cookie;
	// Unique Transaction ID
	unsigned int id[3];
};

#define XOR_MAPPED_ADDRESS_TYPE 0x0020

// RFC 5389 Section 15 STUN Attributes
struct STUNAttributeHeader {
	// Attibute Type
	unsigned short type;
	// Payload length of this attribute
	unsigned short length;
};

#define IPv4_ADDRESS_FAMILY 0x01;
#define IPv6_ADDRESS_FAMILY 0x02;

// RFC 5389 Section 15.2 XOR-MAPPED-ADDRESS
struct STUNXORMappedIPv4Address {
	unsigned char reserved;
	unsigned char family;
	unsigned short port;
	unsigned int address;
};

void do_stun(int sockfd, ipaddr_t *address, uint16_t *port)
{
	struct addrinfo *results = NULL;

	struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};

	if (getaddrinfo(stun_host, NULL, &hints, &results))
		perror_exit("getaddrinfo");

	struct in_addr stunaddr = ((struct sockaddr_in *)results->ai_addr)->sin_addr;
	freeaddrinfo(results);

	struct sockaddr_in remote_addr = {
		.sin_family = AF_INET,
		.sin_addr = stunaddr,
		.sin_port = htons(stun_port),
	};

	struct STUNMessageHeader request = {
		.type = htons(0x0001),
		.length = htons(0x0000),
		.cookie = htonl(0x2112A442),
	};

	for (int i = 0; i < 3; i++) {
		srand(time(0));
		request.id[i] = rand();
	}

	if (sendto(sockfd, &request, sizeof(request), 0,
	    (struct sockaddr *)&remote_addr, sizeof(remote_addr)) < 0)
		perror_exit("sendto");

	struct eventloop *wait_for_stun = eventloop_new();
	eventloop_install_break(wait_for_stun, thread_stop_eventfd(current));
	eventloop_install_break(wait_for_stun, sockfd);

	bool timeout = eventloop_enter(wait_for_stun, 2000);
	eventloop_destroy(wait_for_stun);

	if (thread_should_stop(current))
		return;

	if (timeout)
		fprintf_exit("STUN timeout\n");

	char buffer[512];
	ssize_t length = read(sockfd, buffer, sizeof(buffer));
	if (length < 0)
		perror_exit("read");

	void *ptr = buffer;
	void *end = buffer + length;

	struct STUNMessageHeader *response = (void *)ptr;
	if (ptr + sizeof(*response) > end)
		goto err;

	if (response->type != htons(0x0101))
		goto err;

	for (int i = 0; i < 3; i++)
		if (request.id[i] != response->id[i])
			goto err;

	ptr += sizeof(struct STUNMessageHeader);

	while (ptr < end) {
		struct STUNAttributeHeader *header = ptr;
		ptr += sizeof(*header);

		if (ptr > end)
			goto err;

		if (header->type == htons(XOR_MAPPED_ADDRESS_TYPE)) {
			struct STUNXORMappedIPv4Address *xorAddress = ptr;

			if (ptr + sizeof(*xorAddress) > end)
				goto err;

			*address = htonl(ntohl(xorAddress->address)^0x2112A442);
			*port = ntohs(xorAddress->port)^0x2112;
			return;
		}

		ptr += ntohs(header->length);
	}

err:
	fprintf_exit("STUN resolve failure\n");
}
