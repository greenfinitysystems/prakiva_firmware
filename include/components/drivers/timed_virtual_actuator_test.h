#pragma once
#include "components/core/prakiva_virtual_actuator_driver.h"
#include <Arduino.h>

namespace prakiva {
// Temporary standalone test harness, not a product capability or GPIO driver.
class TimedVirtualActuatorTest final : public IVirtualActuatorDriver {
public:
    bool begin() override;
    ComponentStatus status() const override { return _status; }
    bool get(NumericValue &value) override;
    bool set(const NumericValue &value) override;
    void update() override;
    bool bindActuator(const char *, IActuatorDriver *) override { return false; }
private:
    ComponentStatus _status = ComponentStatus::NotInitialized;
    bool _running = false;
    uint32_t _startedAt = 0;
    static constexpr uint32_t DURATION_MS = 10000;
};
}
