import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv

from .. import device_schema, rf433_ns, setup_device

DEPENDENCIES = ["rf433"]

Rf433BinarySensor = rf433_ns.class_(
    "Rf433BinarySensor", binary_sensor.BinarySensor, cg.Component
)

CONFIG_SCHEMA = device_schema(
    binary_sensor.binary_sensor_schema(Rf433BinarySensor).extend(cv.COMPONENT_SCHEMA),
    off_code_required=False,
)


async def to_code(config):
    var = await binary_sensor.new_binary_sensor(config)
    await cg.register_component(var, config)
    await setup_device(var, config)
