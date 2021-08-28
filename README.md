## WiFiPPP

Yet another ESP8266-based WiFi serial modem emulator ROM featuring a Hayes
AT-style command interface that emulates a serial modem and allows old hardware
to "dial" telnet addresses rather than phone numbers.

It also includes a built-in PPP server that routes through the WiFi interface,
reachable with `ATDT PPP`.
Inside the PPP connection is a SOCKS5 proxy that can strip TLS from common
services like HTTPS and IMAPS, allowing old hardware to talk to modern, secure
services without needing to do any TLS.

### WiModem232

WiFiPPP currently targets the 
[WiModem232](https://www.cbmstuff.com/index.php?route=product/category&path=59_66)
hardware, which includes:

- ESP8266-12F WiFi module
- (Optional) I2C-based
  [SSD1306 monochrome OLED](https://amzn.to/3zs39OL)
  (with blue and yellow sections)
- [NeoPixel 5050 multi-color LED](https://www.adafruit.com/product/1655)
   with an integrated WS2811 controller

### Installation

For the WiModem232, installation will replace the factory ROM image, though no
hardware modification is needed.
The device can be revered back to stock by just flashing the
[factory ROM](http://www.cbmstuff.com/updates/wimodem232_V2_update.bin)
again.

This manual installation will only be required once, since future updates can
use the built-in `AT$UPDATE` command to do an over-the-air update over WiFi.

First, clone this Git repo and compile the WiFiPPP ROM.

	$ git clone --recursive https://github.com/jcs/WiFiPPP
	[...]
	$ cd WiFiPPP
	WiFiPPP$ gmake

On the back side of the WiModem232 (with the USB port facing down) are 5
vertical sockets.
From top to bottom, these are:

1. GPIO0 (to enter bootloader)
2. TX
3. RX
4. 3.3v VCC
5. GND

Connect a
[4- or 5- pin USB TTL serial cable](https://amzn.to/3kxsssl)
to the bottom 4 sockets.
It is recommended to use a breadboard so the 3.3V VCC and GND pins can be
connected to the breadboard's `+` and `-` rails, and then connect wires from
those rails to the sockets on the WiModem232.

Connect a jumper wire between the top port (GPIO0) port and the GND rail.

Connect another jumper wire to the GND rail which will be used to reset the
ESP8266.

Run `gmake install` to launch `esptool` and prepare to flash the WiFiPPP ROM
through the USB TTL cable.

Touch the jumper wire from the GND rail to the RST pin on the ESP8266, which is
the bottom-most pin along the right-hand side.
The blue LED will flash briefly, and if the GPIO0 socket is correctly connected
through your other jumper wire to GND, the ESP8266 will reboot to the
bootloader.

`esptool` should see this and start uploading the ROM automatically.

After flashing, disconnect the GPIO0 jumper wire or else it will reboot into
bootloader again.
Touch the RST pin again and the device should boot and show WiFiPPP on the OLED
screen (if your model has one).

In a serial terminal connected to your USB TTL device at any baud rate, you
should see some junk from the bootloader after rebooting.
Hit enter and type `AT` and it should echo back.

If all is working properly, you can disconnect all of your cables and connect
through the DB25 port (but not at the same time).
You'll need to re-establish your WiFi settings since the EEPROM layout from
WiFiPPP is not compatible with the factory WiModem232 settings.
