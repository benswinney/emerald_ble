#include "emerald_ble.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/time.h"

#ifdef USE_ESP32

namespace esphome {
namespace emerald_ble {

static const char *const TAG = "emerald_ble";

void Emerald::setup() {
  // Persisted state hash mixes a constant tag with the meter's MAC address so:
  //  - the same meter reattaches to the same NVS slot across recompiles (unlike
  //    fnv1_hash(App.get_compilation_time()) which invalidates each build);
  //  - multiple meters on one ESP can coexist without colliding.
  uint32_t hash = fnv1_hash(std::string("emerald_ble_state_") + this->parent_->address_str());
  this->pref_state_ = global_preferences->make_preference<EmeraldPersistedState>(hash, true);

  EmeraldPersistedState saved{};
  if (this->pref_state_.load(&saved)) {
    this->total_pulses_ = saved.total_pulses;
    this->daily_pulses_ = saved.daily_pulses;
    this->day_of_last_measurement_ = saved.day_of_last_measurement;
    ESP_LOGI(TAG, "Restored state: total_pulses=%llu daily_pulses=%llu day_of_last=%u",
             (unsigned long long) this->total_pulses_,
             (unsigned long long) this->daily_pulses_,
             this->day_of_last_measurement_);

    // Republish restored totals immediately so HA picks them up before the
    // first BLE packet arrives (which can take a minute or more).
    if (this->energy_sensor_ != nullptr) {
      this->energy_sensor_->publish_state(this->total_pulses_ / this->pulses_per_kwh_);
    }
    if (this->daily_energy_sensor_ != nullptr) {
      this->daily_energy_sensor_->publish_state(this->daily_pulses_ / this->pulses_per_kwh_);
    }
  } else {
    ESP_LOGI(TAG, "No persisted state found (first boot after flash, or NVS cleared)");
  }
}

void Emerald::save_state_(bool force) {
  const uint32_t now = millis();
  if (!force && (now - this->last_save_ms_) < MIN_SAVE_INTERVAL_MS) {
    return;
  }
  this->last_save_ms_ = now;
  EmeraldPersistedState state{
      .total_pulses = this->total_pulses_,
      .daily_pulses = this->daily_pulses_,
      .day_of_last_measurement = this->day_of_last_measurement_,
  };
  this->pref_state_.save(&state);
}

void Emerald::dump_config() {
  ESP_LOGCONFIG(TAG, "EMERALD");
  LOG_SENSOR(" ", "Battery", this->battery_);
  LOG_SENSOR(" ", "Power", this->power_sensor_);
  LOG_SENSOR(" ", "Daily Energy", this->daily_energy_sensor_);
  LOG_SENSOR(" ", "Total Energy", this->energy_sensor_);
  LOG_BINARY_SENSOR(" ", "Connected", this->connected_sensor_);
  ESP_LOGCONFIG(TAG, "  pulses_per_kwh: %f", this->pulses_per_kwh_);
  ESP_LOGCONFIG(TAG, "  pulse_multiplier: %f", this->pulse_multiplier_);
}

void Emerald::reset_connection_state_() {
  this->handles_discovered_ = false;
  this->auth_completed_ = false;
  this->time_read_char_handle_ = 0;
  this->time_write_size_char_handle_ = 0;
  this->battery_char_handle_ = 0;
  this->publish_connected_(false);
}

void Emerald::publish_connected_(bool connected) {
  if (this->connected_sensor_ != nullptr) {
    this->connected_sensor_->publish_state(connected);
  }
}

void Emerald::reset_daily_energy() {
  ESP_LOGI(TAG, "reset_daily_energy() called — zeroing daily accumulator");
  this->daily_pulses_ = 0;
  if (this->daily_energy_sensor_ != nullptr) {
    this->daily_energy_sensor_->publish_state(0.0f);
  }
  // Force the zero to disk immediately so a power-cycle right after a manual
  // reset doesn't restore the previous daily total.
  this->save_state_(true);
}

void Emerald::force_reconnect_() {
  // Close the GATT connection; ble_client's own DISCONNECT_EVT handler will set
  // state to IDLE and the esp32_ble_tracker scan loop will reconnect.
  ESP_LOGI(TAG, "[%s] Forcing GATT disconnect to trigger reconnect", this->parent_->address_str().c_str());
  auto ret = esp_ble_gattc_close(this->parent()->gattc_if, this->parent()->conn_id);
  if (ret) {
    ESP_LOGW(TAG, "[%s] esp_ble_gattc_close failed during forced reconnect, status=%d",
             this->parent_->address_str().c_str(), ret);
  }
}

std::string Emerald::pkt_to_hex_(const uint8_t *data, uint16_t len) {
  // Delegate to ESPHome's helper: handles any length safely (no fixed buffer)
  // and produces consistent formatting with the rest of the codebase.
  return format_hex_pretty(data, len);
}

void Emerald::decode_(const uint8_t *data, uint16_t length) {
  ESP_LOGD(TAG, "DEC(%d): 0x%s", length, this->pkt_to_hex_(data, length).c_str());
}

void Emerald::parse_battery_(const uint8_t *data, uint16_t length) {
  ESP_LOGD(TAG, "Battery: DEC(%d): 0x%s", length, this->pkt_to_hex_(data, length).c_str());
  if (this->battery_ == nullptr) {
    ESP_LOGD(TAG, "Battery sensor not configured, skipping");
    return;
  }
  if (length != 1) {
    ESP_LOGW(TAG, "Unexpected battery data length: %d (expected 1)", length);
    return;
  }
  ESP_LOGI(TAG, "Battery level: %d%%", data[0]);
  this->battery_->publish_state(data[0]);
}

uint64_t Emerald::parse_command_header_(const uint8_t *data) {
    // The command header is 5 bytes, big-endian. Pack into a uint64_t to avoid
    // the previous bug where data[i] (promoted to int) was shifted by 32 — UB
    // on a 32-bit int — and the result was stored in a uint32_t which would
    // truncate the top byte of any future >4-byte header.
    uint64_t command_header = 0;
    for (int i = 0; i < 5; i++) {
        command_header |= static_cast<uint64_t>(data[i]) << (8 * (4 - i));
    }
    return command_header;
}

uint32_t Emerald::decode_emerald_date_(const uint8_t *data) {
    uint32_t command_date_bin = 0;
    for (int i = 5;  i < 9; i++) {
        command_date_bin += (data[i] << (8*(8-i)));
    }
    // // (6 bits)year + (4 bits)month + (5 bits)days + (5 bits)hours(locale adjusted) + (6 bits)minutes + (6 bits)seconds
    // uint16_t year = 2000 + (commandDateBin >> 26);  // need to add 2000 to get the correct year
    // uint8_t month = ((commandDateBin >> 22) & 0b1111);  // month number between 1 - 12
    // uint8_t days = ((commandDateBin >> 17) & 0b11111); // 1-31
    // uint8_t hours = ((commandDateBin >> 12) & 0b11111); // 0-23
    // uint8_t minutes = ((commandDateBin >> 6) & 0b111111); // 0 -59
    // uint8_t seconds = commandDateBin & 0b111111; // 0 -59
    return command_date_bin;
}

void Emerald::decode_emerald_packet_(const uint8_t *data, uint16_t length) {
  ESP_LOGD(TAG, "DEC(%d): 0x%s", length, this->pkt_to_hex_(data, length).c_str());
  if (length >= 5) {
    uint64_t command_header = this->parse_command_header_(data);
    switch(command_header) {
      case RETURN30S_POWER_CONSUMPTION_CMD: {
        if (length != 11) {
          ESP_LOGW(TAG, "RETURN30S_POWER_CONSUMPTION_CMD: unexpected length %d (expected 11), discarding packet",
                   length);
          return;
        }

        uint16_t pulses_within_interval = data[9] << 8;
        pulses_within_interval += data[10];

        float avg_watts_within_interval = pulses_within_interval * this->pulse_multiplier_;

        ESP_LOGI(TAG, "Timestamp: , Pulses: %d, Average Watts within interval: %f W", pulses_within_interval,
                avg_watts_within_interval);

        if (this->power_sensor_ != nullptr) {
          this->power_sensor_->publish_state(avg_watts_within_interval);
        }

        if (this->energy_sensor_ != nullptr) {
          total_pulses_ += pulses_within_interval;
          float energy = total_pulses_ / this->pulses_per_kwh_;
          this->energy_sensor_->publish_state(energy);
        }

        // Hoisted out of the daily_energy_sensor_ branch so the post-publish
        // save_state_() call below can see it regardless of which sensors are
        // configured.
        bool day_rolled_over = false;

        if (this->daily_energy_sensor_ != nullptr) {
          // even if new day, publish last measurement window before resetting
          this->daily_pulses_ += pulses_within_interval;
          float energy = this->daily_pulses_ / this->pulses_per_kwh_;
          this->daily_energy_sensor_->publish_state(energy);


          // Prefer an ESPHome time component if one is wired up AND currently has a valid time.
          // Otherwise fall back to the day-of-month encoded in the Emerald packet itself.
          bool used_esphome_time = false;
#ifdef USE_TIME
          if (this->time_.has_value()) {
            auto *time_component = *this->time_;
            ESPTime date_of_measurement = time_component->now();
            if (date_of_measurement.is_valid()) {
              used_esphome_time = true;
              if (this->day_of_last_measurement_ == 0) {
                this->day_of_last_measurement_ = date_of_measurement.day_of_year;
              } else if (this->day_of_last_measurement_ != date_of_measurement.day_of_year) {
                this->daily_pulses_ = 0;
                this->day_of_last_measurement_ = date_of_measurement.day_of_year;
                day_rolled_over = true;
              }
            }
          }
#endif
          if (!used_esphome_time) {
            // Fallback: extract day-of-month from the Emerald packet timestamp.
            // Bit layout (32 bits): year[31:26] month[25:22] day[21:17] hour[16:12] min[11:6] sec[5:0]
            uint32_t command_date_bin = this->decode_emerald_date_(data);
            uint8_t day_of_measurement = ((command_date_bin >> 17) & 0b11111); // 1-31
            if (this->day_of_last_measurement_ == 0) {
              this->day_of_last_measurement_ = day_of_measurement;
            } else if (this->day_of_last_measurement_ != day_of_measurement) {
              this->daily_pulses_ = 0;
              this->day_of_last_measurement_ = day_of_measurement;
              day_rolled_over = true;
            }
          }
        }
        // Persist accumulators after every 30-second measurement. Throttled to
        // MIN_SAVE_INTERVAL_MS (1 min) for NVS wear, but force-save on day
        // rollover so the freshly-zeroed daily counter is immediately durable.
        this->save_state_(day_rolled_over);

        break;
      }
      case RETURN_UPDATED_POWER_CMD: {
        break;
      }
      case RETURN_EVERY30S_POWER_CONSUMPTION_CMD: {
        break;
      }
      case RETURN_IMPULSE_CMD: {
        break;
      }
      case RETURN_PAIRING_CODE_CMD: {
        break;
      }
      case RETURN_DEVICE_TIME_CMD: {
        break;
      }
    }
  } else {
    ESP_LOGW(TAG, "Packet too short to contain a command header: %d bytes (need >= 5)", length);
  }
}

void Emerald::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                   esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_DISCONNECT_EVT: {
      ESP_LOGI(TAG, "[%s] Disconnected — resetting connection state for next attempt",
               this->parent_->address_str().c_str());
      this->reset_connection_state_();
      break;
    }
    case ESP_GATTC_SEARCH_CMPL_EVT: {
      ESP_LOGD(TAG, "[%s] ESP_GATTC_SEARCH_CMPL_EVT - discovering handles", this->parent_->address_str().c_str());

      // Discover time_read characteristic
      auto *chr = this->parent()->get_characteristic(EMERALD_SERVICE_TIME_UUID, EMERALD_CHARACTERISTIC_TIME_READ_UUID);
      if (chr == nullptr) {
        ESP_LOGW(TAG, "No time read characteristic found at service %s char %s",
                 EMERALD_SERVICE_TIME_UUID.to_string().c_str(),
                 EMERALD_CHARACTERISTIC_TIME_READ_UUID.to_string().c_str());
        break;
      }
      this->time_read_char_handle_ = chr->handle;

      // Discover time_write characteristic
      chr = this->parent()->get_characteristic(EMERALD_SERVICE_TIME_UUID, EMERALD_CHARACTERISTIC_TIME_WRITE_UUID);
      if (chr == nullptr) {
        ESP_LOGW(TAG, "No time write characteristic found at service %s char %s",
                 EMERALD_SERVICE_TIME_UUID.to_string().c_str(),
                 EMERALD_CHARACTERISTIC_TIME_WRITE_UUID.to_string().c_str());
        break;
      }
      this->time_write_size_char_handle_ = chr->handle;

      // Discover battery characteristic (optional - device may not have standard battery service)
      chr = this->parent()->get_characteristic(EMERALD_BATTERY_SERVICE_UUID, EMERALD_BATTERY_CHARACTERISTIC_UUID);
      if (chr == nullptr) {
        ESP_LOGW(TAG, "No battery characteristic found at service %s char %s - battery level will not be available",
                 EMERALD_BATTERY_SERVICE_UUID.to_string().c_str(),
                 EMERALD_BATTERY_CHARACTERISTIC_UUID.to_string().c_str());
        // Don't break - battery is optional
      } else {
        this->battery_char_handle_ = chr->handle;
        ESP_LOGD(TAG, "[%s] Found battery characteristic at handle 0x%04x", this->parent_->address_str().c_str(), this->battery_char_handle_);
      }

      this->handles_discovered_ = true;
      ESP_LOGI(TAG, "[%s] Discovered handles - time_read:0x%04x time_write:0x%04x battery:0x%04x",
               this->parent_->address_str().c_str(),
               this->time_read_char_handle_, this->time_write_size_char_handle_,
               this->battery_char_handle_);

      // If authentication already completed while we were discovering handles, set up communication now
      if (this->auth_completed_) {
        ESP_LOGD(TAG, "[%s] Auth already complete, setting up communication now", this->parent_->address_str().c_str());
        this->setup_communication_();
      }
      break;
    }
    case ESP_GATTC_READ_CHAR_EVT: {
      ESP_LOGD(TAG, "[%s] ESP_GATTC_READ_CHAR_EVT (Received READ)", this->parent_->address_str().c_str());
      if (param->read.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Error reading char at handle %d, status=%d", param->read.handle, param->read.status);
        break;
      }

      // time_read_char_handle_
      if (param->read.handle == this->time_read_char_handle_) {
        ESP_LOGD(TAG, "Received time read event");
        this->decode_emerald_packet_(param->read.value, param->read.value_len);
        break;
      }

      // battery_char_handle_
      if (param->read.handle == this->battery_char_handle_) {
        ESP_LOGD(TAG, "Received battery read event");
        this->parse_battery_(param->read.value, param->read.value_len);
        break;
      }

      break;
    }

    case ESP_GATTC_WRITE_CHAR_EVT: {
      ESP_LOGD(TAG, "[%s] ESP_GATTC_WRITE_CHAR_EVT (Write confirmed)", this->parent_->address_str().c_str());
      if (param->write.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Error writing value to char at handle %d, status=%d", param->write.handle, param->write.status);
        break;
      }

      // ESP_LOGE(TAG, "[%s] Seemed to miss any handle matches, what is the handel?: %d",
      //          this->parent_->address_str().c_str(), param->write.handle);
      break;
    }  // ESP_GATTC_WRITE_CHAR_EVT

    case ESP_GATTC_NOTIFY_EVT: {
      ESP_LOGD(TAG, "[%s] Received notification", this->parent_->address_str().c_str());

      // time_read_char_handle_
      if (param->notify.handle == this->time_read_char_handle_) {
        ESP_LOGD(TAG, "Received time read notification");
        this->decode_emerald_packet_(param->notify.value, param->notify.value_len);
        break;
      }

      // battery
      if (param->notify.handle == this->battery_char_handle_) {
        ESP_LOGD(TAG, "Received battery notify event");
        this->parse_battery_(param->notify.value, param->notify.value_len);
        break;
      }
      break;
    }
    default:
      break;
  }
}

void Emerald::gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  switch (event) {
    // This event is sent once authentication has completed
    case ESP_GAP_BLE_AUTH_CMPL_EVT: {
      if (param->ble_security.auth_cmpl.success) {
        ESP_LOGI(TAG, "[%s] Authentication successful", this->parent_->address_str().c_str());
        this->auth_completed_ = true;

        if (!this->handles_discovered_) {
          ESP_LOGI(TAG, "[%s] Auth complete but handles not yet discovered, will set up communication after discovery",
                   this->parent_->address_str().c_str());
          break;
        }

        this->setup_communication_();
      } else {
        ESP_LOGW(TAG, "[%s] Authentication failed (reason=0x%02x) — forcing reconnect",
                 this->parent_->address_str().c_str(),
                 param->ble_security.auth_cmpl.fail_reason);
        // Without this the meter stays in a half-connected dead state until reboot.
        // Reset our flags and tear down the GATT connection so ble_client/tracker
        // re-establish it on the next scan cycle.
        this->reset_connection_state_();
        this->force_reconnect_();
      }
      break;
    }
    case ESP_GAP_BLE_PASSKEY_REQ_EVT: { /* passkey request event */
      ESP_LOGE(TAG, "ESP_GAP_BLE_PASSKEY_REQ_EVT, onPassKeyRequest %x", event);
      esp_ble_passkey_reply(param->ble_security.ble_req.bd_addr, true, this->pairing_code_);
      break;
    }
    default:
      break;
  }
}

void Emerald::setup_communication_() {
  ESP_LOGI(TAG, "[%s] Setting up communication with Emerald device", this->parent_->address_str().c_str());

  // Register for notifications on the time read characteristic
  auto status = esp_ble_gattc_register_for_notify(this->parent_->gattc_if, this->parent_->remote_bda,
                                                      this->time_read_char_handle_);
  if (status) {
    ESP_LOGW(TAG, "[%s] esp_ble_gattc_register_for_notify failed, status=%d",
              this->parent_->address_str().c_str(), status);
  }

  // Send auto upload command. The IDF write API takes a non-const uint8_t*, so
  // cast away const on the flash-resident buffer; the device only reads from it.
  ESP_LOGI(TAG, "[%s] Writing auto upload code to Emerald", this->parent_->address_str().c_str());
  auto write_status = esp_ble_gattc_write_char(this->parent()->gattc_if, this->parent()->conn_id,
                                         this->time_write_size_char_handle_, sizeof(SET_AUTO_UPLOAD_STATUS_CMD),
                                         const_cast<uint8_t *>(SET_AUTO_UPLOAD_STATUS_CMD),
                                         ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
  if (write_status) {
    ESP_LOGW(TAG, "Error sending write request for auto upload, status=%d", write_status);
  }

  // Read battery level
  if (this->battery_char_handle_ == 0) {
    ESP_LOGW(TAG, "[%s] Battery characteristic handle is 0, skipping battery read", this->parent_->address_str().c_str());
  } else {
    ESP_LOGD(TAG, "[%s] Reading battery level from handle 0x%04x", this->parent_->address_str().c_str(), this->battery_char_handle_);
    auto read_battery_status = esp_ble_gattc_read_char(this->parent()->gattc_if, this->parent()->conn_id,
                                                        this->battery_char_handle_, ESP_GATT_AUTH_REQ_NONE);
    if (read_battery_status) {
      ESP_LOGW(TAG, "Error sending read request for battery, status=%d", read_battery_status);
    }

    // Enable notifications for battery
    auto notify_battery_status = esp_ble_gattc_register_for_notify(
        this->parent_->gattc_if, this->parent_->remote_bda, this->battery_char_handle_);
    if (notify_battery_status) {
      ESP_LOGW(TAG, "[%s] esp_ble_gattc_register_for_notify for battery failed, status=%d",
                this->parent_->address_str().c_str(), notify_battery_status);
    }
  }

  // Communication fully established — mark connected.
  this->publish_connected_(true);
}

}  // namespace emerald_ble
}  // namespace esphome

#endif
