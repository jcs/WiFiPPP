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

void
pixel_setup(void)
{
	pixel.begin();
	pixel.clear();
	pixel.setBrightness(10);
	pixel.show();

	pixel_set_rgb(100, 100, 100);
}

void
pixel_set_rgb(int r, int g, int b)
{
	pixel.setPixelColor(0, pixel.Color(r, g, b));
	pixel.show();
}
