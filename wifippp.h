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

#ifndef __WIFIPPP_H__
#define __WIFIPPP_H__

#include <Arduino.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <Syslog.h>
#include <WiFiUdp.h>

#define WIFISTATION_VERSION	"0.5"

#define EEPROM_SIZE		512
struct __attribute((__packed__)) eeprom_data {
	char magic[3];
#define EEPROM_MAGIC_BYTES	"ppp"
	uint8_t revision;
#define EEPROM_REVISION		0
	char wifi_ssid[64];
	char wifi_pass[64];
	uint32_t baud;
	char telnet_tterm[32];
	uint8_t telnet_tts_w;
	uint8_t telnet_tts_h;
	uint8_t telnet;
#define BOOKMARK_SIZE 64
#define NUM_BOOKMARKS 3
	char bookmarks[NUM_BOOKMARKS][BOOKMARK_SIZE];
	char syslog_server[64];
	ip4_addr_t ppp_server_ip;
	ip4_addr_t ppp_client_ip;
	uint8_t reg_r;
#define REG_R_RTS_OFF		1
#define REG_R_RTS_ON		2
	uint8_t reg_i;
#define REG_I_XONXOFF_OFF	0
#define REG_I_XONXOFF_ON	1
};

enum {
	STATE_AT,
	STATE_TELNET,
	STATE_PPP,
};

extern uint8_t state;
extern struct eeprom_data *settings;
extern Syslog syslog;

#define MAX_UPLOAD_SIZE (16 * 1024)

/* ESP8266 pins for Adafruit Huzzah */
const int pRedLED  = 0;
const int pBlueLED = 2;
const int pRI      = 0;
const int pDCD     = 4;
const int pDTR     = 5;
const int pDSR     = 12;
const int pRTS     = 13;
const int pCTS     = 14;

/* ppp.cpp */
bool ppp_start(void);
void ppp_process(void);

/* serial.cpp */
void serial_setup(void);
void serial_begin(int baud);
uint8_t serial_read(void);
bool serial_available(void);
int16_t serial_peek(void);
void serial_write(char b);
void serial_flush(void);
long serial_autobaud(void);
void serial_cts(bool clear);
void serial_dcd(bool carrier);
void serial_dsr(bool ready);
bool serial_dtr(void);
void serial_ri(bool ringing);
bool serial_rts(void);

/* socks.cpp */
void socks_setup(void);
void socks_process(void);

/* telnet.cpp */
int telnet_connect(char *host, uint16_t port);
bool telnet_connected(void);
void telnet_disconnect(void);
int telnet_read(void);
int telnet_write(char b);
int telnet_write(String s);

/* update.cpp */
void update_process(bool do_update, bool force);

/* util.cpp */
void syslog_setup(void);
void led_setup(void);
void led_reset(void);
void error_flash(void);
size_t outputf(const char *format, ...);
int output(char c);
int output(const char *str);
int output(String str);

/* wifippp.ino */
void exec_cmd(char *cmd, size_t len);
extern bool serial_alive;

#endif
