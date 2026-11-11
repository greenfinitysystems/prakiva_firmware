#include "components/drivers/timed_virtual_actuator_test.h"

namespace prakiva {
bool TimedVirtualActuatorTest::begin() {
    _running = false;
    _status = ComponentStatus::Ready;
    Serial.println("[VIRTUAL_TEST] READY / IDLE");
    return true;
}
bool TimedVirtualActuatorTest::get(NumericValue &value) {
    if (_status != ComponentStatus::Ready) return false;
    value.valueType = ValueType::UInt32;
    value.numericValue = _running ? "1" : "0";
    return true;
}
bool TimedVirtualActuatorTest::set(const NumericValue &value) {
    if (_status != ComponentStatus::Ready || !value.isValid()) return false;
    if (value.numericValue != "1" && value.numericValue != "0") return false;
    if (value.numericValue == "0") {
        // For this test, OFF does not cancel an operation in progress.
        if (_running) Serial.println("[VIRTUAL_TEST] OFF ignored while running");
        return true;
    }
    if (_running) {
        Serial.println("[VIRTUAL_TEST] ON ignored (already running)");
        return true;
    }
    _running = true;
    _startedAt = millis();
    Serial.println("[VIRTUAL_TEST] START / RUNNING (10 seconds)");
    return true;
}
void TimedVirtualActuatorTest::update() {
    if (_running && static_cast<uint32_t>(millis() - _startedAt) >= DURATION_MS) {
        _running = false;
        Serial.println("[VIRTUAL_TEST] COMPLETE / IDLE");
    }
}
}
