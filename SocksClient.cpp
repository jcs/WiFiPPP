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

unsigned int tls_ports[] = {
	443, /* https */
	993, /* imaps */
	995, /* pop3s */
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

#define REMOTE_CLIENT		(tls() ? remote_client_tls : remote_client)

static unsigned long last_buffer_check = 0;

SocksClient::~SocksClient()
{
	local_client.stop();
	REMOTE_CLIENT.stop();
}

SocksClient::SocksClient(int _slot, WiFiClient _client)
    : slot(_slot), local_client(_client)
{
	state = STATE_INIT;

	memset(local_buf, 0, sizeof(local_buf));
	memset(remote_buf, 0, sizeof(remote_buf));
	local_buf_len = 0;
	remote_buf_len = 0;
	remote_port = 0;
	ip4_addr_set_zero(&remote_ip);
	_tls = false;

#ifdef SOCKS_TRACE
	syslog.logf(LOG_DEBUG, "[%d] in socks client init with ip %s", slot,
	    local_client.remoteIP().toString().c_str());
#endif
}

bool
SocksClient::done()
{
	return (state == STATE_DEAD);
}

void
SocksClient::finish()
{
	state = STATE_DEAD;
}

void
SocksClient::process()
{
	switch (state) {
	case STATE_DEAD:
		return;
	case STATE_INIT:
	case STATE_METHOD:
	case STATE_REQUEST:
		if (local_client.available() &&
		    sizeof(local_buf) - local_buf_len > 0)
			local_buf_len += local_client.read(local_buf +
			    local_buf_len, sizeof(local_buf) - local_buf_len);
		break;
	default:
		/* proxy() will do its own buffering */
		break;
	}

	switch (state) {
	case STATE_INIT:
		if (local_buf_len >= METHOD_MIN_LENGTH) {
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
	case STATE_CONNECT:
		connect();
		break;
	case STATE_PROXY:
		proxy();
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

	local_client.write(msg, sizeof(msg));

	finish();
}

bool
SocksClient::verify_version()
{
	if (local_buf[0] != VERSION_SOCKS5) {
		syslog.logf(LOG_ERR, "[%d] unsupported version 0x%x", slot,
		    local_buf[0]);
		fail_close(REPLY_FAIL);
		return false;
	}

	return true;
}

bool
SocksClient::verify_state(int _state)
{
	if (state != _state) {
		syslog.logf(LOG_ERR, "[%d] in state %d but expected %d", slot,
		    state, _state);
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

	if (local_buf_len < METHOD_MIN_LENGTH)
		return;

	if (!verify_version())
		return;

	/* buf[1] is NMETHODS, find one we like */
	for (i = 0; i < (unsigned char)local_buf[1]; i++) {
		if (local_buf[2 + i] == METHOD_AUTH_NONE) {
			/* send back method selection */
			unsigned char msg[] = {
				VERSION_SOCKS5,
				METHOD_AUTH_NONE,
			};

			local_client.write(msg, sizeof(msg));
			state = STATE_REQUEST;
			local_buf_len = 0;
			return;
		}
	}

	syslog.logf(LOG_ERR, "[%d] no supported auth methods", slot);

	unsigned char msg[] = {
		VERSION_SOCKS5,
		METHOD_AUTH_BAD,
	};
	local_client.write(msg, sizeof(msg));
	fail_close(REPLY_FAIL);
}

void
SocksClient::handle_request()
{
	if (!verify_state(STATE_REQUEST))
		return;

	if (local_buf_len < METHOD_MIN_LENGTH)
		return;

	if (!verify_version())
		return;

	if (local_buf[1] != REQUEST_COMMAND_CONNECT) {
		syslog.logf(LOG_ERR, "[%d] unsupported request command 0x%x",
		    slot, local_buf[1]);
		fail_close(REPLY_BAD_COMMAND);
		return;
	}

	/* local_buf[2] is reserved */

	switch (local_buf[3]) {
	case REQUEST_ATYP_IP:
		if (local_buf_len < 4 + 4 + 2)
			return;

		IP4_ADDR(&remote_ip, local_buf[4], local_buf[5], local_buf[6],
		    local_buf[7]);
		remote_port = (uint16_t)((local_buf[8] & 0xff) << 8) |
		    (local_buf[9] & 0xff);

#ifdef SOCKS_TRACE
		syslog.logf(LOG_DEBUG, "[%d] CONNECT request to IP %s:%d",
		    slot, ipaddr_ntoa(&remote_ip), remote_port);
#endif

		break;
	case REQUEST_ATYP_HOSTNAME: {
		IPAddress resip;
		unsigned char hostlen;

		if (local_buf_len < 4 + 2)
			return;

		hostlen = local_buf[4];
		if (local_buf_len < (unsigned char)(4 + hostlen + 2))
			return;

		remote_hostname = (unsigned char *)malloc(hostlen + 1);
		if (!remote_hostname) {
			syslog.logf(LOG_ERR, "[%d] malloc(%d) failure",
			    slot, hostlen + 1);
			fail_close(REPLY_FAIL);
			return;
		}

		memcpy(remote_hostname, local_buf + 5, hostlen);
		remote_hostname[hostlen] = '\0';

		/* network order */
		remote_port = (uint16_t)((local_buf[5 + hostlen] & 0xff) << 8) |
		    (local_buf[5 + hostlen + 1] & 0xff);

		if (WiFi.hostByName((const char *)remote_hostname,
		    resip) != 1) {
			syslog.logf(LOG_ERR, "[%d] CONNECT request to "
			    "hostname %s:%d, couldn't resolve name",
			    slot, remote_hostname, remote_port);
			fail_close(REPLY_BAD_ADDRESS);
			return;
		}

		ip4_addr_set_u32(&remote_ip, resip.v4());

#ifdef SOCKS_TRACE
		syslog.logf(LOG_DEBUG, "[%d] CONNECT request to hostname "
		    "%s:%d, resolved to IP %s", slot, remote_hostname,
		    remote_port, ipaddr_ntoa(&remote_ip));
#endif
		break;
	}
	case REQUEST_ATYP_IP6:
		syslog.logf(LOG_ERR, "[%d] ipv6 not supported", slot);
		fail_close(REPLY_BAD_ADDRESS);
		return;
	default:
		syslog.logf(LOG_ERR, "[%d] request ATYP 0x%x not supported",
		    slot, local_buf[3]);
		fail_close(REPLY_BAD_ADDRESS);
		return;
	}

	switch (local_buf[1]) {
	case REQUEST_COMMAND_CONNECT:
		state = STATE_CONNECT;
		return;
	default:
		syslog.logf(LOG_ERR, "[%d] unsupported command 0x%x",
		    slot, local_buf[1]);
		fail_close(REPLY_BAD_COMMAND);
		return;
	}
}

void
SocksClient::connect()
{
	bool ret;

	_tls = false;

	if (!verify_state(STATE_CONNECT))
		return;

	if (remote_port == 0 || ip4_addr_isany_val(remote_ip)) {
		syslog.logf(LOG_ERR, "[%d] bogus ip/port %s:%d", slot,
		    ipaddr_ntoa(&remote_ip), remote_port);
		fail_close(REPLY_BAD_ADDRESS);
		return;
	}

	for (size_t i = 0; i < sizeof(tls_ports) / sizeof(tls_ports[0]); i++) {
		if (remote_port == tls_ports[i]) {
			_tls = true;
			break;
		}
	}

	if (tls()) {
		remote_client_tls.setInsecure();
		remote_client_tls.setBufferSizes(1024, 1024);
#ifdef SOCKS_TRACE
		syslog.logf(LOG_DEBUG, "[%d] making TLS connection to %s:%d "
		    "with %d free mem", slot, ipaddr_ntoa(&remote_ip),
		    remote_port, ESP.getFreeHeap());
#endif
	}

	ret = REMOTE_CLIENT.connect(remote_ip, remote_port);
	if (!ret) {
		syslog.logf(LOG_WARNING, "[%d] connection to %s:%d%s failed",
		    slot, ipaddr_ntoa(&remote_ip), remote_port,
		    (tls() ? " (TLS decrypt)" : ""));
		fail_close(REPLY_CONN_REFUSED);
		return;
	}

	unsigned char msg[] = {
		VERSION_SOCKS5, REPLY_SUCCESS, 0, REQUEST_ATYP_IP,
		ip4_addr1(&remote_ip), ip4_addr2(&remote_ip),
		ip4_addr3(&remote_ip), ip4_addr4(&remote_ip),
		(unsigned char)((remote_port >> 8) & 0xff),
		(unsigned char)(remote_port & 0xff)
	};

	local_buf_len = 0;
	local_client.write(msg, sizeof(msg));
	state = STATE_PROXY;
}

void
SocksClient::proxy()
{
	size_t len, wrote;

	if (!verify_state(STATE_PROXY))
		return;

	/*
	 * Process buffers before checking connection, we may have read some
	 * before the client closed.
	 */

	/* push out buffered data from remote to local client */
	if (remote_buf_len) {
		len = remote_buf_len;
		if (len > 64)
			len = 64;
		wrote = local_client.write(remote_buf, len);
		if (wrote) {
			memmove(remote_buf, remote_buf + wrote,
			    remote_buf_len - wrote);
			remote_buf_len -= wrote;
#ifdef SOCKS_TRACE
			syslog.logf(LOG_DEBUG, "[%d] wrote %d to local "
			    "(%d left)", slot, wrote, remote_buf_len);
#endif
		}
	}

	/* push out buffered data from local to remote client */
	if (local_buf_len) {
		wrote = REMOTE_CLIENT.write(local_buf, local_buf_len);
		if (wrote) {
			memmove(local_buf, local_buf + wrote,
			    local_buf_len - wrote);
			local_buf_len -= wrote;
#ifdef SOCKS_TRACE
			syslog.logf(LOG_DEBUG, "[%d] wrote %d to remote "
			    "(%d left)", slot, wrote, local_buf_len);
#endif
		}
	}

	/* buffer new data from local client */
	if (local_client.available() && local_buf_len < sizeof(local_buf)) {
		len = local_client.read(local_buf + local_buf_len,
		    sizeof(local_buf) - local_buf_len);
#ifdef SOCKS_TRACE
		syslog.logf(LOG_DEBUG, "[%d] read %d from local (now %d):",
		    slot, len, local_buf_len + len);
		syslog_buf((const char *)local_buf + local_buf_len, len);
#endif
		local_buf_len += len;
	}

	/* and then read in new data from remote if we have room */
	if (REMOTE_CLIENT.available() &&
	    (remote_buf_len < sizeof(remote_buf))) {
		len = REMOTE_CLIENT.read(remote_buf + remote_buf_len,
		    sizeof(remote_buf) - remote_buf_len);
#ifdef SOCKS_TRACE
		syslog.logf(LOG_DEBUG, "[%d] read %d from remote (now %d):",
		    slot, len, remote_buf_len + len);
		syslog_buf((const char *)remote_buf + remote_buf_len, len);
#endif
		remote_buf_len += len;
	}

#ifdef SOCKS_TRACE
	if (millis() - last_buffer_check > (3 * 1000)) {
		syslog.logf(LOG_DEBUG, "[%d] local:%d remote:%d free:%d", slot,
		    local_buf_len, remote_buf_len, ESP.getFreeHeap());
		last_buffer_check = millis();
	}
#endif

	if (!local_client.connected()) {
#ifdef SOCKS_TRACE
		syslog.logf(LOG_DEBUG, "[%d] local client closed", slot);
#endif
		REMOTE_CLIENT.stop();
		finish();
		return;
	}

	if (!REMOTE_CLIENT.connected()) {
#ifdef SOCKS_TRACE
		syslog.logf(LOG_DEBUG, "[%d] remote client closed", slot);
#endif
		local_client.stop();
		REMOTE_CLIENT.stop();
		finish();
		return;
	}
}
