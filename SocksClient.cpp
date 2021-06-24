/*
 * WiFiPPP
 * Copyright (c) 2021 joshua stein <jcs@jcs.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "SocksClient.h"
#include "wifippp.h"

enum {
	STATE_DEAD = 0,
	STATE_INIT,
	STATE_METHOD,
	STATE_REQUEST,
	STATE_PROXY,
};

#define VERSION_SOCKS5		0x05

#define METHOD_MIN_LENGTH	3
#define METHOD_AUTH_NONE	0x0

#define REQUEST_MIN_LENGTH	9
#define REQUEST_COMMAND_CONNECT	0x1
#define REQUEST_ATYP_IP		0x1
#define REQUEST_ATYP_HOSTNAME	0x3
#define REQUEST_ATYP_IP6	0x4

#define REPLY_SUCCESS		0x0
#define REPLY_FAIL		0x1
#define REPLY_EPERM		0x02
#define REPLY_NET_UNREACHABLE	0x03
#define REPLY_HOST_UNREACHABLE	0x04
#define REPLY_CONN_REFUSED	0x05
#define REPLY_TTL_EXPIRED	0x06
#define REPLY_BAD_COMMAND	0x07
#define REPLY_BAD_ADDRESS	0x08

SocksClient::~SocksClient()
{
}

SocksClient::SocksClient(int _slot, WiFiClient _client)
    : slot(_slot), client(_client)
{
	state = STATE_INIT;

	memset(buf, 0, sizeof(buf));
	buflen = 0;

	syslog.logf(LOG_DEBUG, "[%d] in socks client init with ip %s", slot,
	    client.remoteIP().toString().c_str());
}

bool
SocksClient::done()
{
	if (state == STATE_DEAD)
		return true;

	if (!client.connected())
		return true;

	return false;
}

void
SocksClient::process()
{
	if (state == STATE_DEAD)
		return;

	if (client.available() && sizeof(buf) - buflen > 0)
		buflen += client.read(buf + buflen, sizeof(buf) - buflen);

	switch (state) {
	case STATE_INIT:
		if (buflen >= METHOD_MIN_LENGTH) {
			state = STATE_METHOD;
			break;
		}
		break;
	case STATE_METHOD:
		verify_method();
		break;
	}
}

void
SocksClient::verify_method()
{
	if (state != STATE_METHOD) {
		syslog.logf(LOG_ERR, "[%d] %s but state %d", slot, __func__,
		    state);
		state = STATE_DEAD;
		return;
	}

	if (buflen < METHOD_MIN_LENGTH)
		return;

	if (buf[0] != VERSION_SOCKS5) {
		syslog.logf(LOG_ERR, "[%d] unsupported version 0x%x", slot,
		    buf[0]);
		fail_close(REPLY_FAIL);
		return;
	}
}

void
SocksClient::fail_close(char code)
{
	unsigned char msg[] = {
		VERSION_SOCKS5,
		code,
		0,
		REQUEST_ATYP_IP,
		0, 0, 0, 0,
		0, 0,
	};

	client.write(msg, sizeof(msg));

	state = STATE_DEAD;
	client.stop();
	server.stop();
}
