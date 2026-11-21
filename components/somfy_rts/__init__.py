import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation, pins
from esphome.const import CONF_FREQUENCY, CONF_ID, CONF_TRIGGER_ID

CODEOWNERS = ["@XiCode"]
MULTI_CONF = False

CONF_SOMFY_RTS_ID = "somfy_rts_id"
CONF_SCK_PIN = "sck_pin"
CONF_MISO_PIN = "miso_pin"
CONF_MOSI_PIN = "mosi_pin"
CONF_CS_PIN = "cs_pin"
CONF_GDO0_PIN = "gdo0_pin"
CONF_GDO2_PIN = "gdo2_pin"
CONF_TX_POWER = "tx_power"
CONF_RX_FREQUENCY = "rx_frequency"
CONF_RX_BANDWIDTH = "rx_bandwidth"
CONF_ON_RECEIVE = "on_receive"

somfy_rts_ns = cg.esphome_ns.namespace("somfy_rts")
SomfyRtsHub = somfy_rts_ns.class_("SomfyRtsHub", cg.Component)
ReceiveTrigger = somfy_rts_ns.class_(
    "ReceiveTrigger",
    automation.Trigger.template(cg.std_string, cg.std_string, cg.uint16),
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SomfyRtsHub),
            cv.Optional(CONF_SCK_PIN, default=18): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_MISO_PIN, default=19): pins.internal_gpio_input_pin_number,
            cv.Optional(CONF_MOSI_PIN, default=23): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_CS_PIN, default=5): pins.internal_gpio_output_pin_number,
            # GDO0 = Dateneingang beim Senden, GDO2 = Datenausgang beim Empfangen
            cv.Optional(CONF_GDO0_PIN, default=4): pins.internal_gpio_output_pin_number,
            cv.Optional(CONF_GDO2_PIN, default=2): pins.internal_gpio_input_pin_number,
            cv.Optional(CONF_FREQUENCY, default="433.42MHz"): cv.frequency,
            cv.Optional(CONF_TX_POWER, default=10): cv.int_range(min=-30, max=10),
            # Empfang: ohne Angabe wie "frequency". Fuer Somfy + 433,92-MHz-Protokolle (rf433)
            # z. B. 433.67MHz mit 812kHz Bandbreite.
            cv.Optional(CONF_RX_FREQUENCY): cv.frequency,
            cv.Optional(CONF_RX_BANDWIDTH, default="100kHz"): cv.All(
                cv.frequency, cv.Range(min=58e3, max=812e3)
            ),
            cv.Optional(CONF_ON_RECEIVE): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ReceiveTrigger)}
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    cv.only_with_arduino,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(
        var.set_spi_pins(
            config[CONF_SCK_PIN],
            config[CONF_MISO_PIN],
            config[CONF_MOSI_PIN],
            config[CONF_CS_PIN],
        )
    )
    cg.add(var.set_gdo_pins(config[CONF_GDO0_PIN], config[CONF_GDO2_PIN]))
    cg.add(var.set_frequency(config[CONF_FREQUENCY] / 1e6))
    cg.add(var.set_tx_power(config[CONF_TX_POWER]))
    cg.add(var.set_rx_frequency(config.get(CONF_RX_FREQUENCY, config[CONF_FREQUENCY]) / 1e6))
    cg.add(var.set_rx_bandwidth(config[CONF_RX_BANDWIDTH] / 1e3))

    cg.add_library("SPI", None)
    cg.add_library("lsatan/SmartRC-CC1101-Driver-Lib", "3.0.2")

    for conf in config.get(CONF_ON_RECEIVE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger,
            [(cg.std_string, "remote"), (cg.std_string, "command"), (cg.uint16, "rolling_code")],
            conf,
        )
