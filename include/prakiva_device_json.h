
#pragma once

#include <Arduino.h>
#include "prakiva_device.h"

namespace prakiva
{

    String serializeDeviceInfoJson(const DeviceInfo &device);

} // namespace prakiva
