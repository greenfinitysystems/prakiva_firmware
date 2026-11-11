#pragma once

#include <Arduino.h>
#include "components/core/prakiva_component_types.h"

namespace prakiva
{

enum class CapabilityOperationStatus : uint8_t
{
    Ok = 0,
    ResolverNotReady,
    NotFound,
    NotOperational,
    NotActuator,
    InterfaceMismatch,
    InvalidValue,
    OutOfRange,
    DriverRejected
};

struct CapabilityReadResult
{
    CapabilityOperationStatus status = CapabilityOperationStatus::NotFound;
    String kind;
    bool available = false;
    NumericValue actuatorValue;
    SensorValue sensorValue;
};

struct CapabilityWriteResult
{
    CapabilityOperationStatus status = CapabilityOperationStatus::NotFound;
    String kind;
    bool writeApplied = false;
    bool readbackAvailable = false;
    NumericValue appliedValue;
    NumericValue currentValue;
};

CapabilityReadResult readCapabilityChannel(const char *capabilityId, const char *channelId);
CapabilityWriteResult writeCapabilityChannel(
    const char *capabilityId,
    const char *channelId,
    const NumericValue &value);

} // namespace prakiva
