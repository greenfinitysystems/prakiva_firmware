
#pragma once

#include <Arduino.h>
#include "core/prakiva_device.h"

namespace prakiva
{

    String serializeDeviceInfoJson(const DeviceInfo &device);

} // namespace prakiva
