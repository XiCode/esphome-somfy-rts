import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import somfy_rts
from esphome.const import CONF_FREQUENCY, CONF_ID, CONF_TRIGGER_ID

CODEOWNERS = ["@XiCode"]
DEPENDENCIES = ["somfy_rts"]
MULTI_CONF = False

CONF_RF433_ID = "rf433_id"
CONF_DUMP_UNKNOWN = "dump_unknown"
CONF_ON_RECEIVE = "on_receive"
CONF_MQTT_DISCOVERY = "mqtt_discovery"
CONF_PROTOCOL = "protocol"
CONF_ON_CODE = "on_code"
CONF_OFF_CODE = "off_code"
CONF_ADDRESS = "address"
CONF_UNIT = "unit"
CONF_PULSE_LENGTH = "pulse_length"
CONF_REPEATS = "repeats"

rf433_ns = cg.esphome_ns.namespace("rf433")
Rf433Component = rf433_ns.class_("Rf433Component", cg.Component)
ReceiveTrigger = rf433_ns.class_(
    "ReceiveTrigger",
    automation.Trigger.template(cg.std_string, cg.std_string, cg.bool_),
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(Rf433Component),
        cv.GenerateID(somfy_rts.CONF_SOMFY_RTS_ID): cv.use_id(somfy_rts.SomfyRtsHub),
        cv.Optional(CONF_FREQUENCY, default="433.92MHz"): cv.frequency,
        # Schreibt laengere, nicht erkannte Pulsfolgen ins Log (zum Aufspueren fremder Protokolle)
        cv.Optional(CONF_DUMP_UNKNOWN, default=False): cv.boolean,
        # Geraete ueber MQTT-Discovery in Home Assistant anlegen (braucht den Block "mqtt:")
        cv.Optional(CONF_MQTT_DISCOVERY, default=False): cv.All(
            cv.boolean,
            lambda value: cv.requires_component("mqtt")(value) if value else value,
        ),
        cv.Optional(CONF_ON_RECEIVE): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ReceiveTrigger)}
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    hub = await cg.get_variable(config[somfy_rts.CONF_SOMFY_RTS_ID])
    cg.add(var.set_hub(hub))
    cg.add(var.set_frequency(config[CONF_FREQUENCY] / 1e6))
    cg.add(var.set_dump_unknown(config[CONF_DUMP_UNKNOWN]))
    cg.add(var.set_mqtt_discovery(config[CONF_MQTT_DISCOVERY]))

    for conf in config.get(CONF_ON_RECEIVE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger,
            [(cg.std_string, "protocol"), (cg.std_string, "code"), (cg.bool_, "state")],
            conf,
        )


# --------------------------------------------------
# Gemeinsam fuer switch und binary_sensor
# --------------------------------------------------

_COMMON = {
    cv.GenerateID(CONF_RF433_ID): cv.use_id(Rf433Component),
    # Grundpulslaenge T, ohne Angabe 350 us (pt2262) bzw. 260 us (ac)
    cv.Optional(CONF_PULSE_LENGTH): cv.All(
        cv.positive_time_period_microseconds,
        cv.Range(
            min=cv.TimePeriod(microseconds=100), max=cv.TimePeriod(microseconds=1000)
        ),
    ),
    cv.Optional(CONF_REPEATS): cv.int_range(min=1, max=20),
}


def device_schema(base, off_code_required):
    """Schema je Protokoll: pt2262 mit Codes, ac mit Adresse und Unit."""
    off_key = (
        cv.Required(CONF_OFF_CODE) if off_code_required else cv.Optional(CONF_OFF_CODE)
    )
    pt2262 = base.extend(_COMMON).extend(
        {
            cv.Required(CONF_ON_CODE): cv.hex_int_range(min=0, max=0xFFFFFF),
            off_key: cv.hex_int_range(min=0, max=0xFFFFFF),
        }
    )
    ac = base.extend(_COMMON).extend(
        {
            cv.Required(CONF_ADDRESS): cv.hex_int_range(min=0, max=0x3FFFFFF),
            cv.Required(CONF_UNIT): cv.int_range(min=1, max=16),
        }
    )
    return cv.typed_schema({"pt2262": pt2262, "ac": ac}, key=CONF_PROTOCOL, lower=True)


async def setup_device(var, config):
    parent = await cg.get_variable(config[CONF_RF433_ID])
    cg.add(var.set_parent(parent))
    cg.add(parent.register_listener(var))
    if config[CONF_PROTOCOL] == "ac":
        cg.add(var.set_ac(config[CONF_ADDRESS], config[CONF_UNIT]))
    else:
        cg.add(
            var.set_pt2262(
                config[CONF_ON_CODE],
                CONF_OFF_CODE in config,
                config.get(CONF_OFF_CODE, 0),
            )
        )
    if CONF_PULSE_LENGTH in config:
        cg.add(var.set_pulse_length(config[CONF_PULSE_LENGTH].total_microseconds))
    if CONF_REPEATS in config:
        cg.add(var.set_repeats(config[CONF_REPEATS]))
