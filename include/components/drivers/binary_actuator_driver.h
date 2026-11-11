#pragma once
#include "components/core/prakiva_actuator_driver.h"
namespace prakiva {
class BinaryActuatorDriver final : public IActuatorDriver {
public:
    bool configurePin(int gpio);
    bool begin() override;
    ComponentStatus status() const override { return _status; }
    bool get(NumericValue &value) override;
    bool set(const NumericValue &value) override;
private:
    int _gpio = -1;
    bool _on = false;
    ComponentStatus _status = ComponentStatus::NotInitialized;
};
}
