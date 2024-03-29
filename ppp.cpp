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

#include <lwip/napt.h>
#include <lwip/netif.h>
#include <netif/ppp/ppp.h>
#include <netif/ppp/pppos.h>

#include "wifippp.h"

#define PPP_BUF_SIZE 128
static uint8_t ppp_buf[PPP_BUF_SIZE];
static struct netif ppp_netif;
static ppp_pcb *_ppp = NULL;

#define PPP_TIMEOUT_SECS (60 * 10)
long last_ppp_input = 0;

u32_t ppp_output_cb(ppp_pcb *pcb, u8_t *data, u32_t len, void *ctx);
void ppp_status_cb(ppp_pcb* pcb, int err_code, void *ctx);
void ppp_setup_nat(struct netif *nif);

bool
ppp_start(void)
{
	ip4_addr_t s_addr, c_addr, d_addr;

	_ppp = pppos_create(&ppp_netif, ppp_output_cb, ppp_status_cb, nullptr);
	if (!_ppp) {
		syslog.log(LOG_ERR, "pppos_create failed");
		return false;
	}

	ip_addr_copy(s_addr, settings->ppp_server_ip);
	ip_addr_copy(c_addr, settings->ppp_client_ip);

	ppp_set_ipcp_ouraddr(_ppp, &s_addr);
	ppp_set_ipcp_hisaddr(_ppp, &c_addr); /* or hers! */

	ip4_addr_set_u32(&d_addr, WiFi.dnsIP());
	ppp_set_ipcp_dnsaddr(_ppp, 0, &d_addr);

	outputf("CONNECT %d %s:PPP\r\n", Serial.baudRate(),
	    ipaddr_ntoa(&s_addr));
	serial_dcd(true);

	last_ppp_input = millis();
	ppp_listen(_ppp);

#ifdef PPP_TRACE
	syslog.log(LOG_INFO, "starting PPP negotiation");
#endif

	return true;
}

u32_t
ppp_output_cb(__attribute__((unused)) ppp_pcb *pcb, u8_t *data, u32_t len,
    __attribute__((unused)) void *ctx)
{
#ifdef PPP_TRACE
	long now = millis();
#endif

	serial_write(data, len);

#ifdef PPP_TRACE
	long elap = millis();
	syslog.logf(LOG_DEBUG, "forwarded %ld PPP bytes out in %ldms", len,
	    elap - now);
#endif

	return len;
}

void
ppp_stop(bool wait)
{
	unsigned long now = millis();

	if (wait) {
		ppp_close(_ppp, 0);

		while (state == STATE_PPP && now - millis() < 1000) {
			pppos_input(_ppp, ppp_buf, 0);
			yield();
		}
	}

	if (state == STATE_PPP)
		ppp_close(_ppp, 1);
}

void
ppp_process(void)
{
	size_t bytes;
	long now = millis();

	if (state != STATE_PPP) {
		syslog.logf(LOG_ERR, "%s but state is %d!", __func__, state);
		return;
	}

	bytes = Serial.available();
	if (!bytes) {
		if (now - last_ppp_input > (1000 * PPP_TIMEOUT_SECS)) {
			syslog.logf(LOG_WARNING, "no PPP input in %ld secs, "
			    "hanging up", (now - last_ppp_input) / 1000);
			ppp_close(_ppp, 0);
			last_ppp_input = now;
		}
		return;
	}

	last_ppp_input = now;
	if (bytes > PPP_BUF_SIZE)
		bytes = PPP_BUF_SIZE;

	bytes = Serial.readBytes(ppp_buf, bytes);
	pppos_input(_ppp, ppp_buf, bytes);

#ifdef PPP_TRACE
	long elap = millis();
	syslog.logf(LOG_DEBUG, "processed %zu PPP input bytes in %ldms", bytes,
	    elap - now);
#endif
}

void
ppp_status_cb(ppp_pcb *pcb, int err, __attribute__((unused)) void *ctx)
{
	struct netif *nif = ppp_netif(pcb);

	switch (err) {
	case PPPERR_NONE:
		ppp_setup_nat(nif);
		syslog.logf(LOG_DEBUG, "PPP session established, free mem %d",
		    ESP.getFreeHeap());
		return;
	case PPPERR_USER:
#ifdef PPP_TRACE
		syslog.log(LOG_DEBUG, "ending PPP session, "
		    "returning to AT mode");
#endif
		ppp_free(_ppp);
		_ppp = NULL;
		state = STATE_AT;
		serial_dcd(false);
		outputf("\r\nNO CARRIER\r\n");
		return;
	default:
#ifndef PPP_TRACE
		if (err == PPPERR_USER)
			break;
#endif
		syslog.logf(LOG_ERR, "%s: err %d", __func__, err);

#ifdef PPP_TRACE
		syslog.log(LOG_DEBUG, "closing PPP session");
#endif
		ppp_close(_ppp, 0);
		break;
	}
}

void
ppp_setup_nat(struct netif *nif)
{
	err_t ret = ip_napt_init(IP_NAPT_MAX, IP_PORTMAP_MAX);
	if (ret == ERR_BUF) {
		/* already initialized */
	} else if (ret != ERR_OK) {
		syslog.logf(LOG_ERR, "NAPT initialization failed (%d)",
		    (int)ret);
		return;
	}

	ret = ip_napt_enable_no(nif->num, 1);
	if (ret != ERR_OK) {
		syslog.logf(LOG_INFO, "ip_napt_enable(%d) failed: %d",
		    nif->num, (int)ret);
		return;
	}

	/* forward port 22 on esp8266 to our client's PPP address */
	ret = ip_portmap_add(IP_PROTO_TCP, WiFi.localIP(), 22,
	    ip_2_ip4(&nif->gw)->addr, 22);
	if (ret != 1) {
		syslog.logf(LOG_ERR, "failed setting up NAPT portmap: %d",
		    (int)ret);
		return;
	}
}
