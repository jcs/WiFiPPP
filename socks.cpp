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

#include "wifippp.h"
#include <WiFiClient.h>
#include <WiFiServer.h>
#include "SocksClient.h"

WiFiServer socks_server(1080);

#define MAX_SOCKS_CLIENTS 5
SocksClient *socks_clients[MAX_SOCKS_CLIENTS] = { nullptr };

void
socks_setup(void)
{
	socks_server.begin();
}

void
socks_process(void)
{
	int i;

	if (socks_server.hasClient()) {
		int slot = -1;

		for (i = 0; i < MAX_SOCKS_CLIENTS; i++) {
			if (!socks_clients[i] || socks_clients[i]->done()) {
				if (socks_clients[i])
					delete socks_clients[i];
				slot = i;
				break;
			}
		}

		syslog.logf(LOG_DEBUG, "new SOCKS client, slot %d", slot);

		if (slot > -1) {
			WiFiClient client = socks_server.available();
			if (client.connected())
				socks_clients[slot] = new SocksClient(slot,
				    client);
			else
				syslog.logf(LOG_ERR, "found slot %d for new "
				    "connection but not connected", slot);
		}
	}

	for (i = 0; i < MAX_SOCKS_CLIENTS; i++) {
		if (!socks_clients[i])
			continue;

		if (socks_clients[i]->done()) {
			delete socks_clients[i];
			socks_clients[i] = nullptr;
			continue;
		}

		socks_clients[i]->process();
	}
}
