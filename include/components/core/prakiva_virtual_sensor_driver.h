#pragma once

#include <ArduinoJson.h>

#include "prakiva_sensor_driver.h"

namespace prakiva
{

// A virtual sensor derives its measurement from one or more other sensor
// components. The product runtime binds named inputs before begin() is called.
// Implementations remain ordinary ISensorDriver instances to all consumers.
class IVirtualSensorDriver : public ISensorDriver
{
public:
    IVirtualSensorDriver *asVirtualSensorDriver() override { return this; }

    // Bind one declared input alias to an existing sensor component. Returns
    // false when the alias is not accepted or the supplied driver is invalid.
    virtual bool bindInput(const char *inputId, ISensorDriver *driver) = 0;

    // Optional fixed measurement metadata from the product specification.
    // Not an app-configurable virtual sensor parameter.
    virtual void setMeasurementUnit(const String &unit) { (void)unit; }

    // Parameter values are supplied by the product runtime after being
    // validated against the Product Specification schema. Drivers may add
    // implementation-specific validation before the values are activated.
    virtual bool validateParameters(JsonObjectConst parameters) const
    {
        return parameters.size() == 0;
    }

    virtual bool applyParameters(JsonObjectConst parameters)
    {
        return parameters.size() == 0;
    }

    virtual ~IVirtualSensorDriver() = default;
};

} // namespace prakiva
