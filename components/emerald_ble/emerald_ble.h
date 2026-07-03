#pragma once

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/core/defines.h"

#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif

#ifdef USE_ESP32

#include <esp_gattc_api.h>

namespace esphome {
namespace emerald_ble {

namespace espbt = esphome::esp32_ble_tracker;

static const espbt::ESPBTUUID EMERALD_SERVICE_TIME_UUID =
    espbt::ESPBTUUID::from_raw("00001910-0000-1000-8000-00805f9b34fb");
static const espbt::ESPBTUUID EMERALD_CHARACTERISTIC_TIME_READ_UUID =
    espbt::ESPBTUUID::from_raw("00002b10-0000-1000-8000-00805f9b34fb");  // indicate, notify, read, write
static const espbt::ESPBTUUID EMERALD_CHARACTERISTIC_TIME_WRITE_UUID =
    espbt::ESPBTUUID::from_raw("00002b11-0000-1000-8000-00805f9b34fb");  // indicate, notify, read, write

static const espbt::ESPBTUUID EMERALD_BATTERY_SERVICE_UUID = espbt::ESPBTUUID::from_uint16(0x180F);
static const espbt::ESPBTUUID EMERALD_BATTERY_CHARACTERISTIC_UUID = espbt::ESPBTUUID::from_uint16(0x2A19);

// Enable 30-second auto-upload of power measurements.
static const uint8_t SET_AUTO_UPLOAD_STATUS_CMD[] = {0x00, 0x01, 0x02, 0x0b, 0x01, 0x01};

// 5-byte command headers, big-endian packed into a uint64_t (top 24 bits unused).
static const uint64_t RETURN30S_POWER_CONSUMPTION_CMD =      0x0001020a06ULL;
static const uint64_t RETURN_UPDATED_POWER_CMD =             0x0001020204ULL;
static const uint64_t RETURN_EVERY30S_POWER_CONSUMPTION_CMD = 0x000102050eULL;
static const uint64_t RETURN_IMPULSE_CMD =                   0x0001010602ULL;
static const uint64_t RETURN_PAIRING_CODE_CMD =              0x0001030206ULL;
static const uint64_t RETURN_DEVICE_TIME_CMD =               0x0001010304ULL;

static const uint8_t standard_update_interval = 30;    // seconds
static const float kw_to_w_conversion = 1000.0;    // conversion ratio
static const float hr_to_s_conversion = 3600.0;


/// Pulse counters and day marker persisted across reboots so HA's
/// total_increasing long-term statistics don't reset on every restart.
struct EmeraldPersistedState {
  uint64_t total_pulses;
  uint64_t daily_pulses;
  uint8_t day_of_last_measurement;
} __attribute__((packed));

class Emerald : public esphome::ble_client::BLEClientNode, public Component {
 public:
  void setup() override;
  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;
  void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }
  void set_battery(sensor::Sensor *battery) { battery_ = battery; }
  void set_power_sensor(sensor::Sensor *power_sensor) { power_sensor_ = power_sensor; }
  void set_energy_sensor(sensor::Sensor *energy_sensor) { energy_sensor_ = energy_sensor; }
  void set_daily_energy_sensor(sensor::Sensor *daily_energy_sensor) { daily_energy_sensor_ = daily_energy_sensor; }
  void set_connected_binary_sensor(binary_sensor::BinarySensor *connected) { connected_sensor_ = connected; }
  /// Zero the daily energy accumulator and republish 0. Intended to be called
  /// from a YAML lambda (e.g. a template button) for manual resets.
  void reset_daily_energy();
#ifdef USE_TIME
  void set_time(time::RealTimeClock *time) { this->time_ = time; }
#endif
  void set_pulses_per_kwh(uint16_t pulses_per_kwh) {
    pulses_per_kwh_ = pulses_per_kwh;
    // disabled for now pulse_multiplier_ = (standard_update_interval / (pulses_per_kwh / kw_to_w_conversion));
    pulse_multiplier_ = ((hr_to_s_conversion * kw_to_w_conversion) / (standard_update_interval * pulses_per_kwh));
  }
  void set_pairing_code(uint32_t pairing_code) { pairing_code_ = pairing_code; }

 protected:
  std::string pkt_to_hex_(const uint8_t *data, uint16_t len);
  void decode_(const uint8_t *data, uint16_t length);
  void parse_battery_(const uint8_t *data, uint16_t length);
  void parse_measurement_(const uint8_t *data, uint16_t length);
  uint64_t parse_command_header_(const uint8_t *data);
  uint32_t decode_emerald_date_(const uint8_t *data);
  void decode_emerald_packet_(const uint8_t *data, uint16_t length);

  sensor::Sensor *battery_{nullptr};
  sensor::Sensor *power_sensor_{nullptr};
  sensor::Sensor *energy_sensor_{nullptr};
  sensor::Sensor *daily_energy_sensor_{nullptr};
  binary_sensor::BinarySensor *connected_sensor_{nullptr};

  void publish_connected_(bool connected);
#ifdef USE_TIME
  optional<time::RealTimeClock *> time_{};
#endif
  uint8_t day_of_last_measurement_{0};

  uint32_t pairing_code_;
  float pulses_per_kwh_;
  float pulse_multiplier_;
  uint64_t daily_pulses_{0};
  uint64_t total_pulses_{0};

  uint16_t time_read_char_handle_{0};
  uint16_t time_write_size_char_handle_{0};
  uint16_t battery_char_handle_{0};
  bool handles_discovered_{false};
  bool auth_completed_{false};

  void setup_communication_();
  void reset_connection_state_();
  void force_reconnect_();
  /// Save persisted state to flash. Throttled to MIN_SAVE_INTERVAL_MS unless
  /// force=true (e.g. day rollover, manual daily reset).
  void save_state_(bool force);

  ESPPreferenceObject pref_state_;
  uint32_t last_save_ms_{0};
  static const uint32_t MIN_SAVE_INTERVAL_MS = 60000;  // 1 minute — bounds NVS wear
};

}  // namespace emerald_ble
}  // namespace esphome

#endif
