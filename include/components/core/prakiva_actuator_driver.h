#pragma once

#include "prakiva_component.h"
#include "prakiva_component_types.h"

namespace prakiva
{

    class IActuatorDriver : public IComponent
    {
    public:
        IActuatorDriver *asActuatorDriver() override { return this; }
        // The accepted value type and range are defined by the product/capability
        // declaration and validated by the concrete driver.
        virtual bool get(NumericValue &value) = 0;
        virtual bool set(const NumericValue &value) = 0;
        // Called once per controller loop. Physical actuators need no periodic work;
        // virtual actuators may override this to advance nonblocking operations.
        virtual void update() {}
        // Convenience semantic operation used by binary and trigger actuators:
        // true=start/on, false=off/stop-request according to the public type.
        bool setState(bool state)
        {
            NumericValue value;
            value.numericValue = state ? "1" : "0";
            value.valueType = ValueType::UInt32;
            return set(value);
        }
        virtual ~IActuatorDriver() = default;
    };

} // namespace prakiva
