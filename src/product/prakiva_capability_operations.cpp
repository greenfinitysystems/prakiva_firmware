#include <stdlib.h>
#include <math.h>
#include <limits.h>

#include "product/prakiva_capability_operations.h"

#include "components/core/prakiva_actuator_driver.h"
#include "components/core/prakiva_sensor_driver.h"
#include "product/prakiva_capability_resolver.h"
#include "product/prakiva_product_runtime.h"
#include "core/prakiva_data_acquisition.h"

namespace prakiva
{

CapabilityReadResult readCapabilityChannel(const char *capabilityId, const char *channelId)
{
    CapabilityReadResult result;

    if (!productCapabilityResolverReady())
    {
        result.status = CapabilityOperationStatus::ResolverNotReady;
        return result;
    }

    const CapabilityChannelResolution resolution =
        productCapabilityResolver().resolve(capabilityId, channelId);
    result.kind = resolution.kind;

    if (resolution.status == CapabilityResolutionStatus::NotFound)
    {
        result.status = CapabilityOperationStatus::NotFound;
        return result;
    }

    if (resolution.status != CapabilityResolutionStatus::Operational || !resolution.component)
    {
        result.status = CapabilityOperationStatus::NotOperational;
        return result;
    }

    if (resolution.kind == "sensor")
    {
        ISensorDriver *driver = resolution.component->asSensorDriver();
        if (!driver)
        {
            result.status = CapabilityOperationStatus::InterfaceMismatch;
            return result;
        }
        const SensorSnapshotEntry *snapshot = findSensorSnapshot(resolution.componentId.c_str());
        if (snapshot)
        {
            result.sensorValue = snapshot->reading;
            result.available = snapshot->readSucceeded;
        }
        else
        {
            result.available = false;
        }
        result.status = CapabilityOperationStatus::Ok;
        return result;
    }

    if (resolution.kind == "actuator")
    {
        IActuatorDriver *driver = resolution.component->asActuatorDriver();
        if (!driver)
        {
            result.status = CapabilityOperationStatus::InterfaceMismatch;
            return result;
        }
        result.available = driver->get(result.actuatorValue);
        result.status = CapabilityOperationStatus::Ok;
        return result;
    }

    result.status = CapabilityOperationStatus::InterfaceMismatch;
    return result;
}

CapabilityWriteResult writeCapabilityChannel(
    const char *capabilityId,
    const char *channelId,
    const NumericValue &value)
{
    CapabilityWriteResult result;

    if (!productCapabilityResolverReady())
    {
        result.status = CapabilityOperationStatus::ResolverNotReady;
        return result;
    }

    const CapabilityChannelResolution resolution =
        productCapabilityResolver().resolve(capabilityId, channelId);
    result.kind = resolution.kind;

    if (resolution.status == CapabilityResolutionStatus::NotFound)
    {
        result.status = CapabilityOperationStatus::NotFound;
        return result;
    }

    if (resolution.kind != "actuator")
    {
        result.status = CapabilityOperationStatus::NotActuator;
        return result;
    }

    if (resolution.status != CapabilityResolutionStatus::Operational || !resolution.component)
    {
        result.status = CapabilityOperationStatus::NotOperational;
        return result;
    }

    IActuatorDriver *driver = resolution.component->asActuatorDriver();
    if (!driver)
    {
        result.status = CapabilityOperationStatus::InterfaceMismatch;
        return result;
    }

    if (!value.isValid())
    {
        result.status = CapabilityOperationStatus::InvalidValue;
        return result;
    }

    char *end = nullptr;
    const char *raw = value.numericValue.c_str();
    const double numeric = strtod(raw, &end);
    if (end == raw || *end != '\0' || !isfinite(numeric))
    {
        result.status = CapabilityOperationStatus::InvalidValue;
        return result;
    }

    // Enforce the lexical and numeric constraints of the declared transport type.
    if (value.valueType == ValueType::Int32)
    {
        if (floor(numeric) != numeric || numeric < INT32_MIN || numeric > INT32_MAX)
        {
            result.status = CapabilityOperationStatus::InvalidValue;
            return result;
        }
    }
    else if (value.valueType == ValueType::UInt32)
    {
        if (floor(numeric) != numeric || numeric < 0.0 || numeric > UINT32_MAX)
        {
            result.status = CapabilityOperationStatus::InvalidValue;
            return result;
        }
    }
    else if (value.valueType == ValueType::Float32)
    {
        char *floatEnd = nullptr;
        const float parsedFloat = strtof(raw, &floatEnd);
        if (floatEnd == raw || *floatEnd != '\0' || !isfinite(parsedFloat))
        {
            result.status = CapabilityOperationStatus::InvalidValue;
            return result;
        }
    }

    // Validate against the public channel declaration, not driver-specific rules.
    if (resolution.declaredValueType == "binary")
    {
        if ((value.valueType != ValueType::Int32 && value.valueType != ValueType::UInt32) ||
            (value.numericValue != "0" && value.numericValue != "1"))
        {
            result.status = CapabilityOperationStatus::InvalidValue;
            return result;
        }
    }
    else if (resolution.declaredValueType == "analog")
    {
        if (value.valueType != ValueType::Int32 &&
            value.valueType != ValueType::UInt32 &&
            value.valueType != ValueType::Float32 &&
            value.valueType != ValueType::Float64)
        {
            result.status = CapabilityOperationStatus::InvalidValue;
            return result;
        }

        if ((resolution.hasMinimum && numeric < resolution.minimum) ||
            (resolution.hasMaximum && numeric > resolution.maximum))
        {
            result.status = CapabilityOperationStatus::OutOfRange;
            return result;
        }
    }
    else if (resolution.declaredValueType == "trigger")
    {
        // Trigger actuators use 1=start/execute and 0=stop request. The driver
        // owns the operation lifecycle and any safe-stop sequence.
        if ((value.valueType != ValueType::Int32 && value.valueType != ValueType::UInt32) ||
            (value.numericValue != "0" && value.numericValue != "1"))
        {
            result.status = CapabilityOperationStatus::InvalidValue;
            return result;
        }
    }
    else
    {
        result.status = CapabilityOperationStatus::InvalidValue;
        return result;
    }

    if (!driver->set(value))
    {
        result.status = CapabilityOperationStatus::DriverRejected;
        return result;
    }

    result.writeApplied = true;
    result.appliedValue = value;
    result.readbackAvailable = driver->get(result.currentValue);
    // A successful driver set is a successful write operation. Readback is
    // reported independently because some actuators may not support it reliably.
    result.status = CapabilityOperationStatus::Ok;
    return result;
}

} // namespace prakiva
