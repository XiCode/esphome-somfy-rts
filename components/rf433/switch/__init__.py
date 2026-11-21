import esphome.codegen as cg
from esphome.components import switch
import esphome.config_validation as cv

from .. import device_schema, rf433_ns, setup_device

DEPENDENCIES = ["rf433"]

Rf433Switch = rf433_ns.class_("Rf433Switch", switch.Switch, cg.Component)

CONFIG_SCHEMA = device_schema(
    switch.switch_schema(Rf433Switch).extend(cv.COMPONENT_SCHEMA),
    off_code_required=True,
)


async def to_code(config):
    var = await switch.new_switch(config)
    await cg.register_component(var, config)
    await setup_device(var, config)
