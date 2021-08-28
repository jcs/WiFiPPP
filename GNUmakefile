# pkg_add makeesparduino

# ESP8266-12F
BOARD?= 	nodemcuv2

# default of -w supresses all warnings
COMP_WARNINGS=	-Wall -Wextra

BUILD_ROOT=	$(CURDIR)/obj
EXCLUDE_DIRS=	$(BUILD_ROOT)

LIBS=		Syslog Adafruit_BusIO Adafruit_SSD1306 Adafruit-GFX-Library \
		Adafruit_NeoPixel

ESP_ROOT?=	/usr/local/share/arduino/hardware/espressif/esp8266
ARDUINO_ROOT?=	/usr/local/share/arduino
ARDUINO_LIBS?=	${ESP_ROOT}/libraries

UPLOAD_PORT?=	/dev/cuaU1

include /usr/local/share/makeEspArduino/makeEspArduino.mk
