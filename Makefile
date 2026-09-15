# ═══════════════════════════════════════════════════════════════════════════
#                  DIESEL PILOT — PlatformIO wrapper
# ═══════════════════════════════════════════════════════════════════════════
# Command list:  make  (or make help)
#
# Local values — device IP, passwords — belong in Makefile.local. That file
# is in .gitignore and never reaches the repository. Example contents:
#
#     IP        = 192.168.1.50
#     OTA_PASS  = secret
#     MQTT_HOST = mqtt.example.com
#     MQTT_USER = diesel
#     MQTT_PASS = secret
#
# Any variable can be overridden from the command line:
#     make ota IP=192.168.1.77
# ═══════════════════════════════════════════════════════════════════════════

-include Makefile.local

PIO ?= pio
ENV ?= esp32dev

# Device address on the network — used for OTA and HTTP requests
IP ?=
# OTA password (empty = flashing without a password)
OTA_PASS ?=
# Serial port (empty = PlatformIO autodetection)
PORT ?=

MQTT_HOST ?=
MQTT_PORT ?= 1883
MQTT_TOPIC ?= diesel
MQTT_USER ?=
MQTT_PASS ?=
# Command for mqtt-cmd: power | up | down | mode
C ?= power

UPLOAD_PORT = $(if $(PORT),--upload-port $(PORT))
MON_PORT    = $(if $(PORT),-p $(PORT))
OTA_FLAGS   = $(if $(OTA_PASS),--upload-flags --auth=$(OTA_PASS))
MQTT_AUTH   = $(if $(MQTT_USER),-u $(MQTT_USER) -P $(MQTT_PASS))

.DEFAULT_GOAL := help
.PHONY: help build fs size test lint clean flash flash-fs flash-all monitor dev \
        ports erase ota ota-fs ota-all status ping mqtt-watch mqtt-cmd \
        require-ip require-mqtt

##@ Help

help: ## Show this list
	@echo "DieselPilot — available commands:"
	@awk 'BEGIN{FS=":.*## "} \
	     /^##@/{printf "\n\033[1m%s\033[0m\n", substr($$0,5)} \
	     /^[a-z][a-zA-Z0-9_-]*:.*## /{printf "  \033[36m%-12s\033[0m %s\n", $$1, $$2}' \
	     $(MAKEFILE_LIST)
	@echo ""

##@ Build

build: ## Build the firmware
	$(PIO) run -e $(ENV)

# Note: on Apple Silicon, building the LittleFS image needs Rosetta 2 —
# tool-mklittlefs from espressif32 ships as x86_64 only.
#     softwareupdate --install-rosetta --agree-to-license

fs: ## Build the LittleFS image from data/
	$(PIO) run -e $(ENV) -t buildfs

size: ## Show flash and RAM usage
	$(PIO) run -e $(ENV) -t size

test: ## Run native tests for the pure logic (on the host, no hardware)
	$(PIO) test -e native

lint: ## Rebuild src with -Wall -Wextra (the normal build omits them)
	@rm -f .pio/build/$(ENV)/src/*.o
	PLATFORMIO_BUILD_FLAGS="-Wall -Wextra" $(PIO) run -e $(ENV)

clean: ## Clean the build directory
	$(PIO) run -e $(ENV) -t clean

##@ Flashing over USB

flash: ## Flash the firmware over USB
	$(PIO) run -e $(ENV) -t upload $(UPLOAD_PORT)

flash-fs: ## Flash data/ (LittleFS) over USB
	$(PIO) run -e $(ENV) -t uploadfs $(UPLOAD_PORT)

flash-all: flash flash-fs ## Flash both firmware and data/ over USB

monitor: ## Serial monitor at 115200 with backtrace decoder and timestamps
	$(PIO) device monitor $(MON_PORT) --filter esp32_exception_decoder --filter time

dev: flash monitor ## Flash over USB and open the monitor right away

ports: ## List available serial ports
	$(PIO) device list

erase: ## Erase the whole flash (wipes NVS: pairing, WiFi, MQTT, OTA)
	@printf 'Erase flash and ALL settings in NVS? [y/N] '; \
	read ans; [ "$$ans" = "y" ] || { echo "Cancelled."; exit 1; }
	$(PIO) run -e $(ENV) -t erase $(UPLOAD_PORT)

##@ Flashing over the air (OTA must be enabled in the web GUI)

ota: require-ip ## Flash the firmware over OTA
	$(PIO) run -e $(ENV) -t upload --upload-port $(IP) $(OTA_FLAGS)

ota-fs: require-ip ## Flash data/ (LittleFS) over OTA
	$(PIO) run -e $(ENV) -t uploadfsota --upload-port $(IP) $(OTA_FLAGS)

ota-all: ota ## Flash firmware and data/ over OTA, pausing for the reboot
	@echo "Waiting for the device to reboot (15 s)..."
	@sleep 15
	@$(MAKE) ota-fs

##@ Diagnostics

status: require-ip ## Device status over HTTP (/api/info + /api/status)
	@curl -fsS --max-time 5 http://$(IP)/api/info   | python3 -m json.tool
	@curl -fsS --max-time 5 http://$(IP)/api/status | python3 -m json.tool

ping: require-ip ## Check whether the device responds
	@ping -c 3 $(IP)

##@ MQTT (needs mosquitto-clients: brew install mosquitto)

mqtt-watch: require-mqtt ## Subscribe to all device topics
	mosquitto_sub -h $(MQTT_HOST) -p $(MQTT_PORT) $(MQTT_AUTH) -t '$(MQTT_TOPIC)/#' -v

mqtt-cmd: require-mqtt ## Send a command: make mqtt-cmd C=power|up|down|mode
	mosquitto_pub -h $(MQTT_HOST) -p $(MQTT_PORT) $(MQTT_AUTH) -t '$(MQTT_TOPIC)/cmd/$(C)' -m 1

require-ip:
	@if [ -z "$(IP)" ]; then \
		echo "Device IP is not set."; \
		echo "  make <target> IP=192.168.1.50"; \
		echo "  or put IP in Makefile.local"; \
		exit 1; \
	fi

require-mqtt:
	@if [ -z "$(MQTT_HOST)" ]; then \
		echo "MQTT_HOST is not set."; \
		echo "  make <target> MQTT_HOST=mqtt.example.com"; \
		echo "  or put MQTT_HOST in Makefile.local"; \
		exit 1; \
	fi
	@command -v mosquitto_sub >/dev/null 2>&1 || { \
		echo "mosquitto_sub not found. Install it: brew install mosquitto"; \
		exit 1; \
	}
