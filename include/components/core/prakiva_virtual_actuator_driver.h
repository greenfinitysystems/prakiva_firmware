#pragma once

#include <ArduinoJson.h>
#include "prakiva_actuator_driver.h"
#include "prakiva_sensor_driver.h"

namespace prakiva {

// Virtual actuators expose one product operation while orchestrating private
// physical actuator/sensor dependencies inside their driver implementation.
class IVirtualActuatorDriver : public IActuatorDriver {
public:
    IVirtualActuatorDriver *asVirtualActuatorDriver() override { return this; }
    virtual bool bindActuator(const char *dependencyId, IActuatorDriver *driver) = 0;
    virtual bool bindSensor(const char *dependencyId, ISensorDriver *driver) { (void)dependencyId; (void)driver; return false; }
    virtual bool validateParameters(JsonObjectConst parameters) const { return parameters.size() == 0; }
    virtual bool applyParameters(JsonObjectConst parameters) { return parameters.size() == 0; }
    virtual ~IVirtualActuatorDriver() = default;
};

} // namespace prakiva
