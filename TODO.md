# TODO — ideas for later

Everything deliberately postponed. The current work plan is not duplicated
here; this is only what to come back to.

---

## Verify on real hardware

- [ ] **OTA through Telegram.** `AsyncTelegram2` claims firmware updates by
      sending the file to the bot. If it works, remote updates need no public
      IP at all, and the MTS "static name" service is unnecessary.
- [ ] **Whether TCP is reused between `getUpdates` calls.** Traffic differs by
      a factor of twelve: roughly 37 MB/month with a reused connection against
      430 MB/month if every poll does a TLS handshake.
- [ ] **How long a TLS handshake actually takes** on a poor link — needed to
      pick the watchdog timeout.
- [ ] **Which protocol version this heater actually speaks**, and how its
      power ladder behaves. `/level` walks V1 straight to the level, because
      the V1 frame carries it. The V2 frame has no level number at all, only
      the pump rate, so the walk descends to the bottom of the ladder and
      climbs from there. Two things need watching: whether the ladder stops at
      its ends or wraps round (the stepper detects a wrap and gives up rather
      than circling, but it has never seen one), and whether `STEP_SETTLE_MS`
      — two poll intervals — is long enough for a step to show up in the
      reading.

- [ ] **The display's I2C address.** U8g2 defaults to 0x3C. If the module is
      strapped to 0x3D it needs `display.setI2CAddress(0x3D * 2)`.
- [ ] **Whether MTS hands out a public IP.** `curl -4 ifconfig.me` against the
      modem's WAN address; anything in 100.64.0.0/10 is CGNAT. Only relevant
      if direct access from the internet ever comes back on the table.

---

## Monitoring

- [ ] **Persist the heap and loop figures too.** The counters, the error log
      and the ignition log now survive the night; the diagnostics do not. The
      minimum free heap since boot and the longest loop iteration still reset
      at 22:00, which is precisely what makes a slow leak invisible — every
      night hands back a clean slate. Decide first whether a per-night figure
      is worth storing at all, or whether a running worst-case since the last
      flash is the more useful number.

- [ ] **Read the diagnostics off a running device and decide what to do with
      them.** `/api/info` and the bot's status now carry free heap, minimum
      free heap since boot and the longest loop iteration. The minimum heap is
      the one that matters: a slow leak or fragmentation shows up there long
      before anything visibly breaks.

      After a week of uptime, look at the figures and decide whether they
      deserve a threshold with a notification, a line on the display, or
      whether it is time to take on ArduinoJson.

- [ ] **External dead-man switch.** The firmware reports problems it can see,
      but it cannot report its own death. The rule "no boot message after
      06:30" needs somebody outside to notice the absence.

      Services: Healthchecks.io, Cronitor, BetterStack, or UptimeRobot's
      heartbeat mode. The device pings a URL periodically and the service
      raises the alarm when a ping fails to arrive.

      Why it fits: the ping is outbound, an ordinary HTTPS GET, so it works
      behind CGNAT with nothing exposed. A cron schedule marks 22:00–06:00 as
      expected downtime, so the nightly power cut raises no false alarms.
      Notifications can go to Telegram, keeping everything in one place.

      Around twenty lines on top of the existing `WiFiClientSecure`: a URL in
      the settings, a ping every fifteen minutes, silent failure. Free tiers
      are ample — check the limits before relying on one.

---

## Remote access

- [ ] **Make OTA rollback actually work.** The protection exists in the
      bootloader (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=1`) but is completely
      defeated: the core calls `verifyOta()`, which returns `true` by default,
      and confirms any uploaded image inside `initArduino()` — before the
      sketch's own `setup()` ever runs.

      The fix is to override the weak symbol `verifyRollbackLater()` to return
      `true`. That defers the verdict, and the image is then confirmed by our
      own code — only once the network is up and the bot has reached Telegram,
      say. No confirmation within N minutes means a reboot, and the bootloader
      restores the previous firmware.

      It composes well with the watchdog: a new firmware that hangs gets
      rebooted by the timer and, never having confirmed itself, rolls back.
      Turns "flashed something broken over the air" from a hundred-kilometre
      drive into self-recovery.

- [ ] **MQTT + Home Assistant.** The MQTT code is already there and completely
      inert until a broker address is set. Entering one is all it takes. Worth
      it if graphs, history and automations are ever wanted.
- [ ] **HA MQTT discovery** so entities appear in Home Assistant on their own,
      without hand-written YAML.
- [ ] **Direct access to the web GUI from the internet.** Requires a public IP,
      HTTPS on the ESP32 (`esp_https_server` ships with the framework), a
      private CA installed on every client, and hardening against scanners.
      Rejected in favour of Telegram — see the table at the bottom.

---

## Security

- [ ] **Proper authentication in the admin page**: session tokens, PBKDF2
      instead of a stored password, brute-force protection, `HttpOnly` plus
      `SameSite=Strict`. For now this stops at the `X-DieselPilot` anti-CSRF
      header and moving mutating requests to POST, which is enough while the
      admin page is reachable only from the local network.
- [ ] **NVS encryption.** The bot token and the passwords sit there in plain
      text. Only relevant against physical access to the board.

---

## Code quality

- [ ] **Check whether the ten-second idle heater poll feels too lazy.** It used
      to be three seconds around the clock. The slow rate applies while the
      heater reports OFF and returns to three seconds for a minute after any
      command. If state updates turn out to lag in practice, raise
      `HEATER_POLL_IDLE_MS` back or drop it to five seconds. The constant sits
      next to `HEATER_POLL_FAST_MS`.

- [ ] **Verify on hardware: `sendCommand()` waits for `TXBYTES == 0x01`** with
      strict equality. If the counter skips that value the loop spins until the
      100 ms timeout. Watch how the register actually behaves during a
      transmission.
- [ ] **Verify on hardware: the retry logic in `sendCommand()`.** On success
      the frame goes out ten times in a row; on a single timeout the function
      abandons every remaining attempt. It is unclear whether that redundancy
      is deliberate.
- [ ] **Verify on hardware: `ambientTemp = (b8 & 0x0F) + 10` in V1** yields a
      range of 10–25 °C, with no way to express a negative temperature. Odd for
      a winter garage — the protocol may carry a sign the decoder drops.
- [ ] **Blocking delays in the V1 auto-menu** — roughly 350 ms every 3.5 s.
      This is not carelessness: after `strobe(0x35)` the frame needs time to
      leave the air. Reworking it into a state machine changes protocol
      timing, so only with hardware to watch.

- [ ] **ArduinoJson instead of assembling responses by hand.**

      There is one real reason: **stop relying on nobody forgetting
      `jsonEscape()`.** The escaping is correct today and covered by tests, but
      it takes discipline — add a field with a string value, forget the
      wrapper, and the response breaks silently again. It would also rule out
      structural mistakes: a trailing comma, an unclosed quote, a missing
      `+ ","` when inserting a field in the middle.

      The cost is measured: the library is **already in the firmware**
      (15.3 KB, pulled in by AsyncTelegram2), and converting one handler added
      1768 bytes — a one-off price for instantiating the serialiser. The whole
      migration should come to 2–4 KB.

      What it does not give: little help with heap fragmentation.
      `JsonDocument` allocates dynamically too, and after adding `reserve()`
      the difference is small. Streaming straight into the socket
      (`serializeJson(doc, server.client())`) would genuinely help, but that
      means wrestling with `setContentLength()` and unverified `WebServer`
      compatibility.

- [ ] **Carry on splitting the `.ino` into modules.** `protocol` and `settings`
      are out, and so are `stepper` and `stats`, but the `.ino` has grown to
      around 3200 lines rather than shrunk. Candidates: `cc1101`, `web`,
      `display`, and the Telegram command handling.
      Only code living in its own `.cpp` builds on the host, so everything
      still inside is untestable.
- [ ] **Extend the native tests** to the V1/V2 packet decoders, which first
      needs the parsing extracted into pure functions with no globals
      (`decodePacket_V1` and the V2 reply parser write straight into
      `heaterStatus`). The infrastructure is ready: `pio test -e native`.
- [ ] **Drop the `delay(2000)`** in display initialisation — a pure splash
      screen that delays startup.
- [ ] **Non-blocking `connectMQTT()`** — currently up to three attempts with
      `delay(2000)` between them, blocking the loop for 4+ seconds.

---

## Interface odds and ends

- [ ] **Start mDNS independently of OTA and advertise the web service.**
      `ArduinoOTA.begin()` calls `MDNS.begin(hostname)` internally, so
      `<deviceName>.local` already resolves — but only while the OTA toggle is
      on, and `MDNS.enableArduino()` advertises only the development-tool
      service, not HTTP.

      A three-line change: call `MDNS.begin()` unconditionally and add
      `MDNS.addService("http", "tcp", 80)`. The point is that the modem hands
      out the IP over DHCP and may change it, and there is nowhere in the
      garage to look up what it handed out. The name never changes.

      Caveat: `.local` works out of the box on macOS, iOS and Windows 10+, but
      Android support is uneven. Fine as a replacement for the IP, not as the
      only way in.

- [ ] **Frost protection.** "If the garage drops below X degrees, run for N
      minutes." The data (`ambientTemp`) and the scheduler already exist; only
      a threshold branch is missing. Postponed: this garage has not seen such
      temperatures yet.

- [ ] **Reshoot the web GUI screenshot for the README.** The current one
      predates the Telegram and Timers tabs, and the README carries a note
      saying so — remove it once the image is replaced. Needs a running device.

---

## Rejected (so the analysis is not repeated)

| Idea | Why it was dropped |
|---|---|
| HTTPS on the ESP32 web GUI | ~450 lines of rewriting the web layer plus a private CA on every client. Unnecessary while the admin page lives on the local network |
| Challenge-response at login | Solves password interception on the wire. Irrelevant for local-only access |
| Public IP + DDNS from MTS | Exposes a web server with no authentication, and `375xxxxxxxxx.dyndns.mts.by` is trivially enumerable. Telegram solves the same problem with no open ports |
| Managed MQTT instead of a VPS | Only relevant if MQTT comes back. HiveMQ Cloud, Frankfurt region, free tier — check the limits before relying on it |
| Removing the MQTT code | Completely inert with no broker address, and measured at 2.5 KB of flash. Nothing worth removing |
| Powering the ESP32 from 12 V | Not needed: the heater runs off the same 220 V mains as the controller. When the power goes, both go — there is no unattended overnight burning |
| A log file in LittleFS | The bot chat already is the log: stored by Telegram, searchable, and it survives a reflash |
| Integer formatting to drop float printf | Measured: frees 624 bytes, not the 15 KB the symbol map suggested. The ESP32 core links the full newlib printf regardless |
| Preheat timed to arrival rather than to a start time | "Be warm by 18:00" needs a model of how fast the garage heats, and that depends on the outside temperature, which the controller cannot see. The measured rate from one burn does not carry to the next |
| Thermostat: stop once the garage reaches N degrees | The heater is not powerful enough to overshoot this garage, so the cut-off would never fire. The runtime limit already covers the case it was meant for |
| Warning on a rising case temperature | Meant to catch a blocked duct before the heater faults on it. The heater reports `OVERHEAT` itself, and at this output a real runaway is unlikely enough not to be worth the false alarms |
| Counting mobile data on the device | The operator already meters it, and their figure is the one that decides whether the plan runs out |
| Settings backup and restore through the GUI | Would save retyping the token and Wi-Fi after a factory reset or a board swap. Rare enough to do by hand, and a file holding the bot token is a new thing to look after |
