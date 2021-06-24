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
	STATE_CONNECT,
	STATE_PROXY,
};

/* https://datatracker.ietf.org/doc/html/rfc1928 */

#define VERSION_SOCKS5		0x05

#define METHOD_MIN_LENGTH	3
#define METHOD_AUTH_NONE	0x0
#define METHOD_AUTH_BAD		0xFF

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
	case STATE_REQUEST:
		handle_request();
		break;
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

bool
SocksClient::verify_version()
{
	if (buf[0] != VERSION_SOCKS5) {
		syslog.logf(LOG_ERR, "[%d] unsupported version 0x%x", slot,
		    buf[0]);
		fail_close(REPLY_FAIL);
		return false;
	}

	return true;
}

bool
SocksClient::verify_state(int _state)
{
	if (state != _state) {
		syslog.logf(LOG_ERR, "[%d] in state %d but expected %d",
		    slot, state, _state);
		state = STATE_DEAD;
		return false;
	}

	return true;
}

void
SocksClient::verify_method()
{
	int i;

	if (!verify_state(STATE_METHOD))
		return;

	if (buflen < METHOD_MIN_LENGTH)
		return;

	if (!verify_version())
		return;

	/* buf[1] is NMETHODS, find one we like */
	for (i = 0; i < (unsigned char)buf[1]; i++) {
		if (buf[2 + i] == METHOD_AUTH_NONE) {
			/* send back method selection */
			unsigned char msg[] = {
				VERSION_SOCKS5,
				METHOD_AUTH_NONE,
			};

			client.write(msg, sizeof(msg));
			state = STATE_REQUEST;
			buflen = 0;
			return;
		}
	}

	syslog.logf(LOG_ERR, "[%d] no supported auth methods", slot);

	unsigned char msg[] = {
		VERSION_SOCKS5,
		METHOD_AUTH_BAD,
	};
	client.write(msg, sizeof(msg));
	fail_close(REPLY_FAIL);
}

void
SocksClient::handle_request()
{
	if (!verify_state(STATE_REQUEST))
		return;

	if (buflen < METHOD_MIN_LENGTH)
		return;

	if (!verify_version())
		return;

	if (buf[1] != REQUEST_COMMAND_CONNECT) {
		syslog.logf(LOG_ERR, "[%d] unsupported request command 0x%x",
		    slot, buf[1]);
		fail_close(REPLY_BAD_COMMAND);
		return;
	}

	/* buf[2] is reserved */

	switch (buf[3]) {
	case REQUEST_ATYP_IP:
		if (buflen < 4 + 4 + 2)
			return;

		IP4_ADDR(&remote_ip, buf[4], buf[5], buf[6], buf[7]);
		remote_port = (uint16_t)((buf[8] & 0xff) << 8) |
		    (buf[9] & 0xff);

		syslog.logf(LOG_DEBUG, "[%d] CONNECT request to IP %s:%d",
		    slot, ipaddr_ntoa(&remote_ip), remote_port);

		break;
	case REQUEST_ATYP_HOSTNAME: {
		IPAddress resip;
		unsigned char hostlen;

		if (buflen < 4 + 2)
			return;

		hostlen = buf[4];
		if (buflen < (unsigned char)(4 + hostlen + 2))
			return;

		remote_hostname = (unsigned char *)malloc(hostlen + 1);
		if (!remote_hostname) {
			syslog.logf(LOG_ERR, "[%d] malloc(%d) failure",
			    slot, hostlen + 1);
			fail_close(REPLY_FAIL);
			return;
		}

		memcpy(remote_hostname, buf + 5, hostlen);
		remote_hostname[hostlen] = '\0';

		/* network order */
		remote_port = (uint16_t)((buf[5 + hostlen] & 0xff) << 8) |
		    (buf[5 + hostlen + 1] & 0xff);

		if (WiFi.hostByName((const char *)remote_hostname,
		    resip) != 1) {
			syslog.logf(LOG_ERR, "[%d] CONNECT request to "
			    "hostname %s:%d, couldn't resolve name",
			    slot, remote_hostname, remote_port);
			fail_close(REPLY_BAD_ADDRESS);
			return;
		}

		syslog.logf(LOG_DEBUG, "[%d] CONNECT request to hostname "
		    "%s:%d, resolved to IP %s", slot, remote_hostname,
		    remote_port, ipaddr_ntoa(resip));

		// e.g., curl --preproxy socks5h://1.2.3.4 ...
		break;
	}
	case REQUEST_ATYP_IP6:
		syslog.logf(LOG_ERR, "[%d] ipv6 not supported", slot);
		fail_close(REPLY_BAD_ADDRESS);
		return;
	default:
		syslog.logf(LOG_ERR, "[%d] request ATYP 0x%x not supported",
		    slot, buf[3]);
		fail_close(REPLY_BAD_ADDRESS);
		return;
	}

	switch (buf[1]) {
	case REQUEST_COMMAND_CONNECT:
		// do_connect
		state = STATE_CONNECT;
		break;
	default:
		syslog.logf(LOG_ERR, "[%d] unsupported command 0x%x",
		    slot, buf[1]);
		fail_close(REPLY_BAD_COMMAND);
		return;
	}
}
