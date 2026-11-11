#pragma once
#include "components/core/prakiva_actuator_driver.h"

namespace prakiva {
// Development utility: remembers commands without operating hardware.
class NullActuatorDriver final : public IActuatorDriver {
public:
    bool begin() override;
    ComponentStatus status() const override { return _status; }
    bool get(NumericValue &value) override;
    bool set(const NumericValue &value) override;
private:
    ComponentStatus _status = ComponentStatus::NotInitialized;
    NumericValue _last;
};
}
