
#include "prakiva_device.h"

namespace prakiva
{
    namespace
    {
        constexpr DeviceInfo DEVICE_INFO = {
            "Prakiva",
            "0.1.0",
            "ESP32",
            "0.1"};

    } // namespace

    const DeviceInfo &getDeviceInfo()
    {
        return DEVICE_INFO;
    }

} // namespace prakiva
