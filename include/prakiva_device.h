
#pragma once

namespace prakiva
{

    struct DeviceInfo
    {
        const char *productName;
        const char *firmwareVersion;
        const char *hardwareFamily;
        const char *protocolVersion;
    };

    const DeviceInfo &getDeviceInfo();

} // namespace prakiva
