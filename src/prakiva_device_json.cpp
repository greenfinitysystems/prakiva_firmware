
#include "prakiva_device_json.h"
#include "prakiva_identity.h"

#include <ArduinoJson.h>

namespace prakiva
{

    String serializeDeviceInfoJson(const DeviceInfo &device)
    {
        JsonDocument document;

        document["protocol_version"] = device.protocolVersion;

        JsonObject deviceObject = document["device"].to<JsonObject>();
        deviceObject["device_id"] = getDeviceId();
        deviceObject["product"] = device.productName;
        deviceObject["firmware_version"] = device.firmwareVersion;
        deviceObject["hardware_family"] = device.hardwareFamily;

        String output;
        serializeJson(document, output);

        return output;
    }

} // namespace prakiva
