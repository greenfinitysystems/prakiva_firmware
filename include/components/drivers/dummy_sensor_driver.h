#pragma once
#include "components/core/prakiva_virtual_sensor_driver.h"

namespace prakiva {
// Development utility: zero-input virtual sensor. Values are simulated.
class DummySensorDriver final : public IVirtualSensorDriver {
public:
    bool begin() override;
    ComponentStatus status() const override { return _status; }
    bool get(SensorValue &reading) override;
    bool bindInput(const char *, ISensorDriver *) override { return false; }
    void setMeasurementUnit(const String &unit) override { _unit = unit.length() ? unit : "n/a"; }
    bool validateParameters(JsonObjectConst parameters) const override;
    bool applyParameters(JsonObjectConst parameters) override;
private:
    ComponentStatus _status = ComponentStatus::NotInitialized;
    bool _random = false;
    float _value = 0.0f, _min = 0.0f, _max = 1.0f;
    String _unit = "n/a";
};
}
