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

void
serial_setup(void)
{
	/* DCE mode, acting as a modem */

	serial_start(settings->baud);

	pinMode(pRI, OUTPUT);
	serial_ri(false);

	pinMode(pDSR, OUTPUT);
	serial_dsr(false);

	pinMode(pRTS, INPUT);

	pinMode(pCTS, OUTPUT);
	serial_cts(false);

	pinMode(pDCD, OUTPUT);
	serial_dcd(false);

	pinMode(pDTR, INPUT);
}

void
serial_start(int baud)
{
	Serial.begin(baud);
}

void
serial_write(unsigned char b)
{
	serial_write(&b, 1);
}

void
serial_write(unsigned char *data, size_t len)
{
	size_t i;
	for (i = 0; i < len; i++) {
		if (i % 8 == 0 && settings->reg_r == REG_R_RTS_ON) {
			while (!serial_rts())
				yield();
		}
		Serial.write(data[i]);
	}
}

void
serial_flush(void)
{
	Serial.flush();
}

bool
serial_available(void)
{
	return Serial.available();
}

uint8_t
serial_read(void)
{
	return Serial.read();
}

int16_t
serial_peek(void)
{
	return Serial.peek();
}

long
serial_autobaud(void)
{
	Serial.begin(115200);

	unsigned long baud = Serial.detectBaudrate(30000);
	if (baud)
		syslog.logf(LOG_INFO, "auto-detected baud rate of %lu", baud);
	else {
		syslog.logf(LOG_INFO, "couldn't auto-detect baud rate, "
		    "using %d", settings->baud);
		baud = settings->baud;
	}

	Serial.begin(baud);

	/* terminal program will probably look for an AT */
	if (!settings->quiet) {
		if (settings->verbal)
			output("\r\nOK\r\n");
		else
			output("\r0\r");
	}

	return baud;
}

/* Clear to Send */
void
serial_cts(bool clear)
{
	digitalWrite(pCTS, clear ? LOW : HIGH); /* inverted */
}

/* Data Carrier Detect */
void
serial_dcd(bool carrier)
{
	digitalWrite(pDCD, carrier ? LOW : HIGH); /* inverted */
}

/* Data Set Ready */
void
serial_dsr(bool ready)
{
	digitalWrite(pDSR, ready ? LOW : HIGH); /* inverted */
}

/* Data Terminal Ready */
bool
serial_dtr(void)
{
	return (digitalRead(pDTR) == LOW); /* inverted */
}

/* Ring Indicator */
void
serial_ri(bool ringing)
{
	digitalWrite(pRI, ringing ? LOW : HIGH); /* inverted */
}

/* Ready to Send */
bool
serial_rts(void)
{
	return (digitalRead(pRTS) == LOW); /* inverted */
}
