import esphome.codegen as cg
from esphome.components import cover
import esphome.config_validation as cv
from esphome.const import CONF_ADDRESS, CONF_CLOSE_DURATION, CONF_OPEN_DURATION

from . import CONF_SOMFY_RTS_ID, SomfyRtsHub, somfy_rts_ns

DEPENDENCIES = ["somfy_rts"]

CONF_STORAGE_KEY = "storage_key"
CONF_MIN_ROLLING_CODE = "min_rolling_code"
CONF_MY_POSITION = "my_position"
CONF_REMOTES = "remotes"

SomfyRtsCover = somfy_rts_ns.class_("SomfyRtsCover", cover.Cover, cg.Component)

somfy_address = cv.hex_int_range(min=1, max=0xFFFFFF)

CONFIG_SCHEMA = (
    cover.cover_schema(SomfyRtsCover, device_class="shutter")
    .extend(
        {
            cv.GenerateID(CONF_SOMFY_RTS_ID): cv.use_id(SomfyRtsHub),
            # Adresse der virtuellen Fernbedienung. Nach dem Anlernen nie mehr ändern!
            cv.Required(CONF_ADDRESS): somfy_address,
            # NVS-Key für den Rolling Code (max. 15 Zeichen). Nach dem Anlernen nie mehr ändern!
            cv.Required(CONF_STORAGE_KEY): cv.All(cv.string, cv.Length(min=1, max=15)),
            # Sicherheitsnetz: geht der NVS verloren, wird mindestens mit diesem Code weitergemacht
            cv.Optional(CONF_MIN_ROLLING_CODE, default=1): cv.int_range(min=1, max=0xFFFF),
            cv.Optional(CONF_OPEN_DURATION, default="30s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_CLOSE_DURATION, default="30s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_MY_POSITION): cv.percentage,
            # Original-Fernbedienungen (bzw. Kanäle), deren Tasten ins Positionsmodell übernommen werden
            cv.Optional(CONF_REMOTES, default=[]): cv.ensure_list(somfy_address),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await cover.new_cover(config)
    await cg.register_component(var, config)

    hub = await cg.get_variable(config[CONF_SOMFY_RTS_ID])
    cg.add(var.set_hub(hub))
    cg.add(hub.register_cover(var))

    cg.add(var.set_address(config[CONF_ADDRESS]))
    cg.add(var.set_storage_key(config[CONF_STORAGE_KEY]))
    cg.add(var.set_min_rolling_code(config[CONF_MIN_ROLLING_CODE]))
    cg.add(var.set_open_duration(config[CONF_OPEN_DURATION]))
    cg.add(var.set_close_duration(config[CONF_CLOSE_DURATION]))
    if CONF_MY_POSITION in config:
        cg.add(var.set_initial_my_position(config[CONF_MY_POSITION]))
    for remote in config[CONF_REMOTES]:
        cg.add(var.add_remote(remote))
