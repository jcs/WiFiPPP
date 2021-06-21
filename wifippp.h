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
};

extern struct eeprom_data *settings;

#define MAX_UPLOAD_SIZE (16 * 1024)

/* ESP8266 pins */
const int pBlueLED = 16;

/* wifippp.ino */
void exec_cmd(char *cmd, size_t len);
extern bool serial_alive;

/* util.cpp */
void led_setup(void);
void led_reset(void);
void error_flash(void);
size_t outputf(const char *format, ...);
int output(char c);
int output(const char *str);
int output(String str);

/* telnet.cpp */
int telnet_connect(char *host, uint16_t port);
bool telnet_connected(void);
void telnet_disconnect(void);
int telnet_read(void);
int telnet_write(char b);
int telnet_write(String s);

/* update.cpp */
void update_process(bool do_update, bool force);

#endif
