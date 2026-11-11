#include "components/drivers/binary_actuator_driver.h"
#include <Arduino.h>
namespace prakiva {
bool BinaryActuatorDriver::configurePin(int gpio) {
    if (_status != ComponentStatus::NotInitialized || gpio < 0 || gpio > 48) return false;
    _gpio = gpio; return true;
}
bool BinaryActuatorDriver::begin() {
    if (_gpio < 0) { _status = ComponentStatus::Error; return false; }
    digitalWrite(_gpio, LOW);
    pinMode(_gpio, OUTPUT);
    digitalWrite(_gpio, LOW);
    _on = false; _status = ComponentStatus::Ready; return true;
}
bool BinaryActuatorDriver::get(NumericValue &value) {
    if (_status != ComponentStatus::Ready) return false;
    value.numericValue = _on ? "1" : "0"; value.valueType = ValueType::UInt32; return true;
}
bool BinaryActuatorDriver::set(const NumericValue &value) {
    if (_status != ComponentStatus::Ready || (value.valueType != ValueType::UInt32 && value.valueType != ValueType::Int32)) return false;
    if (value.numericValue != "0" && value.numericValue != "1") return false;
    _on = value.numericValue == "1";
    digitalWrite(_gpio, _on ? HIGH : LOW); return true;
}
}
