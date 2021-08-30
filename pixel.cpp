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
#include <Adafruit_NeoPixel.h>

Adafruit_NeoPixel pixel(1, pPixel, NEO_GRB + NEO_KHZ800);
static uint32_t cur_color = 0;
static int wifi_status = WL_DISCONNECTED;

void
pixel_setup(void)
{
	pixel.begin();
	pixel.clear();
	pixel.setBrightness(10);
	pixel.show();

	/* default to red */
	pixel_set_rgb(255, 0, 0);
}

void
pixel_set_rgb(int r, int g, int b)
{
	pixel_set_rgb(pixel.Color(r, g, b));
}

void
pixel_set_rgb(uint32_t color)
{
#ifdef PIXEL_TRACE
	syslog.logf(LOG_DEBUG, "pixel: changing color from %d to %d",
	    cur_color, color);
#endif
	cur_color = color;
	pixel.setPixelColor(0, cur_color);
	pixel.show();
}

void
pixel_color_by_state(void)
{
	uint32_t color = cur_color;

	if (WiFi.status() != wifi_status) {
		switch (WiFi.status()) {
		case WL_CONNECTED:
			/* yellow */
			color = pixel.Color(255, 255, 0);
			break;
		default:
			/* red */
			color = pixel.Color(255, 0, 0);
		}
		wifi_status = WiFi.status();
	} else {
		switch (state) {
		case STATE_AT:
			if (telnet_connected())
				/* teal */
				color = pixel.Color(0, 255, 255);
			else
				/* yellow */
				color = pixel.Color(255, 255, 0);
			break;
		case STATE_TELNET:
			/* green */
			color = pixel.Color(0, 255, 0);
			break;
		case STATE_UPDATING:
			/* purple */
			color = pixel.Color(255, 0, 255);
			break;
		}
	}

	if (color != cur_color)
		pixel_set_rgb(color);
}
