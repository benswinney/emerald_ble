# emerald_ble

An [ESPHome](https://esphome.io) external component for the **Emerald Electricity Advisor** — a BLE energy monitor. It connects over BLE, authenticates with your pairing code, and exposes power, energy (total + daily), battery level, and a connectivity binary sensor.

This is packaged as a **standalone external component** so it overlays cleanly on top of any modern ESPHome version — it pulls only `emerald_ble` from this repo and lets `ble_client` / `esp32_ble_tracker` resolve to your installed ESPHome build.

> Derived from the original work by [@WeekendWarrior1](https://github.com/WeekendWarrior1).

## Usage

Add this repo as an external component source, then configure the sensor:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/benswinney/emerald_ble.git
      ref: main
    components: [emerald_ble]

# BLE stack (provided by your installed ESPHome, not this repo)
esp32_ble_tracker:
ble_client:
  - mac_address: "XX:XX:XX:XX:XX:XX"   # your Emerald Advisor's MAC
    id: emerald_client

# Optional but recommended: real time for daily energy rollover
time:
  - platform: sntp
    id: sntp_time

sensor:
  - platform: emerald_ble
    ble_client_id: emerald_client
    time_id: sntp_time
    pairing_code: 123456        # 6-digit code from the Emerald app
    pulses_per_kwh: 1000        # from your meter / Emerald config
    power:
      name: "Emerald Power"
    energy:
      name: "Emerald Energy"
    daily_energy:
      name: "Emerald Daily Energy"
    battery_level:
      name: "Emerald Battery"

binary_sensor:
  - platform: emerald_ble
    connected:
      name: "Emerald Connected"
```

### Manual daily-energy reset

`reset_daily_energy()` is callable from a lambda, e.g. a template button:

```yaml
button:
  - platform: template
    name: "Reset Emerald Daily Energy"
    on_press:
      - lambda: 'id(my_emerald).reset_daily_energy();'
```

(Give the `emerald_ble` sensor an `id:` to reference it.)

## Requirements

- ESP32 target (`USE_ESP32`)
- A recent ESPHome (ESP-IDF framework). The BLE dependencies come from your ESPHome install, so keep ESPHome up to date.
