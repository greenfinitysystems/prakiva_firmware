#pragma once

#include "prakiva_component.h"
#include "prakiva_component_types.h"

namespace prakiva
{

    class ISensorDriver : public IComponent
    {
    public:
        ISensorDriver *asSensorDriver() override { return this; }
        // Acquires one measurement. A successful call does not necessarily
        // mean the measurement is valid; inspect SensorValue::valid/status.
        virtual bool get(SensorValue &reading) = 0;
        virtual ~ISensorDriver() = default;
    };

} // namespace prakiva
