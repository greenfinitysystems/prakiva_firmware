#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

namespace prakiva
{
    // Initializes I2C/RTC and restores the ESP32 system clock when RTC time is valid.
    // Returns false only if the time module's persistent settings cannot be initialized.
    bool beginTime();

    bool isTimeModuleReady();
    bool isRtcAvailable();
    bool isClockValid();

    // All timestamps are Unix UTC seconds. The schedule offset never changes the system clock.
    bool syncTimeFromPhone(uint32_t unixTime, String &errorCode, String &errorMessage);
    bool syncTimeFromNtp(String &errorCode, String &errorMessage);
    bool setScheduleOffsetMinutes(int16_t offsetMinutes, String &errorCode, String &errorMessage);

    // Returns JSON fields used by the protocol response's data object.
    void addTimeStatusJson(JsonObject &data);
}
