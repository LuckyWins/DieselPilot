# ═══════════════════════════════════════════════════════════════════════════
#                  DIESEL PILOT — обёртка над PlatformIO
# ═══════════════════════════════════════════════════════════════════════════
# Список команд:  make  (или make help)
#
# Локальные значения — IP устройства, пароли — держим в Makefile.local.
# Он в .gitignore и в репозиторий не попадёт. Пример содержимого:
#
#     IP        = 192.168.1.50
#     OTA_PASS  = secret
#     MQTT_HOST = mqtt.example.com
#     MQTT_USER = diesel
#     MQTT_PASS = secret
#
# Любую переменную можно переопределить из командной строки:
#     make ota IP=192.168.1.77
# ═══════════════════════════════════════════════════════════════════════════

-include Makefile.local

PIO ?= pio
ENV ?= esp32dev

# Адрес устройства в сети — для OTA и HTTP-запросов
IP ?=
# Пароль OTA (пусто = прошивка без пароля)
OTA_PASS ?=
# Последовательный порт (пусто = автоопределение PlatformIO)
PORT ?=

MQTT_HOST ?=
MQTT_PORT ?= 1883
MQTT_TOPIC ?= diesel
MQTT_USER ?=
MQTT_PASS ?=
# Команда для mqtt-cmd: power | up | down | mode
C ?= power

UPLOAD_PORT = $(if $(PORT),--upload-port $(PORT))
MON_PORT    = $(if $(PORT),-p $(PORT))
OTA_FLAGS   = $(if $(OTA_PASS),--upload-flags --auth=$(OTA_PASS))
MQTT_AUTH   = $(if $(MQTT_USER),-u $(MQTT_USER) -P $(MQTT_PASS))

.DEFAULT_GOAL := help
.PHONY: help build fs size test lint clean flash flash-fs flash-all monitor dev \
        ports erase ota ota-fs ota-all status ping mqtt-watch mqtt-cmd \
        require-ip require-mqtt

##@ Справка

help: ## Показать этот список
	@echo "DieselPilot — доступные команды:"
	@awk 'BEGIN{FS=":.*## "} \
	     /^##@/{printf "\n\033[1m%s\033[0m\n", substr($$0,5)} \
	     /^[a-z][a-zA-Z0-9_-]*:.*## /{printf "  \033[36m%-12s\033[0m %s\n", $$1, $$2}' \
	     $(MAKEFILE_LIST)
	@echo ""

##@ Сборка

build: ## Собрать прошивку
	$(PIO) run -e $(ENV)

# Внимание: на Apple Silicon сборка образа LittleFS требует Rosetta 2 —
# tool-mklittlefs из espressif32 собран только под x86_64.
#     softwareupdate --install-rosetta --agree-to-license

fs: ## Собрать образ LittleFS из data/
	$(PIO) run -e $(ENV) -t buildfs

size: ## Показать занятые flash и RAM
	$(PIO) run -e $(ENV) -t size

test: ## Прогнать нативные тесты чистой логики (на хосте, без железа)
	$(PIO) test -e native

lint: ## Пересобрать src с -Wall -Wextra (штатная сборка их не включает)
	@rm -f .pio/build/$(ENV)/src/*.o
	PLATFORMIO_BUILD_FLAGS="-Wall -Wextra" $(PIO) run -e $(ENV)

clean: ## Очистить каталог сборки
	$(PIO) run -e $(ENV) -t clean

##@ Прошивка по USB

flash: ## Залить прошивку по USB
	$(PIO) run -e $(ENV) -t upload $(UPLOAD_PORT)

flash-fs: ## Залить data/ (LittleFS) по USB
	$(PIO) run -e $(ENV) -t uploadfs $(UPLOAD_PORT)

flash-all: flash flash-fs ## Залить и прошивку, и data/ по USB

monitor: ## Монитор порта 115200 + декодер стектрейсов и метки времени
	$(PIO) device monitor $(MON_PORT) --filter esp32_exception_decoder --filter time

dev: flash monitor ## Залить прошивку по USB и сразу открыть монитор

ports: ## Показать доступные последовательные порты
	$(PIO) device list

erase: ## Стереть флеш целиком (сотрёт NVS: паринг, WiFi, MQTT, OTA)
	@printf 'Стереть флеш и ВСЕ настройки в NVS? [y/N] '; \
	read ans; [ "$$ans" = "y" ] || { echo "Отменено."; exit 1; }
	$(PIO) run -e $(ENV) -t erase $(UPLOAD_PORT)

##@ Прошивка по воздуху (OTA должен быть включён в веб-GUI)

ota: require-ip ## Залить прошивку по OTA
	$(PIO) run -e $(ENV) -t upload --upload-port $(IP) $(OTA_FLAGS)

ota-fs: require-ip ## Залить data/ (LittleFS) по OTA
	$(PIO) run -e $(ENV) -t uploadfsota --upload-port $(IP) $(OTA_FLAGS)

ota-all: ota ## Залить прошивку и data/ по OTA, с паузой на перезагрузку
	@echo "Ждём перезагрузку устройства (15 с)..."
	@sleep 15
	@$(MAKE) ota-fs

##@ Диагностика

status: require-ip ## Статус устройства по HTTP (/api/info + /api/status)
	@curl -fsS --max-time 5 http://$(IP)/api/info   | python3 -m json.tool
	@curl -fsS --max-time 5 http://$(IP)/api/status | python3 -m json.tool

ping: require-ip ## Проверить, отвечает ли устройство
	@ping -c 3 $(IP)

##@ MQTT (нужен mosquitto-clients: brew install mosquitto)

mqtt-watch: require-mqtt ## Слушать все топики устройства
	mosquitto_sub -h $(MQTT_HOST) -p $(MQTT_PORT) $(MQTT_AUTH) -t '$(MQTT_TOPIC)/#' -v

mqtt-cmd: require-mqtt ## Послать команду: make mqtt-cmd C=power|up|down|mode
	mosquitto_pub -h $(MQTT_HOST) -p $(MQTT_PORT) $(MQTT_AUTH) -t '$(MQTT_TOPIC)/cmd/$(C)' -m 1

require-ip:
	@if [ -z "$(IP)" ]; then \
		echo "Не задан IP устройства."; \
		echo "  make <цель> IP=192.168.1.50"; \
		echo "  или пропишите IP в Makefile.local"; \
		exit 1; \
	fi

require-mqtt:
	@if [ -z "$(MQTT_HOST)" ]; then \
		echo "Не задан MQTT_HOST."; \
		echo "  make <цель> MQTT_HOST=mqtt.example.com"; \
		echo "  или пропишите MQTT_HOST в Makefile.local"; \
		exit 1; \
	fi
	@command -v mosquitto_sub >/dev/null 2>&1 || { \
		echo "mosquitto_sub не найден. Установите: brew install mosquitto"; \
		exit 1; \
	}
