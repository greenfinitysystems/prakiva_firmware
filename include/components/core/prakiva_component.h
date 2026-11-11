#pragma once

#include <Arduino.h>
#include <stdint.h>

namespace prakiva
{

    class ISensorDriver;
    class IVirtualSensorDriver;
    class IVirtualActuatorDriver;
    class IActuatorDriver;

    enum class ComponentStatus : uint8_t
    {
        NotInitialized = 0,
        Ready,
        Error
    };

    class IComponent
    {
    public:
        virtual bool begin() = 0;
        virtual ComponentStatus status() const = 0;
        virtual ISensorDriver *asSensorDriver() { return nullptr; }
        virtual IVirtualSensorDriver *asVirtualSensorDriver() { return nullptr; }
        virtual IVirtualActuatorDriver *asVirtualActuatorDriver() { return nullptr; }
        virtual IActuatorDriver *asActuatorDriver() { return nullptr; }
        virtual ~IComponent() = default;
    };

} // namespace prakiva
