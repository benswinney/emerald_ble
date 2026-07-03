import esphome.codegen as cg
from esphome.components import ble_client

CODEOWNERS = ["@WeekendWarrior1"]
DEPENDENCIES = ["ble_client"]

emerald_ble_ns = cg.esphome_ns.namespace("emerald_ble")
Emerald = emerald_ble_ns.class_("Emerald", ble_client.BLEClientNode, cg.Component)

CONF_EMERALD_BLE_ID = "emerald_ble_id"
