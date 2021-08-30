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

#pragma once

#include <WiFiClient.h>
#include <WiFiClientSecure.h>

class SocksClient : public WiFiClient {
public:
	virtual ~SocksClient();
	SocksClient(int _slot, WiFiClient _client);

	bool done();
	void process();

	bool tls() { return _tls; };
	int state;
	int slot;
	WiFiClient client_in;
	WiFiClient client_out;
	WiFiClientSecure client_out_tls;

private:
	void verify_method();
	bool verify_state(int _state);
	bool verify_version();
	void handle_request();
	void fail_close(char code);
	void connect();
	void proxy();
	void finish();

	void dump_buf(size_t len);

	unsigned char buf[64];
	unsigned char *remote_hostname;
	ip4_addr_t remote_ip;
	uint16_t remote_port;
	bool _tls;

	size_t buflen;
};
