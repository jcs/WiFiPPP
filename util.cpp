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

struct eeprom_data *settings;

WiFiUDP syslogUDPClient;
Syslog syslog(syslogUDPClient, SYSLOG_PROTO_BSD);

void
setup(void)
{
	static_assert(sizeof(struct eeprom_data) < EEPROM_SIZE,
	    "EEPROM_SIZE is not large enough to hold struct eeprom_data");

	EEPROM.begin(EEPROM_SIZE);
	settings = (struct eeprom_data *)EEPROM.getDataPtr();
	if (memcmp(settings->magic, EEPROM_MAGIC_BYTES,
	    sizeof(settings->magic)) == 0) {
		/* do migrations if needed based on current revision */
		if (settings->revision != EEPROM_REVISION) {
			settings->revision = EEPROM_REVISION;
			EEPROM.commit();
		}
	} else {
		/* start over */
		memset(settings, 0, sizeof(struct eeprom_data));
		memcpy(settings->magic, EEPROM_MAGIC_BYTES,
		    sizeof(settings->magic));
		settings->revision = EEPROM_REVISION;

		settings->echo = 1;
		settings->quiet = 0;
		settings->verbal = 1;

		settings->baud = 9600;

		/* enable hardware flow control, disable software */
		settings->reg_r = REG_R_RTS_ON;
		settings->reg_i = REG_I_XONXOFF_OFF;

		settings->telnet = 1;
		strlcpy(settings->telnet_tterm, "ansi",
		    sizeof(settings->telnet_tterm));
		settings->telnet_tts_w = 80;
		settings->telnet_tts_h = 24;

		memset(settings->bookmarks, 0, BOOKMARK_SIZE * NUM_BOOKMARKS);
		strlcpy(settings->bookmarks[0], "klud.ge",
		    sizeof(settings->bookmarks[0]));

		IP4_ADDR(&settings->ppp_server_ip, 10, 10, 10, 10);
		IP4_ADDR(&settings->ppp_client_ip, 10, 10, 10, 20);

		settings->pixel_brightness = 10;

		EEPROM.commit();
	}

	syslog_setup();
	serial_setup();
	pixel_setup();
	screen_setup();

	WiFi.mode(WIFI_STA);

	/* don't require wifi_pass in case it's an open network */
	if (settings->wifi_ssid[0] == 0)
		WiFi.disconnect();
	else
		WiFi.begin(settings->wifi_ssid, settings->wifi_pass);

	socks_setup();

	serial_dsr(true);
	serial_cts(true);
}

void
syslog_setup(void)
{
	if (settings->syslog_server[0])
		syslog.server(settings->syslog_server, 514);
	else
		syslog.server(NULL, 514);

	syslog.appName("WiFiPPP");
}

size_t
outputf(const char *format, ...)
{
	va_list arg;
	char temp[64];
	char* buf;

	va_start(arg, format);
	size_t len = vsnprintf(temp, sizeof(temp), format, arg);
	va_end(arg);

	if (len > sizeof(temp) - 1) {
		/* too big for stack buffer, malloc something bigger */
		buf = (char *)malloc(len + 1);
		if (!buf)
			return 0;

		va_start(arg, format);
		vsnprintf(buf, len + 1, format, arg);
		va_end(arg);
	} else
		buf = temp;

	output(buf);

	if (buf != temp)
		free(buf);

	return len;
}

int
output(char c)
{
	serial_write(c);
	if (c == '\n')
		serial_flush();

	return 0;
}

int
output(const char *str)
{
	size_t len = strlen(str);

#ifdef OUTPUT_TRACE
	syslog.logf(LOG_DEBUG, "output: \"%s\"", str);
#endif

	for (size_t i = 0; i < len; i++)
		output(str[i]);

	return 0;
}

int
output(String str)
{
	size_t len = str.length();
	char *buf = (char *)malloc(len + 1);
	int ret;

	if (buf == NULL)
		return -1;

	str.toCharArray(buf, len);
	ret = output(buf);
	free(buf);

	return ret;
}

void
syslog_buf(const char *buf, size_t len)
{
	static char tbuf[(64 * 4) + 1];
	size_t tbufl = 0;

	if (len > 64)
		len = 64;

	for (size_t i = 0; i < len; i++) {
		if (buf[i] == '\n') {
			tbuf[tbufl++] = '\\';
			tbuf[tbufl++] = 'n';
		} else if (buf[i] == '\r') {
			tbuf[tbufl++] = '\\';
			tbuf[tbufl++] = 'r';
		} else if (buf[i] == ' ') {
			tbuf[tbufl++] = ' ';
		} else if (buf[i] < '!' || buf[i] > '~') {
			sprintf(tbuf + tbufl, "[%02x]", buf[i]);
			tbufl += 4;
		} else {
			tbuf[tbufl++] = buf[i];
		}
	}
	tbuf[tbufl] = '\0';

	syslog.log(LOG_DEBUG, tbuf);
}
