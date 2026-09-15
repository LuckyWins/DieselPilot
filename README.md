# Diesel Pilot — fork

ESP32 controller for Chinese diesel heaters that talk over 433 MHz RF (CC1101).
Control through a local web GUI and, remotely, through a Telegram bot.

Forked from **[PPTG/DieselPilot](https://github.com/PPTG/DieselPilot)** at V1.3.
The upstream project is the origin of the protocol work and of everything in
`src/` that speaks to the heater; this fork adds remote control, unattended
reliability and a test suite. See [What this fork changes](#what-this-fork-changes).

> ⚠️ Use at your own risk. This drives a device that burns fuel unattended.

---

## What this fork changes

| Area | Upstream V1.3 | Here |
|---|---|---|
| Remote control | MQTT, or the web GUI exposed to the internet | Telegram bot over an outbound connection — works behind CGNAT, opens no ports |
| Display driver | SH1106 | SSD1306, which also covers SSD1315 |
| Unattended safety | — | Runtime limit and a shutdown deadline before scheduled power cuts |
| Hang protection | — | Task watchdog, bounded CC1101 SPI waits, module self-test |
| Network recovery | Connect once at boot | Supervised Wi-Fi with exponential backoff |
| Settings forms | A blank field erased the stored value | Blank keeps, `__CLEAR__` erases |
| Web API | Open to any cross-origin request | Mutating endpoints require a custom header and POST |
| Tests | None | 62 host-side cases, no hardware needed |

MQTT is still in the firmware and still works; it is simply inert until a
broker address is set. It remains the path to Home Assistant.

---

## Compatibility

- Two controllers with 🔧 as the upper-left button were tested upstream, one
  with a red remote and one with a black one. Both work.
- There is also a controller version with the ☀️/⚙️ symbol. Versions with the
  same display as the 🔧 version should work straight away.
- The ☀️/⚙️ version with the older menu-less display and H1–H6 power levels
  works as of V1.3.

| Version 1.0 | Version 1.5 | Version 2.0 |
|-----------|-------------|-------------|
| <img src="https://github.com/user-attachments/assets/b8a03480-bc09-4f54-aad0-e6de703ac34e" width="200" /> | <img src="https://github.com/user-attachments/assets/c592318d-d72a-4915-9d4b-620a8a11268a" width="200" />  | <img src="https://github.com/user-attachments/assets/5ea85b0c-e52e-4975-95cd-75102e8717f1" width="200" /> |

More detail, including manual pairing, lives on the
[upstream Wiki](https://github.com/PPTG/DieselPilot/wiki).

---

## Features

**Control**
- Web GUI with a dark theme, served from LittleFS — local network only
- Telegram bot with an inline keyboard: on, off, step up, step down, mode
- Explicit on/off rather than the protocol's bare power toggle
- Automatic and manual pairing (V1 and V2 protocols)

**Unattended operation**
- Runtime limit: stop the heater after N minutes
- Shutdown deadline: stop it early enough before a scheduled power cut for the
  purge cycle to finish
- Task watchdog, plus bounded SPI waits so an unplugged CC1101 cannot hang the
  controller during boot
- Wi-Fi supervision with exponential backoff
- CC1101 presence check via the VERSION register

**Monitoring**
- Telegram notifications: heater faults, state changes, flat battery, silent
  RF module, scheduled shutdowns, and a boot notice carrying the reset reason
- OLED display with status and IP
- Error code decoding (BYTE[7])

**Plumbing**
- MQTT with Home Assistant integration (inert until configured)
- OTA firmware updates over the local network (ArduinoOTA / espota)
- Settings in NVS, surviving reboots and reflashes

---

## Hardware

| Component | Model | Notes |
|-----------|-------|-------|
| Microcontroller | ESP32 | classic ESP-WROOM-32 |
| RF transceiver | CC1101 | 433 MHz |
| Display | SSD1306 / SSD1315 | OLED 128x64, I2C |

**CC1101 wiring**
```
ESP32    CC1101
-----    ------
GPIO4  - GDO2
GPIO18 - SCK
GPIO19 - MISO
GPIO23 - MOSI
GPIO5  - CSn
3.3V   - VCC
GND    - GND
```

**OLED wiring**
```
ESP32    OLED
-----    ------
GPIO21 - SDA
GPIO22 - SCL
3.3V   - VCC
GND    - GND
```

The display driver is chosen in `src/DieselPilot_V1_3-FS.ino`. SSD1315 panels
are software compatible with the SSD1306. If the screen stays blank, check the
I2C address: U8g2 defaults to 0x3C, some modules ship strapped to 0x3D.

---

## Project structure

```
platformio.ini              board, partitions, libraries, native test env
Makefile                    common commands — run `make help`
src/
  DieselPilot_V1_3-FS.ino   firmware: hardware, web server, Telegram, loop
  protocol.{h,cpp}          CRC-16, frequency maths, state and error decoders
  settings.{h,cpp}          settings form parsing
  scheduler.{h,cpp}         shutdown timers
  notify.{h,cpp}            chat whitelist, repeat suppression, backoff
data/index.html             web GUI (uploaded to LittleFS)
test/                       host-side unit tests
MQTT-Example/               Home Assistant MQTT configuration example
TODO.md                     deferred ideas, and why some were rejected
```

Modules under `src/` other than the `.ino` hold pure logic with no hardware
access, which is what lets them be tested on the host.

---

## Build and upload

Requires [PlatformIO](https://platformio.org/). Libraries are downloaded
automatically. The CC1101 is driven by SPI functions in this repository, not by
an external library.

```bash
make help        # every available command
make build       # compile the firmware
make test        # run the host-side tests, no hardware needed
make flash       # firmware over USB
make flash-fs    # web GUI (contents of data/) over USB
make monitor     # serial monitor with backtrace decoding
```

Plain PlatformIO works too: `pio run -t upload`, `pio run -t uploadfs`,
`pio device monitor`.

> After changing `data/index.html`, run `make flash-fs` again. Flashing the
> firmware does not touch the filesystem partition.

**Partitions.** The built-in `default.csv` table gives two app partitions
(`app0`/`app1`, needed for OTA) plus roughly 1.5 MB of LittleFS.

**On Apple Silicon**, building the LittleFS image needs Rosetta 2, because the
`mklittlefs` tool shipped with the espressif32 platform is x86_64 only:

```bash
softwareupdate --install-rosetta --agree-to-license
```

### OTA updates

1. In the web GUI open the **⬆ OTA** tab, enable OTA, optionally set a
   password. The device reboots.
2. Flash over the network:

```bash
make ota IP=192.168.1.50                    # add OTA_PASS=… if one is set
```

Put `IP`, `OTA_PASS` and the MQTT credentials in `Makefile.local` to avoid
retyping them. That file is gitignored.

### Flashing without PlatformIO

[DieselPilotTool](https://github.com/PPTG/DieselPilotTool) flashes prebuilt
`firmware.bin` and `littlefs.bin` over OTA or USB. It is a separate upstream
repository under GNU GPL v3; this firmware stays MIT.

| Image        | Offset     |
|--------------|------------|
| firmware.bin | `0x10000`  |
| littlefs.bin | `0x290000` |

> The very first flash of a blank chip — bootloader and partition table — has
> to be done with PlatformIO. The tool only writes partitions at `0x10000` and
> above.

---

## First run

1. The ESP32 starts in **AP mode**.
2. Join the Wi-Fi network `Diesel-Pilot`, password `12345678`.
3. Open `http://192.168.4.1`.
4. Pair the heater, automatically or manually.
5. Configure your own Wi-Fi, and optionally Telegram, timers and MQTT.

Saving settings takes a moment — the values are written to NVS. Wait for the
confirmation dialog.

In every settings form, **a blank field keeps the stored value**. To erase one,
type `__CLEAR__` into it. Without that rule, editing one field would wipe the
others: that is how the Wi-Fi credentials used to disappear, leaving the device
in AP mode where nobody could reach it.

### Automatic pairing

1. Press **AUTO PAIR** in the GUI.
2. The ESP32 listens for 60 seconds.
3. Press and hold the pairing button on the heater panel, usually 5–10 seconds.
   The heater enters discovery mode and sends a status frame with its address.
4. The address is caught and stored in NVS.

[Video of the pairing process](https://youtu.be/xmEbU_qbN60).

For manual pairing, see `ForNerds.md` on the upstream Wiki.

---

## Telegram bot

The bot is how the device is reached from outside the local network. The
connection is outbound, so it works behind carrier-grade NAT and needs no
public IP, no port forwarding and no broker.

1. Message [@BotFather](https://t.me/BotFather), send `/newbot`, follow the
   prompts and copy the token.
2. Web GUI → **✈️ Telegram** tab → paste the token, enable the bot, save. The
   device reboots.
3. Press **🔍 FIND MY CHAT ID**, then send `/id` to your bot within five
   minutes. It replies with your chat id.
4. Put that id into **Allowed chat ids** and save.
5. Press **✉️ SEND TEST** to confirm.

**The whitelist is the only thing protecting the heater.** Anyone can find a
bot by its name and message it, so only the listed chat ids are obeyed;
everything else is ignored in silence. An empty list allows nobody.

Commands: `/status`, `/on`, `/off`, `/id`, `/help`, plus the inline keyboard.

**Polling interval** defaults to 20 seconds and is configurable. Each poll
costs mobile data, which matters on a metered plan; a preheat scheduled hours
ahead does not need a faster reply.

The Telegram root certificate is bundled with the firmware. The tab accepts a
replacement in case Telegram ever changes its certificate authority — without
that escape hatch, a stale certificate could only be fixed on site.

---

## Shutdown timers

Two independent mechanisms stop the heater, whichever comes first. Both are
configured in the **⏱ Timers** tab.

**Runtime limit.** Stops the heater after N minutes of running. Protection
against switching it on and forgetting. Counted on the monotonic clock, so it
works even when the time was never synchronised.

**Shutdown before a scheduled power cut.** If mains power at your site is cut
on a schedule, set the time here. The controller stops the heater early enough
for the purge to finish first.

That second one is about hardware, not convenience. Cutting power to a running
diesel heater skips the purge cycle: unburnt fuel stays in the combustion
chamber and the heat exchanger cools with no airflow. Repeated nightly, that
cokes up the burner and makes ignition progressively harder.

The heater is then tracked through `SHUTDOWN` → `SHUTTING_DOWN` → `COOLING`
until it reports `OFF`, and a Telegram alert is raised if it never does.

The deadline needs a synchronised clock, since the ESP32 boots believing it is
1970. Without one it is skipped entirely and only the runtime limit applies —
the realistic case being a morning where mains power returns before the router
does. NTP server and UTC offset are set in the same tab.

---

## Tests

Pure logic lives in separate modules so it can be compiled and tested on the
host, with no board attached:

```bash
make test
```

62 cases covering the shutdown scheduler, the chat whitelist, settings form
parsing, CRC-16 and the CC1101 frequency maths. The scheduler is the reason the
suite exists — it switches off a heater, and states like "21:45 with an
unsynced clock" or a shutdown window wrapping past midnight are awkward to
stage on real hardware.

---

## Troubleshooting

**No communication with the heater**
- Verify the frequency (433.937 MHz for V2)
- Check the heater is paired
- Check the CC1101 supply voltage — it must be 3.3 V
- Watch the serial log for `CC1101 self-test failed`, which means the module is
  not answering at all

**OLED blank**
- Check the I2C address: 0x3C by default, some modules use 0x3D
- Verify the SDA/SCL wiring

**Telegram silent**
- Check your chat id is in the whitelist
- The status line in the Telegram tab shows whether the bot connected
- The clock has to be synchronised before TLS can validate a certificate; the
  System tab shows the device time

### CC1101 frequency tuning

Every CC1101 module drifts a little — typically ±10–30 kHz from the nominal
433.92 MHz. Five modules were tested upstream and all worked, but if reception
is weak, tune the frequency.

The register values used here are `FREQ2=0x10, FREQ1=0xB0, FREQ0=0x9C`, which
works out to 433.937 MHz. A custom frequency can be entered in hertz in the
Pairing tab without rebuilding the firmware; the serial log then reports the
value the module actually tuned to after rounding to its 397 Hz step.

Useful tools: [rtl_433](https://github.com/merbanan/rtl_433) for packet
decoding, SDR# or GQRX for spectrum visualisation, Inspectrum for IQ analysis,
Universal Radio Hacker for protocol reverse engineering.

---

## Roadmap

Deferred ideas, planned work and the reasoning behind rejected alternatives are
in [TODO.md](TODO.md).

Upstream's own roadmap — fuel level sensor, heater simulator, further
controller versions — lives in the
[upstream repository](https://github.com/PPTG/DieselPilot).

---

## License

**MIT** — see [LICENSE](LICENSE). Third-party libraries keep their own terms:
U8g2 (BSD-2), PubSubClient (MIT), ArduinoJson (MIT), AsyncTelegram2 (MIT),
ESP32 core and ArduinoOTA (LGPL 2.1).

---

## Acknowledgments

- **[PPTG/DieselPilot](https://github.com/PPTG/DieselPilot)** — the upstream
  project this is forked from. The protocol work, the CC1101 driver and the web
  GUI all started there.
- **[merbanan/rtl_433](https://github.com/merbanan/rtl_433)** — the tool for RF
  protocol reverse engineering. Without it the protocol analysis would not have
  been possible.
- **[DieselHeaterRF](https://github.com/jakkik/DieselHeaterRF)** — inspiration
  for parts of the protocol and the CC1101 handling.
- **RTL-SDR community** — for making SDR cheap and accessible.
- **Home Assistant community** — for the motivation behind the MQTT
  integration.
