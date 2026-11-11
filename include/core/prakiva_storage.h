#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

namespace prakiva
{
    bool beginStorage();
    bool isStorageReady();
    bool saveConfiguration(JsonVariantConst configuration, uint32_t configurationTimestamp, String &errorCode, String &errorMessage);
    bool loadConfigurationJson(String &configurationJson, uint32_t &configurationTimestamp, String &errorCode, String &errorMessage);
}
