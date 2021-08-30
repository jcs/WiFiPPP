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

UPLOAD_PORT?=	/dev/cuaU0

SERVER_IP?=	`route -qn get 0.0.0.0 | grep 'address:' | sed 's/.*: //'`
SERVER_PORT?=	8000

include /usr/local/share/makeEspArduino/makeEspArduino.mk

serve_ota: all
	@[ -d release ] || mkdir release
	@grep _VERSION wifippp.h | sed -e 's/"$$//' -e 's/.*"//' > release/ota.txt
	@cp -f $(MAIN_EXE) release/update.bin
	@stat -f "%z" release/update.bin >> release/ota.txt
	@md5 -q release/update.bin >> release/ota.txt
	@echo http://$(SERVER_IP):$(SERVER_PORT)/update.bin >> release/ota.txt
	@echo ""
	@echo "Issue update command:"
	@echo ""
	@echo -n "AT$$"
	@echo "UPDATE! http://${SERVER_IP}:${SERVER_PORT}/ota.txt"
	@echo ""
	cd release && python3 -m http.server $(SERVER_PORT)
