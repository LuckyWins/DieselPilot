# Project context

Fork of PPTG/DieselPilot — an ESP32 controller for a Chinese diesel heater
over 433 MHz (CC1101).

Hardware and site-specific details live in CLAUDE.local.md (not committed).

Control: web GUI on the local network only, remote control through a Telegram
bot. The MQTT code is kept in the project but switched off and inert — it is
the path to Home Assistant should that ever be wanted.

## Rules
- Do not change the CC1101 protocol or the pairing logic without an explicit request
- Build: `pio run` / `pio run -t uploadfs` (or `make build` / `make fs`)
- Tests: `make test` — pure logic, runs on the host with no hardware attached
- Deferred tasks and the rationale behind rejected options live in `TODO.md`
- Written in English: code comments, both CLAUDE.md files, tests,
  `platformio.ini`, `Makefile`
- Written in Russian: commit messages, `TODO.md`, conversation with the user
