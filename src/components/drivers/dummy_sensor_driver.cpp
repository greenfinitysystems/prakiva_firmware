#include "components/drivers/dummy_sensor_driver.h"
#include "core/prakiva_time.h"
#include <cmath>
#include <esp_system.h>

namespace prakiva
{
    namespace
    {
        bool finiteNumber(JsonVariantConst v)
        {
            return (v.is<float>() || v.is<double>() || v.is<int>() || v.is<unsigned int>()) &&
                   std::isfinite(v.as<double>());
        }
    }
    bool DummySensorDriver::validateParameters(JsonObjectConst p) const
    {
        for (JsonPairConst entry : p)
        {
            const String key = entry.key().c_str();
            if (key != "random" && key != "value" && key != "min" && key != "max")
                return false;
        }
        if (p["random"].is<JsonVariantConst>() && !finiteNumber(p["random"]))
            return false;

        if (p["value"].is<JsonVariantConst>() &&
            !finiteNumber(p["value"]))
            return false;

        if (p["min"].is<JsonVariantConst>() &&
            !finiteNumber(p["min"]))
            return false;

        if (p["max"].is<JsonVariantConst>() &&
            !finiteNumber(p["max"]))
            return false;


        float lo = p["min"].isNull() ? 0.0f : p["min"].as<float>();
        float hi = p["max"].isNull() ? 1.0f : p["max"].as<float>();
        return lo <= hi;
    }
    bool DummySensorDriver::applyParameters(JsonObjectConst p)
    {
        if (!validateParameters(p))
            return false;
        _random = !p["random"].isNull() && p["random"].as<double>() == 1.0;
        _value = p["value"].isNull() ? 0.0f : p["value"].as<float>();
        _min = p["min"].isNull() ? 0.0f : p["min"].as<float>();
        _max = p["max"].isNull() ? 1.0f : p["max"].as<float>();
        return true;
    }
    bool DummySensorDriver::begin()
    {
        _status = ComponentStatus::Ready;
        return true;
    }
    bool DummySensorDriver::get(SensorValue &reading)
    {
        if (_status != ComponentStatus::Ready)
            return false;
        float value = _value;
        if (_random && _max > _min)
            value = _min + (_max - _min) * (static_cast<double>(esp_random()) / 4294967295.0);
        reading = SensorValue();
        reading.value.numericValue = String(value, 3);
        reading.value.valueType = ValueType::Float32;
        reading.unit = _unit;
        uint32_t now = 0;
        reading.timestamp = getCurrentUnixTime(now) ? now : 0;
        reading.valid = true;
        reading.status = MeasurementStatus::Ok;
        return true;
    }
}
