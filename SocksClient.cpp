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

SocksClient::~SocksClient()
{
	client_in.stop();
	if (tls())
		client_out_tls.stop();
	else
		client_out.stop();
}

SocksClient::SocksClient(int _slot, WiFiClient _client)
    : slot(_slot), client_in(_client)
{
	state = STATE_INIT;

	memset(buf, 0, sizeof(buf));
	buflen = 0;
	remote_port = 0;
	ip4_addr_set_zero(&remote_ip);
	_tls = false;

#ifdef SOCKS_TRACE
	syslog.logf(LOG_DEBUG, "[%d] in socks client init with ip %s", slot,
	    client_in.remoteIP().toString().c_str());
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
		if (client_in.available() && sizeof(buf) - buflen > 0)
			buflen += client_in.read(buf + buflen,
			    sizeof(buf) - buflen);
		break;
	default:
		/* proxy() will do its own buffering */
		break;
	}

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

	client_in.write(msg, sizeof(msg));

	finish();
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

			client_in.write(msg, sizeof(msg));
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
	client_in.write(msg, sizeof(msg));
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

#ifdef SOCKS_TRACE
		syslog.logf(LOG_DEBUG, "[%d] CONNECT request to IP %s:%d",
		    slot, ipaddr_ntoa(&remote_ip), remote_port);
#endif

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
		    slot, buf[3]);
		fail_close(REPLY_BAD_ADDRESS);
		return;
	}

	switch (buf[1]) {
	case REQUEST_COMMAND_CONNECT:
		state = STATE_CONNECT;
		return;
	default:
		syslog.logf(LOG_ERR, "[%d] unsupported command 0x%x",
		    slot, buf[1]);
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
		syslog.logf(LOG_ERR, "[%d] bogus ip/port %s:%d",
		    slot, ipaddr_ntoa(&remote_ip), remote_port);
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
		client_out_tls.setInsecure();
		client_out_tls.setBufferSizes(1024, 1024);
#ifdef SOCKS_TRACE
		syslog.logf(LOG_DEBUG, "[%d] making TLS connection to %s:%d "
		    "with %d free mem", slot, ipaddr_ntoa(&remote_ip),
		    remote_port, ESP.getFreeHeap());
#endif
		ret = client_out_tls.connect(remote_ip, remote_port);
	} else
		ret = client_out.connect(remote_ip, remote_port);

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

	buflen = 0;
	client_in.write(msg, sizeof(msg));
	state = STATE_PROXY;
}

void
SocksClient::proxy()
{
	size_t len;

	if (!verify_state(STATE_PROXY))
		return;

	/*
	 * Process buffers before checking connection, we may have read some
	 * before the client closed.
	 */
	while (client_in.available()) {
		len = client_in.read(buf, sizeof(buf));
#ifdef SOCKS_TRACE
		syslog.logf(LOG_DEBUG, "[%d] read %d bytes from client in, "
		    "sending to client out %s", slot, len,
		    (tls() ? "tls" : ""));
		syslog_buf(buf, len);
#endif
		if (tls())
			client_out_tls.write(buf, len);
		else
			client_out.write(buf, len);
	}

	if (tls()) {
		while (client_out_tls.available()) {
			len = client_out_tls.read(buf, sizeof(buf));
#ifdef SOCKS_TRACE
			syslog.logf(LOG_DEBUG, "[%d] read %d bytes from "
			    "client out tls, sending to client in",
			    slot, len);
			syslog_buf(buf, len);
#endif
			client_in.write(buf, len);
		}
	} else {
		while (client_out.available()) {
			len = client_out.read(buf, sizeof(buf));
#ifdef SOCKS_TRACE
			syslog.logf(LOG_DEBUG, "[%d] read %d bytes from "
			    "client out, sending to client in", slot, len);
			syslog_buf(buf, len);
#endif
			client_in.write(buf, len);
		}
	}

	if (!client_in.connected()) {
#ifdef SOCKS_TRACE
		syslog.logf(LOG_DEBUG, "[%d] client in closed", slot);
#endif
		if (tls())
			client_out_tls.stop();
		else
			client_out.stop();
		finish();
		return;
	}

	if (tls() && !client_out_tls.connected()) {
#ifdef SOCKS_TRACE
		syslog.logf(LOG_DEBUG, "[%d] client out tls closed", slot);
#endif
		client_in.stop();
		finish();
		return;
	} else if (!tls() && !client_out.connected()) {
#ifdef SOCKS_TRACE
		syslog.logf(LOG_DEBUG, "[%d] client out closed", slot);
#endif
		client_in.stop();
		finish();
		return;
	}
}
