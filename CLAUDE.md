# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

A single ESPHome **external component** (`emerald_ble`) for the Emerald Electricity Advisor, a BLE energy monitor. The repo contains no application, no build system, and no tests — only `components/emerald_ble/`, which ESPHome pulls in via `external_components:` and compiles as part of a user's firmware.

Consequence: nothing here compiles standalone. Everything under `components/emerald_ble/` — the Python codegen and the C++ — is validated only by ESPHome's own build. `ble_client` / `esp32_ble_tracker` / `time` are deliberately *not* vendored; they resolve to whatever ESPHome version the user has installed. That is the whole point of the packaging (see README), so do not add copies of ESPHome core components to this repo.

## Building / verifying a change

There is no local test suite. To verify a change, compile it inside a real ESPHome config:

1. Write a minimal YAML (see the README example) that points `external_components:` at a local path rather than git:
   ```yaml
   external_components:
     - source:
         type: local
         path: /path/to/this/checkout/components   # absolute — it is not resolved relative to the YAML
       components: [emerald_ble]
   ```
2. `esphome config test.yaml` — exercises the Python side only (`__init__.py`, `sensor.py`, `binary_sensor.py`) and is the fast feedback loop for schema/codegen edits.
3. `esphome compile test.yaml` — the only thing that type-checks the C++. Requires an ESP32 target and the ESP-IDF toolchain.
4. `esphome run test.yaml` then `esphome logs test.yaml` for on-device behaviour; the component is log-heavy at `DEBUG` level (`logger: level: DEBUG`).

Keep scratch YAML out of the repo — `.gitignore` covers `.esphome/` and PlatformIO dirs but not stray configs.

## Architecture

**Three-file split, ESPHome-conventional:**

- `__init__.py` — declares the `Emerald` C++ class to codegen and owns `CONF_EMERALD_BLE_ID`. Deliberately thin: it exists so `sensor.py` and `binary_sensor.py` can both reference the same class without a circular import.
- `sensor.py` — the *primary* platform. It instantiates the `Emerald` component (`cv.declare_id`) and wires power / energy / daily_energy / battery sensors plus `pairing_code`, `pulses_per_kwh`, and optional `time_id`.
- `binary_sensor.py` — a *secondary* platform. It does **not** create a component; it looks up the existing `Emerald` via `cv.use_id` and attaches the `connected` binary sensor. So a `binary_sensor: platform: emerald_ble` block is only valid alongside a `sensor:` block.

**C++ (`emerald_ble.cpp/.h`) — `Emerald` is both a `BLEClientNode` and a `Component`.** It has no `loop()`; everything is driven by two callbacks:

- `gap_event_handler` handles pairing. `ESP_GAP_BLE_PASSKEY_REQ_EVT` replies with the configured 6-digit `pairing_code_`. `ESP_GAP_BLE_AUTH_CMPL_EVT` sets `auth_completed_`, and on failure calls `force_reconnect_()` — without that, the meter parks in a half-connected state until reboot.
- `gattc_event_handler` handles discovery and data. `ESP_GATTC_SEARCH_CMPL_EVT` resolves three characteristic handles and sets `handles_discovered_`.

**The two-flag rendezvous is load-bearing.** Auth completion and handle discovery arrive in a nondeterministic order, so `setup_communication_()` is called from whichever of the two lands second (`auth_completed_` && `handles_discovered_`). Changing either handler means preserving that. `reset_connection_state_()` clears both on disconnect.

**Data path:** the device is told to auto-upload every 30 s (`SET_AUTO_UPLOAD_STATUS_CMD`). Notifications on the time-read characteristic land in `decode_emerald_packet_()`, which switches on a 5-byte big-endian command header packed into a `uint64_t`. Only `RETURN30S_POWER_CONSUMPTION_CMD` is implemented; the other five cases are recognised-but-ignored placeholders. Power is `pulses × pulse_multiplier_`, where the multiplier folds in the 30 s window and `pulses_per_kwh`.

**Persistence:** `total_pulses_` / `daily_pulses_` / `day_of_last_measurement_` are stored in NVS as a packed `EmeraldPersistedState`. The preference hash is `fnv1_hash("emerald_ble_state_" + MAC)` — intentionally *not* keyed on compilation time, so totals survive recompiles and multiple meters don't collide. `setup()` republishes restored totals immediately because the first BLE packet can be a minute away, and Home Assistant's `total_increasing` statistics would otherwise see a reset. Writes are throttled to 60 s for NVS wear, with `save_state_(true)` forcing a write on day rollover and manual reset.

**Daily rollover** prefers the ESPHome `time` component when one is configured *and* currently has a valid time, and otherwise falls back to the day bits in the packet's own timestamp. Both paths must yield **day-of-month (1-31)** into `day_of_last_measurement_`, and the two earlier bugs explain why: the field is a `uint8_t`, so `ESPTime::day_of_year` (a `uint16_t`, 1-366) truncated — day 256 became `0`, colliding with the "no measurement yet" sentinel and silently skipping that rollover; and because the fallback path already stored day-of-month, a `time` component going valid mid-run (BLE packets normally beat SNTP after a reboot) looked like a day change and zeroed `daily_pulses_` mid-day. Residual limitation of day-of-month: a meter offline for exactly one calendar month misses one reset. `sensor.py` warns at config time when `daily_energy` is used without `time_id`.

## Upstream API drift

The main maintenance burden is ESPHome core changing under this component. When a compile breaks, check the headers in the *user's installed* ESPHome version before assuming the bug is local — `gh api "repos/esphome/esphome/contents/<path>?ref=<version>"` reads them without a checkout. Two worked examples:

- **2026.6 (`70143e5`)** made `BLEClientBase::gattc_if`/`conn_id`/`remote_bda` protected and changed `address_str()` to return `const char *`. Fix: use the getters (`get_gattc_if()`, `get_conn_id()`, `get_remote_bda()`) and pass `address_str()` straight to `%s`.
- **2026.8 (`88c4cd0`)** moved `ESPBTUUID` into a new platform-neutral `ble_device_base` layer (`esp32_ble/ble_uuid.h` is now just a re-export) and replaced `std::string to_string()` with `const char *to_str(char *buf)`, which writes into a caller-supplied `UUID_STR_LEN` buffer. Fix: *don't* chase the new signature — `UUID_STR_LEN` doesn't exist on older ESPHome, so adopting it would fork the component by version. The UUIDs are compile-time constants, so the string forms now live beside them in `emerald_ble.h` (`EMERALD_*_UUID_STR`) and feed both `from_raw()` and the log lines.

Generalising from those two: prefer a fix that depends on *less* of the core API rather than one that tracks the newest signature. This component is pulled by users on whatever version they happen to run, and there is no version guard in the build.

Raw IDF calls (`esp_ble_gattc_write_char`, `esp_ble_gattc_read_char`, `esp_ble_gattc_register_for_notify`, `esp_ble_gattc_close`) are used directly rather than via ESPHome wrappers, and every one has its status checked and logged.

## Conventions

- Guard the whole translation unit with `#ifdef USE_ESP32`, and time-dependent code with `#ifdef USE_TIME`.
- Sensors are all optional and nullable — check for `nullptr` before every `publish_state`.
- Public setters are for codegen only; anything the YAML lambda layer calls (currently `reset_daily_energy()`) must be public and documented in the README.
- `CODEOWNERS = ["@WeekendWarrior1"]` credits the original author; this is a derived work.
