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
- Written in English: everything in the repository — code comments, both
  CLAUDE.md files, tests, `platformio.ini`, `Makefile`, commit messages
- Written in Russian: `TODO.md` and conversation with the user

## Commits

Conventional Commits, in English — the same convention the vib3 projects follow.

```
type(scope): short imperative summary
```

- Types: `feat`, `fix`, `refactor`, `docs`, `chore`, `style`, `test`, `revert`
- Scope: the area touched — `cc1101`, `display`, `web`, `settings`, `scheduler`,
  `telegram`, `wifi`, `mqtt`, `build`, `test`
- Subject: lowercase, imperative, no trailing period, under ~72 characters
- Body: only when the reason is not obvious from the diff. Explain why the
  change was needed, not what the diff already shows. Wrap at 72 columns.
