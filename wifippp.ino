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

/*
 * Useful AT command sets to emulate:
 *
 * USRobotics Courier 56K Business Modem
 * http://web.archive.org/web/20161116174421/http://support.usr.com/support/3453c/3453c-ug/alphabetic.html
 *
 * Xecom XE3314L
 * http://web.archive.org/web/20210816224031/http://static6.arrow.com/aropdfconversion/63e466a4e0c7e004c40f79e4dbe4c1356a3dcef6/xe3314l.pdf
 */

#include "wifippp.h"

uint8_t state = STATE_AT;

static char curcmd[64] = { 0 };
static char lastcmd[64] = { 0 };
static unsigned int curcmdlen = 0;
static unsigned int lastcmdlen = 0;
static int plusses = 0;
static unsigned long plus_wait = 0;
static unsigned long last_dtr = 0;
static unsigned long last_autobaud = 0;

void
loop(void)
{
	int b = -1, i;
	long now = millis();
	bool hangup = false;

	socks_process();

	if (serial_dtr()) {
		if (!last_dtr) {
			/* new connection, re-autobaud */
			syslog.logf(LOG_DEBUG, "new connection with DTR, "
			    "doing auto-baud");
			serial_autobaud();
			now = millis();
			last_autobaud = now;
		}
		last_dtr = now;
	} else if (last_dtr && (now - last_dtr > 1750)) {
		/* had DTR, dropped it for 1.75 secs, hangup */
		hangup = true;
		last_dtr = 0;
		syslog.log(LOG_INFO, "dropped DTR, hanging up");
	}

	switch (state) {
	case STATE_AT:
		if (!(serial_available() && (b = serial_read())))
			return;

		/* USR modem mode, ignore input not starting with at or a/ */
		if (curcmdlen == 0 && (b != 'A' && b != 'a')) {
			if (b > 127 && (now - last_autobaud > 2000)) {
				serial_autobaud();
				now = millis();
				last_autobaud = now;
			}
			return;
		}

		if (curcmdlen == 1 && b == '/') {
			if (settings->echo)
				output("/\r");
			curcmd[0] = '\0';
			curcmdlen = 0;
			exec_cmd((char *)&lastcmd, lastcmdlen);
			break;
		} else if (curcmdlen == 1 && (b != 'T' && b != 't')) {
			if (settings->echo)
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
					int b2 = serial_peek();
					if (b2 == -1)
						continue;
					else if (b2 == '\n') {
						/* this is a \r\n, ignore \n */
						serial_read();
						break;
					} else {
						/* some other data */
						break;
					}
				}
			}
			output("\r");
			curcmd[curcmdlen] = '\0';
			exec_cmd((char *)&curcmd, curcmdlen);
			curcmd[0] = '\0';
			curcmdlen = 0;
			break;
		case '\b':
		case 127:
			if (curcmdlen) {
				if (settings->echo)
					output("\b \b");
				curcmdlen--;
			}
			break;
		default:
			curcmd[curcmdlen++] = b;
			if (settings->echo)
				output(b);
		}
		break;
	case STATE_TELNET:
		b = -1;

		if (hangup) {
			telnet_disconnect();
			break;
		}

		if (serial_available())
			b = serial_read();

		if (b == -1 && plus_wait > 0 && (millis() - plus_wait) >= 500) {
			/* received no input within 500ms of a plus */
			if (plusses >= 3) {
				state = STATE_AT;
				if (!settings->quiet) {
					if (settings->verbal)
						output("\nOK\r\n");
					else
						output("0\r");
				}
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
			serial_write(b);
			return;
		} else if (!telnet_connected()) {
			if (!settings->quiet) {
				if (settings->verbal)
					output("\r\nNO CARRIER\r\n");
				else
					output("3\r");
			}
			state = STATE_AT;
			break;
		}
		break;
	case STATE_PPP:
		if (hangup) {
			ppp_stop(true);
			break;
		}

		ppp_process();
		break;
	}
}

void
exec_cmd(char *cmd, size_t len)
{
	char *errstr = NULL;
	char *lcmd, *olcmd;
	char cmd_char;
	uint8_t cmd_num = 0;
	bool did_nl = false;

	lcmd = olcmd = (char *)malloc(len + 1);
	if (lcmd == NULL) {
		if (settings->verbal)
			outputf("ERROR malloc %zu failed\r\n", len);
		else
			output("4\r");
		return;
	}

	for (size_t i = 0; i < len; i++)
		lcmd[i] = tolower(cmd[i]);
	lcmd[len] = '\0';

	/* shouldn't be able to get here, but just in case */
	if (len < 2 || lcmd[0] != 'a' || lcmd[1] != 't') {
		errstr = strdup("not an AT command");
		goto error;
	}

	memcpy(&lastcmd, lcmd, len + 1);
	lastcmdlen = len;

	/* strip AT */
	cmd += 2;
	lcmd += 2;
	len -= 2;

	/* whether we printed a newline in our response */
	did_nl = false;

parse_cmd:
	if (lcmd[0] == '\0')
		goto done_parsing;

	/* remove command character */
	cmd_char = lcmd[0];
	len--;
	cmd++;
	lcmd++;

	/* find optional single digit after command, defaulting to 0 */
	cmd_num = 0;
	if (cmd[0] >= '0' && cmd[0] <= '9') {
		if (cmd[1] >= '0' && cmd[1] <= '9')
			/* nothing uses more than 1 digit */
			goto error;
		cmd_num = cmd[0] - '0';
		len--;
		cmd++;
		lcmd++;
	}

	switch (cmd_char) {
	case 'd': {
		char *host, *ohost, *bookmark;
		uint16_t port;
		int chars;
		int index;

		if (len < 2)
			goto error;

		switch (lcmd[0]) {
		case 't':
			/* ATDT: dial a host */
			host = ohost = (char *)malloc(len);
			if (host == NULL)
				goto error;
			host[0] = '\0';
			if (sscanf(lcmd, "t%[^:]:%hu%n", host, &port,
			    &chars) == 2 && chars > 0)
				/* matched host:port */
				;
			else if (sscanf(lcmd, "t%[^:]%n", host, &chars) == 1
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
			if (sscanf(lcmd, "s%d", &index) != 1)
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
		case 'p':
			if (strncmp(lcmd, "ppp", 3) == 0) {
				/*
				 * We can't easily support ATD because we allow
				 * hostnames, so if the user typed
				 * "atdtelnethost", we can't tell whether they
				 * meant "atd telnethost" or "atdt elnethost",
				 * but we can allow PPP since that is easy to
				 * check and will probably be done
				 * automatically by PPP software.
				 */
				host = ohost = (char *)malloc(4);
				if (host == NULL)
					goto error;
				sprintf(host, "ppp");
			} else
				goto error;
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

		/* no commands can follow */
		len = 0;

		if (strcasecmp(host, "ppp") == 0) {
			ip4_addr_t t_addr;
			ip_addr_copy(t_addr, settings->ppp_server_ip);
			if (!settings->quiet) {
				if (settings->verbal)
					outputf("\nDIALING %s:PPP\r\n",
					    ipaddr_ntoa(&t_addr));
			}

			telnet_disconnect();
			if (ppp_start())
				/* ppp_begin outputs CONNECT line, since it has
				 * to do so before calling ppp_listen */
				state = STATE_PPP;
			else if (!settings->quiet) {
				if (settings->verbal)
					output("NO ANSWER\r\n");
				else
					output("8\r");
			}
		} else {
			if (!settings->quiet && settings->verbal)
				outputf("\nDIALING %s:%d\r\n", host, port);

			if (telnet_connect(host, port) == 0) {
				if (!settings->quiet) {
					if (settings->verbal)
						outputf("CONNECT %d %s:%d\r\n",
			    			    Serial.baudRate(), host,
						    port);
					else
						output("18\r"); /* 57600 */
				}
				state = STATE_TELNET;
			} else if (!settings->quiet) {
				if (settings->verbal)
					output("\nNO ANSWER\r\n");
				else
					output("8\r");
			}
		}

		if (!settings->quiet)
			did_nl = true;

		free(ohost);
		break;
	}
	case 'e':
		/* ATE/ATE0 or ATE1: disable or enable echo */
		switch (cmd_num) {
		case 0:
			settings->echo = 0;
			break;
		case 1:
			settings->echo = 1;
			break;
		default:
			goto error;
		}
		break;
	case 'h':
		/* ATH/ATH0: hangup */
		switch (cmd_num) {
		case 0:
			telnet_disconnect();
			output("\r\n");
			break;
		default:
			goto error;
		}
		break;
	case 'i':
		/* ATI/ATI#: show information pages */
		switch (cmd_num) {
		case 0:
		case 3:
			/* ATI/ATI0/ATI3: show product name */
			outputf("\njcs WiFiPPP %s\r\n", WIFIPPP_VERSION);
			did_nl = true;
			break;
		case 1:
			/* ATI1: checksum of RAM (not used) */
			output("\n1337\r\n");
			did_nl = true;
			break;
		case 2:
			/* ATI2: test RAM (not used) */
			break;
		case 4: {
			/* ATI4: show settings */
			ip4_addr_t t_addr;

			output("\n");

			outputf("Firmware version:  %s\r\n", WIFIPPP_VERSION);

			outputf("Default baud rate: %d\r\n", settings->baud);
			outputf("Current baud rate: %d\r\n",
			    Serial.baudRate());

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

			ip_addr_copy(t_addr, settings->ppp_server_ip);
			outputf("PPP server:        %s\r\n",
			    ipaddr_ntoa(&t_addr));
			ip_addr_copy(t_addr, settings->ppp_client_ip);
			outputf("PPP client:        %s\r\n",
			    ipaddr_ntoa(&t_addr));

			outputf("Syslog server:     %s\r\n",
			    settings->syslog_server);

			for (int i = 0; i < NUM_BOOKMARKS; i++) {
				if (settings->bookmarks[i][0] != '\0')
					outputf("ATDS bookmark %d:   %s\r\n",
					    i + 1, settings->bookmarks[i]);
			}

			did_nl = true;
			break;
		}
		case 5: {
			/* ATI5: scan for wifi networks */
			int n = WiFi.scanNetworks();

			output("\n");

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
			did_nl = true;
			break;
		}
		default:
			goto error;
		}
		break;
	case 'o':
		/* ATO: go back online after a +++ */
		switch (cmd_num) {
		case 0:
			if (telnet_connected())
				state = STATE_TELNET;
			else
				goto error;
			break;
		default:
			goto error;
		}
		break;
	case 'q':
		/* ATQ/ATQ0 or ATQ1: enable or disable quiet */
		switch (cmd_num) {
		case 0:
			settings->quiet = 0;
			break;
		case 1:
		case 2:
			settings->quiet = 1;
			break;
		default:
			goto error;
		}
		break;
	case 'v':
		/* ATV/ATV0 or ATV1: enable or disable verbal responses */
		switch (cmd_num) {
		case 0:
			settings->verbal = 0;
			break;
		case 1:
			settings->verbal = 1;
			break;
		default:
			goto error;
		}
		break;
	case 'x':
		/* ATX/ATX#: ignore dialtone, certain results (not used) */
		break;
	case 'z':
		/* ATZ/ATZ0: restart */
		switch (cmd_num) {
		case 0:
			if (!settings->quiet) {
				if (settings->verbal)
					output("\nOK\r\n");
				else
					output("0\r");
			}
			ESP.restart();
			/* NOTREACHED */
		default:
			goto error;
		}
		break;
	case '$':
		/* wifi232 commands, all consume the rest of the input string */
		if (strcmp(lcmd, "net=0") == 0) {
			/* AT$NET=0: disable telnet setting */
			settings->telnet = 0;
		} else if (strcmp(lcmd, "net=1") == 0) {
			/* AT$NET=1: enable telnet setting */
			settings->telnet = 1;
		} else if (strcmp(lcmd, "net?") == 0) {
			/* AT$NET?: show telnet setting */
			outputf("\n%d\r\n", settings->telnet);
			did_nl = true;
		} else if (strncmp(lcmd, "pass=", 5) == 0) {
			/* AT$PASS=...: store wep/wpa passphrase */
			memset(settings->wifi_pass, 0,
			    sizeof(settings->wifi_pass));
			strncpy(settings->wifi_pass, cmd + 5,
			    sizeof(settings->wifi_pass));

			WiFi.disconnect();
			if (settings->wifi_ssid[0])
				WiFi.begin(settings->wifi_ssid,
				    settings->wifi_pass);
		} else if (strcmp(lcmd, "pass?") == 0) {
			/* AT$PASS?: print wep/wpa passphrase */
			outputf("\n%s\r\n", settings->wifi_pass);
			did_nl = true;
		} else if (strncmp(lcmd, "pppc=", 5) == 0) {
			/* AT$PPPC=...: store PPP client IP */
			ip4_addr_t t_addr;

			if (!ipaddr_aton(cmd + 5, &t_addr)) {
				errstr = strdup("invalid IP");
				goto error;
			}

			ip_addr_copy(settings->ppp_client_ip, t_addr);
		} else if (strcmp(lcmd, "pppc?") == 0) {
			/* AT$PPPC?: print PPP client IP */
			ip4_addr_t t_addr;
			ip_addr_copy(t_addr, settings->ppp_client_ip);
			outputf("\n%s\r\n", ipaddr_ntoa(&t_addr));
			did_nl = true;
		} else if (strncmp(lcmd, "ppps=", 5) == 0) {
			/* AT$PPPS=...: store PPP server IP */
			ip4_addr_t t_addr;

			if (!ipaddr_aton(cmd + 5, &t_addr)) {
				errstr = strdup("invalid IP");
				goto error;
			}

			ip_addr_copy(settings->ppp_server_ip, t_addr);
			/* re-bind to the new ip */
			socks_setup();
		} else if (strcmp(lcmd, "ppps?") == 0) {
			/* AT$PPPS?: print PPP server IP */
			ip4_addr_t t_addr;
			ip_addr_copy(t_addr, settings->ppp_server_ip);
			outputf("\n%s\r\n", ipaddr_ntoa(&t_addr));
			did_nl = true;
		} else if (strncmp(lcmd, "sb=", 3) == 0) {
			uint32_t baud = 0;
			int chars = 0;

			/* AT$SB=...: set baud rate */
			if (sscanf(lcmd, "sb=%d%n", &baud, &chars) != 1 ||
		    	    chars == 0) {
				if (settings->verbal)
					output("ERROR invalid baud rate\r\n");
				else
					output("4\r");
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
				if (!settings->quiet) {
					if (settings->verbal)
						outputf("\nOK switching to "
						    "%d\r\n", settings->baud);
					else
						output("0\r");
				}
				serial_flush();
				serial_start(settings->baud);
				break;
			default:
				output("ERROR unsupported baud rate\r\n");
				break;
			}
		} else if (strcmp(lcmd, "sb?") == 0) {
			/* AT$SB?: print baud rate */
			outputf("\n%d\r\n", settings->baud);
			did_nl = true;
		} else if (strncmp(lcmd, "ssid=", 5) == 0) {
			/* AT$SSID=...: set wifi ssid */
			memset(settings->wifi_ssid, 0,
			    sizeof(settings->wifi_ssid));
			strncpy(settings->wifi_ssid, cmd + 5,
			    sizeof(settings->wifi_ssid));

			WiFi.disconnect();
			if (settings->wifi_ssid[0])
				WiFi.begin(settings->wifi_ssid,
				    settings->wifi_pass);
		} else if (strcmp(lcmd, "ssid?") == 0) {
			/* AT$SSID?: print wifi ssid */
			outputf("\n%s\r\n", settings->wifi_ssid);
			did_nl = true;
		} else if (strncmp(lcmd, "syslog=", 7) == 0) {
			/* AT$SYSLOG=...: set syslog server */
			memset(settings->syslog_server, 0,
			    sizeof(settings->syslog_server));
			strncpy(settings->syslog_server, cmd + 7,
			    sizeof(settings->syslog_server));
			syslog_setup();
			syslog.logf(LOG_INFO, "syslog server changed to %s",
			    settings->syslog_server);
		} else if (strcmp(lcmd, "syslog?") == 0) {
			/* AT$SYSLOG?: print syslog server */
			outputf("\n%s\r\n", settings->syslog_server);
			did_nl = true;
		} else if (strncmp(lcmd, "tts=", 4) == 0) {
			/* AT$TTS=: set telnet NAWS */
			int w, h, chars;
			if (sscanf(lcmd + 4, "%dx%d%n", &w, &h, &chars) == 2 &&
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
			} else {
				errstr = strdup("must be WxH");
				goto error;
			}
		} else if (strcmp(lcmd, "tts?") == 0) {
			/* AT$TTS?: show telnet NAWS setting */
			outputf("\n%dx%d\r\n", settings->telnet_tts_w,
			    settings->telnet_tts_h);
			did_nl = true;
		} else if (strncmp(lcmd, "tty=", 4) == 0) {
			/* AT$TTY=: set telnet TTYPE */
			memset(settings->telnet_tterm, 0,
			    sizeof(settings->telnet_tterm));
			strncpy(settings->telnet_tterm, cmd + 4,
			    sizeof(settings->telnet_tterm));
		} else if (strcmp(lcmd, "tty?") == 0) {
			/* AT$TTY?: show telnet TTYPE setting */
			outputf("\n%s\r\n", settings->telnet_tterm);
			did_nl = true;
		} else if (strcmp(lcmd, "update?") == 0) {
			/* AT$UPDATE?: show whether an OTA update is available */
			update_process(false, false);
		} else if (strcmp(lcmd, "update!") == 0) {
			/* AT$UPDATE!: force an OTA update */
			update_process(true, true);
		} else if (strcmp(lcmd, "update") == 0) {
			/* AT$UPDATE: do an OTA update */
			update_process(true, false);
		} else
			goto error;

		/* consume all chars */
		len = 0;
		break;
	case '&':
		if (cmd[0] == '\0')
			goto error;

		cmd_char = lcmd[0];
		len--;
		cmd++;
		lcmd++;

		/* find optional single digit after &command, defaulting to 0 */
		cmd_num = 0;
		if (cmd[0] >= '0' && cmd[0] <= '9') {
			if (cmd[1] >= '0' && cmd[1] <= '9')
				/* nothing uses more than 1 digit */
				goto error;
			cmd_num = cmd[0] - '0';
			len--;
			cmd++;
			lcmd++;
		}

		switch (cmd_char) {
		case 'w':
			switch (cmd_num) {
			case 0:
				/* AT&W: save settings */
				if (!EEPROM.commit())
					goto error;
				break;
			default:
				goto error;
			}
			break;
		case 'z': {
			/* AT&Z: manage bookmarks */
			uint32_t index = 0;
			uint8_t query;
			int chars = 0;

			if (sscanf(lcmd, "%u=%n", &index, &chars) == 1 &&
			    chars > 0) {
				/* AT&Zn=...: store address */
				query = 0;
			} else if (sscanf(lcmd, "%u?%n", &index, &chars) == 1 &&
			    chars > 0) {
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
				outputf("\n%s\r\n",
				    settings->bookmarks[index - 1]);
				did_nl = true;
			} else {
				memset(settings->bookmarks[index - 1], 0,
				    sizeof(settings->bookmarks[0]));
				strncpy(settings->bookmarks[index - 1],
				    cmd + 2,
				    sizeof(settings->bookmarks[0]) - 1);
			}

			/* consume all chars */
			len = 0;
			break;
		}
		default:
			goto error;
		}
		break;
	default:
		goto error;
	}

done_parsing:
	/* if any len left, parse as another command */
	if (len > 0)
		goto parse_cmd;

	if (olcmd)
		free(olcmd);

	if (state == STATE_AT && !settings->quiet) {
		if (settings->verbal)
			outputf("%sOK\r\n", did_nl ? "" : "\n");
		else
			output("0\r");
	}

	return;

error:
	if (olcmd)
		free(olcmd);

	if (!settings->quiet) {
		if (settings->verbal) {
			output("\nERROR");
			if (errstr != NULL)
				outputf(" %s", errstr);
			output("\r\n");
		} else
			output("4\r");
	}

	if (errstr != NULL)
		free(errstr);
}
