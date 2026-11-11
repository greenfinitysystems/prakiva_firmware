#pragma once
#include "components/core/prakiva_actuator_driver.h"
namespace prakiva {
class PwmActuatorDriver final : public IActuatorDriver {
public:
    // Must be called by product-resource binding before begin().
    bool configurePin(int gpio);
    bool begin() override;
    ComponentStatus status() const override { return _status; }
    bool get(NumericValue &value) override;
    bool set(const NumericValue &value) override;
private:
    int _gpio = -1;
    float _percent = 0;
    ComponentStatus _status = ComponentStatus::NotInitialized;
};
}
