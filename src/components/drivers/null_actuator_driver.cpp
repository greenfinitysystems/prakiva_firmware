#include "components/drivers/null_actuator_driver.h"
#include <Arduino.h>
#include <cmath>
#include <cstdlib>

namespace prakiva {
bool NullActuatorDriver::begin() {
    _last.numericValue = "0";
    _last.valueType = ValueType::Float32;
    _status = ComponentStatus::Ready;
    return true;
}
bool NullActuatorDriver::get(NumericValue &value) {
    if (_status != ComponentStatus::Ready) return false;
    value = _last;
    return true;
}
bool NullActuatorDriver::set(const NumericValue &value) {
    if (_status != ComponentStatus::Ready || !value.isValid()) return false;
    char *end = nullptr;
    double n = strtod(value.numericValue.c_str(), &end);
    if (end == value.numericValue.c_str() || *end != '\0' || !std::isfinite(n)) return false;
    if (value.numericValue != _last.numericValue || value.valueType != _last.valueType) {
        _last = value;
        Serial.printf("[NULL_ACTUATOR] commanded=%s type=%s (no hardware action)\n",
                      _last.numericValue.c_str(), valueTypeName(_last.valueType));
    }
    return true;
}
}
