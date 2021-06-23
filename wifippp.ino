/*
 * WiFiStation
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

enum {
	STATE_AT,
	STATE_TELNET,
};

static char curcmd[128] = { 0 };
static char lastcmd[128] = { 0 };
static unsigned int curcmdlen = 0;
static unsigned int lastcmdlen = 0;
static uint8_t state = STATE_AT;
static int plusses = 0;
static unsigned long plus_wait = 0;

void
loop(void)
{
	int b = -1, i;
	long now = millis();

	switch (state) {
	case STATE_AT:
		if (Serial.available() && (b = Serial.read()))
			serial_alive = true;
		else
			return;

		/* USR modem mode, ignore input not starting with at or a/ */
		if (curcmdlen == 0 && (b != 'A' && b != 'a')) {
			return;
		} else if (curcmdlen == 1 && b == '/') {
			output("/\r\n");
			curcmd[0] = '\0';
			curcmdlen = 0;
			exec_cmd((char *)&lastcmd, lastcmdlen);
			break;
		} else if (curcmdlen == 1 && (b != 'T' && b != 't')) {
			output("\b \b");
			curcmdlen = 0;
			return;
		}

		switch (b) {
		case '\n':
		case '\r':
			if (b == '\r') {
				/* if sender is using \r\n, ignore the \n */
				now = millis();
				while (millis() - now < 10) {
					int b2 = Serial.peek();
					if (b2 == -1)
						continue;
					else if (b2 == '\n') {
						/* this is a \r\n, ignore \n */
						Serial.read();
						break;
					} else {
						/* some other data */
						break;
					}
				}
			}
			output("\r\n");
			curcmd[curcmdlen] = '\0';
			exec_cmd((char *)&curcmd, curcmdlen);
			curcmd[0] = '\0';
			curcmdlen = 0;
			break;
		case '\b':
		case 127:
			if (curcmdlen) {
				output("\b \b");
				curcmdlen--;
			}
			break;
		default:
			curcmd[curcmdlen++] = b;
			output(b);
		}
		break;
	case STATE_TELNET:
		b = -1;

		if (Serial.available() && (b = Serial.read()))
			serial_alive = true;

		if (b == -1 && plus_wait > 0 && (millis() - plus_wait) >= 500) {
			/* received no input within 500ms of a plus */
			if (plusses >= 3) {
				state = STATE_AT;
				output("\r\nOK\r\n");
			} else {
				/* cancel, flush any plus signs received */
				for (i = 0; i < plusses; i++)
					telnet_write("+");
			}
			plusses = 0;
			plus_wait = 0;
		} else if (b != -1) {
			if (b == '+') {
				plusses++;
				plus_wait = millis();
				break;
			}

			if (plusses) {
				for (i = 0; i < plusses; i++)
					telnet_write("+");
				plusses = 0;
			}
			plus_wait = 0;
			telnet_write(b);
			break;
		}

		if ((b = telnet_read()) != -1) {
			if (serial_alive)
				Serial.write(b);
			return;
		} else if (!telnet_connected()) {
			output("\r\nNO CARRIER\r\n");
			state = STATE_AT;
			break;
		}
		break;
	}
}

void
exec_cmd(char *cmd, size_t len)
{
	char *errstr = NULL;

	char *lcmd = (char *)malloc(len + 1);
	if (lcmd == NULL) {
		outputf("ERROR malloc %zu failed\r\n", len);
		return;
	}

	for (size_t i = 0; i < len; i++)
		lcmd[i] = tolower(cmd[i]);
	lcmd[len] = '\0';

	if (len < 2 || lcmd[0] != 'a' || lcmd[1] != 't') {
		errstr = strdup("not an AT command");
		goto error;
	}

	memcpy(&lastcmd, lcmd, len + 1);
	lastcmdlen = len;

	if (len == 2) {
		output("OK\r\n");
		return;
	}

	switch (lcmd[2]) {
	case 'd': {
		char *host, *ohost, *bookmark;
		uint16_t port;
		int chars;
		int index;

		if (len < 5)
			goto error;

		switch (lcmd[3]) {
		case 't':
			/* ATDT: dial a host */
			host = ohost = (char *)malloc(len);
			if (host == NULL)
				goto error;
			host[0] = '\0';
			if (sscanf(lcmd, "atdt%[^:]:%hu%n", host, &port,
			    &chars) == 2 && chars > 0)
				/* matched host:port */
				;
			else if (sscanf(lcmd, "atdt%[^:]%n", host, &chars) == 1
			    && chars > 0)
				/* host without port */
				port = 23;
			else {
				errstr = strdup("invalid hostname");
				goto error;
			}
			break;
		case 's':
			/* ATDS: dial a stored host */
			if (sscanf(lcmd, "atds%d", &index) != 1)
				goto error;

			if (index < 1 || index > NUM_BOOKMARKS) {
				errstr = strdup("invalid index");
				goto error;
			}

			bookmark = settings->bookmarks[index - 1];

			host = ohost = (char *)malloc(BOOKMARK_SIZE);
			if (host == NULL)
				goto error;

			host[0] = '\0';

			if (sscanf(bookmark, "%[^:]:%hu%n", host, &port,
			    &chars) == 2 && chars > 0)
				/* matched host:port */
				;
			else if (sscanf(bookmark, "%[^:]%n", host, &chars) == 1
			    && chars > 0)
				/* host without port */
				port = 23;
			else {
				errstr = strdup("invalid hostname");
				goto error;
			}
			break;
		default:
			goto error;
		}

		/* skip leading spaces */
		while (host[0] == ' ')
			host++;

		if (host[0] == '\0') {
			errstr = strdup("blank hostname");
			goto error;
		}

		outputf("DIALING %s:%d\r\n", host, port);

		if (telnet_connect(host, port) == 0) {
			outputf("CONNECT %d %s:%d\r\n", settings->baud, host,
			    port);
			state = STATE_TELNET;
		} else {
			output("NO ANSWER\r\n");
		}

		free(ohost);

		break;
	}
	case 'h':
		telnet_disconnect();
		output("OK\r\n");
		break;
	case 'i':
		if (len > 4)
			goto error;

		switch (len == 3 ? '0' : cmd[3]) {
		case '0':
			/* ATI or ATI0: show settings */
			outputf("Firmware version:  %s\r\n",
			    WIFISTATION_VERSION);
			outputf("Serial baud rate:  %d\r\n",
			    settings->baud);
			outputf("Default WiFi SSID: %s\r\n",
			    settings->wifi_ssid);
			outputf("Current WiFi SSID: %s\r\n", WiFi.SSID());
			outputf("WiFi connected:    %s\r\n",
			    WiFi.status() == WL_CONNECTED ? "yes" : "no");
			if (WiFi.status() == WL_CONNECTED) {
				outputf("IP address:        %s\r\n",
				    WiFi.localIP().toString().c_str());
				outputf("Gateway IP:        %s\r\n",
				    WiFi.gatewayIP().toString().c_str());
				outputf("DNS server IP:     %s\r\n",
				    WiFi.dnsIP().toString().c_str());
			}
			for (int i = 0; i < NUM_BOOKMARKS; i++) {
				if (settings->bookmarks[i][0] != '\0')
					outputf("ATDS bookmark %d:   %s\r\n",
					    i + 1, settings->bookmarks[i]);
			}
			output("OK\r\n");
			break;
		case '1': {
			/* ATI1: scan for wifi networks */
			int n = WiFi.scanNetworks();

			for (int i = 0; i < n; i++) {
				outputf("%02d: %s (chan %d, %ddBm, ",
				    i + 1,
				    WiFi.SSID(i).c_str(),
				    WiFi.channel(i),
				    WiFi.RSSI(i));

				switch (WiFi.encryptionType(i)) {
				case ENC_TYPE_WEP:
					output("WEP");
					break;
				case ENC_TYPE_TKIP:
					output("WPA-PSK");
					break;
				case ENC_TYPE_CCMP:
					output("WPA2-PSK");
					break;
				case ENC_TYPE_NONE:
					output("NONE");
					break;
				case ENC_TYPE_AUTO:
					output("WPA-PSK/WPA2-PSK");
					break;
				default:
					outputf("?(%d)",
					    WiFi.encryptionType(i));
				}

				output(")\r\n");
			}
			output("OK\r\n");
			break;
		}
		case '3':
			/* ATI3: show version */
			outputf("%s\r\nOK\r\n", WIFISTATION_VERSION);
			break;
		default:
			goto error;
		}
		break;
	case 'o':
		if (telnet_connected())
			state = STATE_TELNET;
		else
			goto error;
		break;
	case 'z':
		output("OK\r\n");
		ESP.restart();
		break;
	case '$':
		/* wifi232 commands */

		if (strcmp(lcmd, "at$net=0") == 0) {
			/* AT$NET=0: disable telnet setting */
			settings->telnet = 0;
			output("OK\r\n");
		} else if (strcmp(lcmd, "at$net=1") == 0) {
			/* AT$NET=1: enable telnet setting */
			settings->telnet = 1;
			output("OK\r\n");
		} else if (strcmp(lcmd, "at$net?") == 0) {
			/* AT$NET?: show telnet setting */
			outputf("%d\r\nOK\r\n", settings->telnet);
		} else if (strncmp(lcmd, "at$pass=", 8) == 0) {
			/* AT$PASS=...: store wep/wpa passphrase */
			memset(settings->wifi_pass, 0,
			    sizeof(settings->wifi_pass));
			strncpy(settings->wifi_pass, cmd + 8,
			    sizeof(settings->wifi_pass));
			output("OK\r\n");

			WiFi.disconnect();
			if (settings->wifi_ssid[0])
				WiFi.begin(settings->wifi_ssid,
				    settings->wifi_pass);
		} else if (strcmp(lcmd, "at$pass?") == 0) {
			/* AT$PASS?: print wep/wpa passphrase */
			outputf("%s\r\nOK\r\n", settings->wifi_pass);
		} else if (strncmp(lcmd, "at$sb=", 6) == 0) {
			uint32_t baud = 0;
			int chars = 0;

			/* AT$SB=...: set baud rate */
			if (sscanf(lcmd, "at$sb=%d%n", &baud, &chars) != 1 ||
		    	    chars == 0) {
				output("ERROR invalid baud rate\r\n");
				break;
			}

			switch (baud) {
			case 110:
			case 300:
			case 1200:
			case 2400:
			case 4800:
			case 9600:
			case 19200:
			case 38400:
			case 57600:
			case 115200:
				settings->baud = baud;
				outputf("OK switching to %d\r\n",
				    settings->baud);
				Serial.flush();
				Serial.begin(settings->baud);
				break;
			default:
				output("ERROR unsupported baud rate\r\n");
				break;
			}
		} else if (strcmp(lcmd, "at$sb?") == 0) {
			/* AT$SB?: print baud rate */
			outputf("%d\r\nOK\r\n", settings->baud);
		} else if (strncmp(lcmd, "at$ssid=", 8) == 0) {
			/* AT$SSID=...: set wifi ssid */
			memset(settings->wifi_ssid, 0,
			    sizeof(settings->wifi_ssid));
			strncpy(settings->wifi_ssid, cmd + 8,
			    sizeof(settings->wifi_ssid));
			output("OK\r\n");

			WiFi.disconnect();
			if (settings->wifi_ssid[0])
				WiFi.begin(settings->wifi_ssid,
				    settings->wifi_pass);
		} else if (strcmp(lcmd, "at$ssid?") == 0) {
			/* AT$SSID?: print wifi ssid */
			outputf("%s\r\nOK\r\n", settings->wifi_ssid);
		} else if (strncmp(lcmd, "at$tts=", 7) == 0) {
			/* AT$TTS=: set telnet NAWS */
			int w, h, chars;
			if (sscanf(lcmd + 7, "%dx%d%n", &w, &h, &chars) == 2 &&
			    chars > 0) {
				if (w < 1 || w > 255) {
					errstr = strdup("invalid width");
					goto error;
				}
				if (h < 1 || h > 255) {
					errstr = strdup("invalid height");
					goto error;
				}

				settings->telnet_tts_w = w;
				settings->telnet_tts_h = h;
				output("OK\r\n");
			} else {
				errstr = strdup("must be WxH");
				goto error;
			}
		} else if (strcmp(lcmd, "at$tts?") == 0) {
			/* AT$TTS?: show telnet NAWS setting */
			outputf("%dx%d\r\nOK\r\n", settings->telnet_tts_w,
			    settings->telnet_tts_h);
		} else if (strncmp(lcmd, "at$tty=", 7) == 0) {
			/* AT$TTY=: set telnet TTYPE */
			memset(settings->telnet_tterm, 0,
			    sizeof(settings->telnet_tterm));
			strncpy(settings->telnet_tterm, cmd + 7,
			    sizeof(settings->telnet_tterm));
			output("OK\r\n");
		} else if (strcmp(lcmd, "at$tty?") == 0) {
			/* AT$TTY?: show telnet TTYPE setting */
			outputf("%s\r\nOK\r\n", settings->telnet_tterm);
		} else if (strcmp(lcmd, "at$update?") == 0) {
			/* AT$UPDATE?: show whether an OTA update is available */
			update_process(false, false);
		} else if (strcmp(lcmd, "at$update!") == 0) {
			/* AT$UPDATE!: force an OTA update */
			update_process(true, true);
		} else if (strcmp(lcmd, "at$update") == 0) {
			/* AT$UPDATE: do an OTA update */
			update_process(true, false);
		} else
			goto error;
		break;
	case '&':
		if (len < 4)
			goto error;

		switch (lcmd[3]) {
		case 'w':
			/* AT&W: save settings */
			if (len != 4)
				goto error;

			if (!EEPROM.commit())
				goto error;

			output("OK\r\n");
			break;
		case 'z': {
			/* AT&Z: manage bookmarks */
			uint32_t index = 0;
			uint8_t query;
			int chars = 0;

			if (sscanf(lcmd, "at&z%u=%n", &index, &chars) == 1 &&
			    chars > 0) {
				/* AT&Zn=...: store address */
				query = 0;
			} else if (sscanf(lcmd, "at&z%u?%n", &index,
			    &chars) == 1 && chars > 0) {
				/* AT&Zn?: query stored address */
				query = 1;
			} else {
				errstr = strdup("invalid store command");
				goto error;
			}

			if (index < 1 || index > NUM_BOOKMARKS) {
				errstr = strdup("invalid index");
				goto error;
			}

			if (query) {
				outputf("%s\r\nOK\r\n",
				    settings->bookmarks[index - 1]);
			} else {
				memset(settings->bookmarks[index - 1], 0,
				    sizeof(settings->bookmarks[0]));
				strncpy(settings->bookmarks[index - 1],
				    cmd + 6,
				    sizeof(settings->bookmarks[0]) - 1);
				output("OK\r\n");
			}
			break;
		}
		default:
			goto error;
		}
		break;
	default:
		goto error;
	}

	if (lcmd)
		free(lcmd);
	return;

error:
	if (lcmd)
		free(lcmd);

	output("ERROR");
	if (errstr != NULL) {
		outputf(" %s", errstr);
		free(errstr);
	}
	output("\r\n");
}
