#include "components/drivers/pwm_actuator_driver.h"
#include <Arduino.h>
#include <cmath>
#include <cstdlib>
namespace prakiva {
bool PwmActuatorDriver::configurePin(int gpio) {
    if (_status != ComponentStatus::NotInitialized || gpio < 0 || gpio > 48) return false;
    _gpio = gpio; return true;
}
bool PwmActuatorDriver::begin() {
    if (_gpio < 0) { _status = ComponentStatus::Error; return false; }
    pinMode(_gpio, OUTPUT);
    analogWriteResolution(_gpio, 8);
    analogWriteFrequency(_gpio, 5000);
    analogWrite(_gpio, 0);
    _percent = 0; _status = ComponentStatus::Ready; return true;
}
bool PwmActuatorDriver::get(NumericValue &value) {
    if (_status != ComponentStatus::Ready) return false;
    value.numericValue = String(_percent, 3); value.valueType = ValueType::Float32; return true;
}
bool PwmActuatorDriver::set(const NumericValue &value) {
    if (_status != ComponentStatus::Ready || !value.isValid()) return false;
    char *end = nullptr;
    const double percent = strtod(value.numericValue.c_str(), &end);
    if (end == value.numericValue.c_str() || *end != '\0' || !std::isfinite(percent) || percent < 0 || percent > 100) return false;
    const uint32_t duty = static_cast<uint32_t>(percent * 255.0 / 100.0 + 0.5);
    analogWrite(_gpio, duty);
    _percent = static_cast<float>(percent); return true;
}
}
