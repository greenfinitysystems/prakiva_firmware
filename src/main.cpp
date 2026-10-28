#include <Arduino.h>

#include "prakiva_config.h"
#include "prakiva_identity.h"
#include "prakiva_protocol.h"
#include "prakiva_wifi.h"
#include "prakiva_time.h"

namespace
{

    constexpr unsigned long SERIAL_BAUD_RATE = 115200;

    void runProtocolSmokeTest(const char *label, const String &request)
    {
        Serial.printf("\n--- %s ---\n", label);
        Serial.println(prakiva::handleProtocolRequest(request));
    }

} // namespace

void setup()
{
    Serial.begin(SERIAL_BAUD_RATE);
    delay(500);

    Serial.println();
    Serial.println("Prakiva controller");
    Serial.println("==================");

    if (!prakiva::beginIdentity())
    {
        Serial.println("[IDENTITY] Initialization FAILED");
        return;
    }

    if (!prakiva::beginConfig())
    {
        Serial.println("[CONFIG] Initialization FAILED");
        return;
    }

    if (!prakiva::beginTime())
    {
        Serial.println("[TIME] Initialization FAILED; continuing without time settings");
    }

    runProtocolSmokeTest(
        "Get device information",
        R"({"request_id":"req-001","operation":"get_device_info","protocol_version":"0.1"})");

    runProtocolSmokeTest(
        "Get setup status",
        R"({"request_id":"req-002","operation":"get_setup_status","protocol_version":"0.1"})");

    if (!prakiva::beginWifiApi())
    {
        Serial.println("[WIFI] Initialization FAILED");
        return;
    }
}

void loop()
{
    prakiva::handleWifiApi();
    delay(2);
}
