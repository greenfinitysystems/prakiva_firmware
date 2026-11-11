#pragma once

#include <Arduino.h>
#include <stdint.h>

namespace prakiva
{

    // Stable, language-independent names are used when serialising values.
    enum class ValueType : uint8_t
    {
        Int32 = 0,
        UInt32,
        Float32,
        Float64
    };

    inline const char *valueTypeName(ValueType type)
    {
        switch (type)
        {
        case ValueType::Int32:
            return "int32";
        case ValueType::UInt32:
            return "uint32";
        case ValueType::Float32:
            return "float32";
        case ValueType::Float64:
            return "float64";
        default:
            return "unknown";
        }
    }

    // Numeric values are represented as strings at component/API boundaries.
    // Convert to a numeric type only when a control policy actually needs it.
    struct NumericValue
    {
        String numericValue;
        ValueType valueType = ValueType::Float32;

        bool isValid() const
        {
            return numericValue.length() > 0;
        }
    };

    enum class MeasurementStatus : uint8_t
    {
        Ok = 0,
        Invalid,
        SensorDisconnected,
        OutOfRange,
        Error
    };

    inline const char *measurementStatusName(MeasurementStatus status)
    {
        switch (status)
        {
        case MeasurementStatus::Ok:
            return "ok";
        case MeasurementStatus::Invalid:
            return "invalid";
        case MeasurementStatus::SensorDisconnected:
            return "sensor_disconnected";
        case MeasurementStatus::OutOfRange:
            return "out_of_range";
        case MeasurementStatus::Error:
            return "error";
        default:
            return "unknown";
        }
    }

    struct SensorValue
    {
        NumericValue value;
        String unit;
        uint64_t timestamp = 0; // Unix epoch seconds; zero means unavailable.
        bool valid = false;
        MeasurementStatus status = MeasurementStatus::Invalid;
    };

} // namespace prakiva
