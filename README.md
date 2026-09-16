# Diesel Pilot — fork

ESP32 controller for Chinese diesel heaters that talk over 433 MHz RF (CC1101).
Control through a local web GUI and, remotely, through a Telegram bot.

Forked from **[PPTG/DieselPilot](https://github.com/PPTG/DieselPilot)** at V1.3.
The upstream project is the origin of the protocol work and of everything in
`src/` that speaks to the heater; this fork adds remote control, unattended
reliability and a test suite. See [What this fork changes](#what-this-fork-changes).

> ⚠️ Use at your own risk. This drives a device that burns fuel unattended.

<img width="874" height="730" alt="Web GUI" src="https://github.com/user-attachments/assets/a3715ef1-9ef1-4257-a28f-77bb7ff2645d" />

*Screenshot predates the Telegram and Timers tabs.*

---

## What this fork changes

| Area | Upstream V1.3 | Here |
|---|---|---|
| Remote control | MQTT, or the web GUI exposed to the internet | Telegram bot over an outbound connection — works behind CGNAT, opens no ports |
| Display driver | SH1106 | SSD1306, which also covers SSD1315 |
| Unattended safety | — | Runtime limit, a shutdown deadline before scheduled power cuts, and a refusal to light what cannot finish before one |
| Maintenance | — | Hours, fuel, starts and ignition quality, tracked against the last service |
| Fuel | — | Consumption integrated from the pump, and what is left in the tank |
| State across reboots | — | Counters, error log and ignition log in NVS, so the nightly power cut costs nothing |
| Hang protection | — | Task watchdog, bounded CC1101 SPI waits, module self-test |
| Network recovery | Connect once at boot | Supervised Wi-Fi: exponential backoff on a dropped link, and the fallback access point keeps hunting for the real network instead of sitting there until somebody reboots it |
| Settings forms | A blank field erased the stored value | Blank keeps, `__CLEAR__` erases |
| Web API | Open to any cross-origin request | Mutating endpoints require a custom header and POST |
| Tests | None | 170 host-side cases, no hardware needed |

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
- Explicit on/off rather than the protocol's bare power toggle, on both paths
- Set a power level or a target temperature in one command, instead of
  pressing ±1 and waiting out a poll after each
- Scheduled start, by clock time or after a delay
- Automatic and manual pairing (V1 and V2 protocols)

**Unattended operation**
- Runtime limit: stop the heater after N minutes
- Shutdown deadline: stop it early enough before a scheduled power cut for the
  purge cycle to finish
- A start that would be stopped again within minutes is refused outright,
  whether it comes from the chat, the web GUI or the schedule
- Ignition is verified rather than assumed: a command that did not light the
  heater is retried, then reported
- Task watchdog, plus bounded SPI waits so an unplugged CC1101 cannot hang the
  controller during boot
- Wi-Fi supervision: a dropped link is retried with exponential backoff, and
  a network that was missing at boot is retried once a minute from behind the
  fallback access point — after a power cut the controller is awake long
  before an LTE modem has registered, and losing that race used to mean
  sitting in access-point mode, unreachable, until the next reboot
- CC1101 presence check via the VERSION register

**Monitoring**
- Telegram notifications: heater faults, state changes, a sagging supply,
  silent RF module, scheduled shutdowns, failed ignitions, and a boot notice
  carrying the reset reason
- Progress reports while it burns, which turn into a warning when the garage
  is not actually warming up
- A notice in the morning when the power went while the heater was still lit
- OLED display with status and IP
- Error code decoding (BYTE[7]), logged to NVS with absolute timestamps

**Fuel and wear**
- Consumption integrated from the pump rate, per burn and over the device's
  life
- Optionally, what is left in the tank, by dead reckoning — no sensor — with
  warnings at a quarter and a tenth, and a word before a burn the tank will
  not cover
- Hours, fuel and starts since the last service, with an optional reminder
- Ignition quality tracked over time: how long a start takes and how far the
  glow plug drags the supply down, compared against the same burner when it
  was last cleaned

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
  scheduler.{h,cpp}         shutdown timers, ignition and start decisions
  stepper.{h,cpp}           walking the power level to a requested value
  stats.{h,cpp}             persistent counters, ring buffers, ignition trend
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

**Partitions.** The built-in `min_spiffs.csv` table gives two 1.875 MB app
partitions (`app0`/`app1`, needed for OTA) plus 128 KB of LittleFS. The stock
`default.csv` splits the same flash the other way round — 1.25 MB app slots
and 1.375 MB of filesystem — which suits a project whose data outweighs its
code. This one is the opposite: the firmware fills its slot while `data/`
holds a single 54 KB page.

A partition table is written over USB together with the bootloader and cannot
be changed over the air, so it is chosen before the board is installed.

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
| littlefs.bin | `0x3D0000` |

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

Commands, plus the inline keyboard:

| Command | What it does |
|---|---|
| `/status` | Readings, and the buttons |
| `/on`, `/off` | Start, or stop with the purge that follows |
| `/at 06:30` | Start at that time |
| `/in 2h` | Start after that delay |
| `/cancel` | Drop a pending scheduled start |
| `/for 90` | Runtime limit for this burn only |
| `/level 4` | Power level, in MANUAL |
| `/temp 22` | Target temperature, in AUTO |
| `/tank` | What is left, and roughly how long it lasts |
| `/filled`, `/filled 10` | Tank filled up, or ten litres added |
| `/stats` | Hours, fuel and starts, all time and since the last service |
| `/service done` | Record a service and start those counters again |
| `/ign` | The last few starts, and whether they are getting worse |
| `/id`, `/help` | Chat id, and this list |

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

A start inside that window is refused rather than allowed and then undone.
Lighting a heater only to stop it minutes later is worse than not lighting it:
the burner never reaches a steady flame, and that is what cokes it up.

The deadline needs a synchronised clock, since the ESP32 boots believing it is
1970. Without one it is skipped entirely and only the runtime limit applies —
the realistic case being a morning where mains power returns before the router
does. NTP server and UTC offset are set in the same tab.

---

## Fuel, and what wears out

Both are worked out from numbers the controller already had and used to throw
away. Neither needs any extra hardware.

**Fuel.** Each pump stroke doses a fixed volume, so consumption follows from
the pump rate the heater reports. Switch the tank estimate on in the
**⏱ Timers** tab and give it a capacity, and the controller keeps a running
figure for what is left, warns at a quarter and at a tenth, and says so when a
burn about to start needs more than the tank holds.

The switch is separate from the capacity, so turning the estimate off keeps
the capacity for later. Nothing is debited while it is off, and switching it
back on rearms the warnings from wherever the figure now sits rather than
firing for a crossing that happened while nobody was counting — the stored
figure will be stale by whatever was burnt in between, which is what `/filled`
or `/tank` is for.

This is dead reckoning, not a gauge. The dose drifts with pump wear,
temperature and supply voltage, so the figure is always shown with a tilde.
`/filled` after a real fill wipes the accumulated error; `/tank 7` corrects it
by hand.

**Wear.** Hours, fuel and starts are counted for the life of the device and
again since the last service, which `/service done` records. What matters for
the glow plug is the number of starts rather than the hours, and decoking is
due on accumulated running — neither was counted anywhere before.

**Ignition quality.** Every start is logged: how long it took to reach
`RUNNING`, how far the glow plug dragged the supply down while it tried, the
ambient temperature, and how many attempts it needed. The median of the last
five is compared against the same burner just after it was serviced, and a
drift past either threshold is reported once. A glow plug on its way out
announces itself this way months before it finally refuses to light in a
frost.

All of it lives in NVS, written at the end of each burn, every ten minutes
while one runs, and before a scheduled shutdown — a handful of writes a day,
which is what keeps the nightly power cut from costing anything.

---

## Tests

Pure logic lives in separate modules so it can be compiled and tested on the
host, with no board attached:

```bash
make test
```

170 cases covering the shutdown scheduler, the level stepper, the persistent
counters and ring buffers, the ignition trend, fuel and tank arithmetic, the
chat whitelist, settings form parsing, CRC-16 and the CC1101 frequency maths. The scheduler is the reason the
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
