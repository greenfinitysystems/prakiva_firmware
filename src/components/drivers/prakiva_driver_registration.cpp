#include "components/drivers/prakiva_driver_registration.h"
#include "components/drivers/dummy_sensor_driver.h"
#include "components/drivers/null_actuator_driver.h"
#include "components/drivers/pwm_actuator_driver.h"
#include "components/drivers/binary_actuator_driver.h"
#include "components/drivers/timed_virtual_actuator_test.h"

namespace prakiva
{
namespace
{
IComponent *makeDummySensorDriver() { return new DummySensorDriver(); }
IComponent *makeNullActuatorDriver() { return new NullActuatorDriver(); }
IComponent *makePwmActuatorDriver() { return new PwmActuatorDriver(); }
IComponent *makeBinaryActuatorDriver() { return new BinaryActuatorDriver(); }
IComponent *makeTimedVirtualActuatorTest() { return new TimedVirtualActuatorTest(); }
} // namespace

bool registerPlatformDrivers(ComponentRegistry &registry)
{
    if (!registry.registerFactory("DummySensorDriver", makeDummySensorDriver)) return false;
    if (!registry.registerFactory("NullActuatorDriver", makeNullActuatorDriver)) return false;
    if (!registry.registerFactory("PwmActuatorDriver", makePwmActuatorDriver)) return false;
    if (!registry.registerFactory("BinaryActuatorDriver", makeBinaryActuatorDriver)) return false;
    if (!registry.registerFactory("TimedVirtualActuatorTest", makeTimedVirtualActuatorTest)) return false;
    return true;
}
} // namespace prakiva
