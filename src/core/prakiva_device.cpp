
#include "core/prakiva_device.h"

namespace prakiva
{
    namespace
    {
        constexpr DeviceInfo DEVICE_INFO = {
            "Prakiva",
            "0.1.0",
            "ESP32-S3"};

    } // namespace

    const DeviceInfo &getDeviceInfo()
    {
        return DEVICE_INFO;
    }

} // namespace prakiva
