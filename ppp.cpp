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

u32_t ppp_output_cb(ppp_pcb *pcb, u8_t *data, u32_t len, void *ctx);
void ppp_status_cb(ppp_pcb* pcb, int err_code, void *ctx);
void ppp_setup_nat(struct netif *nif);

bool
ppp_start(void)
{
	ip4_addr_t s_addr, c_addr;

	_ppp = pppos_create(&ppp_netif, ppp_output_cb, ppp_status_cb, nullptr);
	if (!_ppp) {
		syslog.log(LOG_ERR, "pppos_create failed");
		return false;
	}

	ip_addr_copy(s_addr, settings->ppp_server_ip);
	ip_addr_copy(c_addr, settings->ppp_client_ip);

	ppp_set_ipcp_ouraddr(_ppp, &s_addr);
	ppp_set_ipcp_hisaddr(_ppp, &c_addr); /* or hers! */

	outputf("CONNECT %d %s:PPP\r\n", settings->baud, ipaddr_ntoa(&s_addr));

	ppp_listen(_ppp);

	syslog.log(LOG_INFO, "starting PPP negotiation");

	return true;
}

u32_t
ppp_output_cb(__attribute__((unused)) ppp_pcb *pcb, u8_t *data, u32_t len,
    __attribute__((unused)) void *ctx)
{
	return Serial.write(data, len);
}

void
ppp_process(void)
{
	size_t bytes;

	if (state != STATE_PPP) {
		syslog.logf(LOG_ERR, "%s but state is %d!", __func__, state);
		return;
	}

	bytes = Serial.available();
	if (!bytes)
		return;

	if (bytes > PPP_BUF_SIZE)
		bytes = PPP_BUF_SIZE;

	bytes = Serial.readBytes(ppp_buf, bytes);
	pppos_input(_ppp, ppp_buf, bytes);
}

void
ppp_status_cb(ppp_pcb *pcb, int err, __attribute__((unused)) void *ctx)
{
	struct netif *nif = ppp_netif(pcb);

	switch (err) {
	case PPPERR_NONE:
		syslog.log(LOG_DEBUG, "PPP session established");
		ppp_setup_nat(nif);
		break;
	case PPPERR_PARAM:
		/* Invalid parameter. */
		syslog.logf(LOG_ERR, "%s: PPPERR_PARAM", __func__);
		break;
	case PPPERR_OPEN:
		/* Unable to open PPP session. */
		syslog.logf(LOG_ERR, "%s: PPPERR_OPEN", __func__);
		break;
	case PPPERR_DEVICE:
		/* Invalid I/O device for PPP. */
		syslog.logf(LOG_ERR, "%s: PPPERR_DEVICE", __func__);
		break;
	case PPPERR_ALLOC:
		/* Unable to allocate resources. */
		syslog.logf(LOG_ERR, "%s: PPPERR_ALLOC", __func__);
		break;
	case PPPERR_USER:
		/* User interrupt. */
		syslog.logf(LOG_ERR, "%s: PPPERR_USER", __func__);
		break;
	case PPPERR_CONNECT:
		/* Connection lost. */
		syslog.logf(LOG_ERR, "%s: PPPERR_CONNECT", __func__);
		break;
	case PPPERR_AUTHFAIL:
		/* Failed authentication challenge. */
		syslog.logf(LOG_ERR, "%s: PPPERR_AUTHFAIL", __func__);
		break;
	case PPPERR_PROTOCOL:
		/* Failed to meet protocol. */
		syslog.logf(LOG_ERR, "%s: PPPERR_PROTOCOL", __func__);
		break;
	case PPPERR_PEERDEAD:
		/* Connection timeout. */
		syslog.logf(LOG_ERR, "%s: PPPERR_PEERDEAD", __func__);
		break;
	case PPPERR_IDLETIMEOUT:
		/* Idle Timeout. */
		syslog.logf(LOG_ERR, "%s: PPPERR_IDLETIMEOUT", __func__);
		break;
	case PPPERR_CONNECTTIME:
		/* Max connect time reached. */
		syslog.logf(LOG_ERR, "%s: PPPERR_CONNECTTIME", __func__);
		break;
	case PPPERR_LOOPBACK:
		/* Connection timeout. */
		syslog.logf(LOG_ERR, "%s: PPPERR_LOOPBACK", __func__);
		break;
	default:
		syslog.logf(LOG_ERR, "%s: unknown error %d", __func__, err);
		break;
	}

	if (err == PPPERR_NONE)
		return;

	if (err == PPPERR_USER) {
		syslog.log(LOG_DEBUG, "ending PPP session, "
		    "returning to AT mode");
		ppp_free(_ppp);
		_ppp = NULL;
		state = STATE_AT;
		outputf("\r\nNO CARRIER\r\n");
		return;
	}

	syslog.log(LOG_DEBUG, "closing PPP session");
	ppp_close(_ppp, 0);
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
