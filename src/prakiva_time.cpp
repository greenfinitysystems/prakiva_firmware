#include "prakiva_time.h"

#include <Wire.h>
#include <RTClib.h>
#include <Preferences.h>
#include <WiFi.h>
#include <sys/time.h>
#include <time.h>
#include <esp_sntp.h>

namespace prakiva
{
    namespace
    {
        constexpr uint8_t I2C_SDA_PIN = 21;
        constexpr uint8_t I2C_SCL_PIN = 22;
        constexpr uint32_t MIN_VALID_UNIX_TIME = 1704067200UL; // 2024-01-01 UTC
        constexpr uint32_t NTP_TIMEOUT_MS = 15000;
        constexpr int16_t MIN_SCHEDULE_OFFSET_MINUTES = -720;
        constexpr int16_t MAX_SCHEDULE_OFFSET_MINUTES = 720;

        constexpr char TIME_NAMESPACE[] = "prktime";
        constexpr char OFFSET_KEY[] = "sched_offset";
        constexpr char SOURCE_KEY[] = "sync_source";
        constexpr char LAST_SYNC_KEY[] = "last_sync";

        RTC_DS3231 rtc;
        bool timeModuleReady = false;
        bool rtcAvailable = false;
        bool clockValid = false;
        int16_t scheduleOffsetMinutes = 0;
        String syncSource = "none";
        uint32_t lastSuccessfulSync = 0;
        volatile bool ntpSyncObserved = false;

        void onNtpTimeSync(struct timeval *tv)
        {
            (void)tv;
            ntpSyncObserved = true;
        }

        uint32_t currentUnixTime()
        {
            const time_t now = time(nullptr);
            return now > 0 ? static_cast<uint32_t>(now) : 0;
        }

        bool setSystemClock(uint32_t unixTime)
        {
            if (unixTime < MIN_VALID_UNIX_TIME)
                return false;

            timeval tv{};
            tv.tv_sec = static_cast<time_t>(unixTime);
            tv.tv_usec = 0;
            if (settimeofday(&tv, nullptr) != 0)
                return false;

            clockValid = true;
            return true;
        }

        bool persistSyncMetadata(const char *source, uint32_t syncedAt)
        {
            Preferences p;
            if (!p.begin(TIME_NAMESPACE, false))
                return false;

            const size_t sourceBytes = p.putString(SOURCE_KEY, source);
            const size_t timeBytes = p.putUInt(LAST_SYNC_KEY, syncedAt);
            const bool verified = p.getString(SOURCE_KEY, "") == source &&
                                  p.getUInt(LAST_SYNC_KEY, 0) == syncedAt;
            p.end();
            if (sourceBytes == 0 || timeBytes == 0 || !verified)
                return false;

            syncSource = source;
            lastSuccessfulSync = syncedAt;
            return true;
        }

        bool writeRtc(uint32_t unixTime)
        {
            if (!rtcAvailable)
                return false;
            rtc.adjust(DateTime(unixTime));
            return !rtc.lostPower();
        }

        bool isWifiStaConnected()
        {
            return WiFi.getMode() == WIFI_STA || WiFi.getMode() == WIFI_AP_STA
                       ? WiFi.status() == WL_CONNECTED
                       : false;
        }

        void loadTimeSettings()
        {
            Preferences p;
            if (!p.begin(TIME_NAMESPACE, true))
                return;

            const int value = p.getShort(OFFSET_KEY, 0);
            if (value >= MIN_SCHEDULE_OFFSET_MINUTES && value <= MAX_SCHEDULE_OFFSET_MINUTES)
                scheduleOffsetMinutes = static_cast<int16_t>(value);

            syncSource = p.getString(SOURCE_KEY, "none");
            lastSuccessfulSync = p.getUInt(LAST_SYNC_KEY, 0);
            p.end();
        }
    }

    bool beginTime()
    {
        if (timeModuleReady)
            return true;

        Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
        rtcAvailable = rtc.begin(&Wire);
        if (rtcAvailable)
        {
            if (!rtc.lostPower())
            {
                const DateTime rtcNow = rtc.now();
                const uint32_t rtcUnix = rtcNow.unixtime();
                if (rtcUnix >= MIN_VALID_UNIX_TIME && setSystemClock(rtcUnix))
                {
                    Serial.printf("[TIME] System clock restored from DS3231: %lu\n",
                                  static_cast<unsigned long>(rtcUnix));
                }
                else
                {
                    Serial.println("[TIME] DS3231 time is not valid; phone or NTP sync is required");
                }
            }
            else
            {
                Serial.println("[TIME] DS3231 reports lost power; phone or NTP sync is required");
            }
        }
        else
        {
            Serial.println("[TIME] WARNING: DS3231 not detected; time will not survive power loss");
        }

        // Preferences are separate from Wi-Fi and owner configuration namespaces.
        Preferences p;
        if (!p.begin(TIME_NAMESPACE, false))
        {
            Serial.println("[TIME] ERROR: Cannot open time settings in NVS");
            return false;
        }
        if (!p.isKey(OFFSET_KEY))
            p.putShort(OFFSET_KEY, 0);
        p.end();

        loadTimeSettings();
        timeModuleReady = true;
        clockValid = clockValid || currentUnixTime() >= MIN_VALID_UNIX_TIME;
        Serial.printf("[TIME] Module ready; RTC=%s, clock=%s, schedule offset=%d minutes\n",
                      rtcAvailable ? "available" : "unavailable",
                      clockValid ? "valid" : "invalid",
                      scheduleOffsetMinutes);
        return true;
    }

    bool isTimeModuleReady() { return timeModuleReady; }
    bool isRtcAvailable() { return rtcAvailable; }
    bool isClockValid() { return clockValid && currentUnixTime() >= MIN_VALID_UNIX_TIME; }

    bool syncTimeFromPhone(uint32_t unixTime, String &errorCode, String &errorMessage)
    {
        if (!timeModuleReady)
        {
            errorCode = "TIME_NOT_READY";
            errorMessage = "The time module is not initialized.";
            return false;
        }
        if (unixTime < MIN_VALID_UNIX_TIME)
        {
            errorCode = "INVALID_TIMESTAMP";
            errorMessage = "unix_time must be a valid Unix timestamp from 2024 onward.";
            return false;
        }
        if (!setSystemClock(unixTime))
        {
            errorCode = "CLOCK_SET_FAILED";
            errorMessage = "The ESP32 system clock could not be set.";
            return false;
        }

        const bool rtcWritten = writeRtc(unixTime);
        if (!persistSyncMetadata("phone", unixTime))
        {
            errorCode = "TIME_STORAGE_FAILURE";
            errorMessage = "The clock was set, but sync metadata could not be saved.";
            return false;
        }

        if (!rtcWritten && rtcAvailable)
            Serial.println("[TIME] WARNING: System clock set, but DS3231 update did not verify");
        errorCode = "";
        errorMessage = "";
        Serial.printf("[TIME] Synchronized from phone: %lu; RTC=%s\n",
                      static_cast<unsigned long>(unixTime), rtcWritten ? "updated" : "unavailable or failed");
        return true;
    }

    bool syncTimeFromNtp(String &errorCode, String &errorMessage)
    {
        if (!timeModuleReady)
        {
            errorCode = "TIME_NOT_READY";
            errorMessage = "The time module is not initialized.";
            return false;
        }
        if (!isWifiStaConnected())
        {
            errorCode = "NTP_REQUIRES_STA";
            errorMessage = "NTP synchronization requires a connected Wi-Fi station network.";
            return false;
        }

        // Do not mistake a valid RTC-restored clock for a successful NTP response.
        ntpSyncObserved = false;
        sntp_set_time_sync_notification_cb(onNtpTimeSync);
        configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
        const uint32_t started = millis();
        uint32_t ntpTime = 0;
        while (static_cast<uint32_t>(millis() - started) < NTP_TIMEOUT_MS)
        {
            if (ntpSyncObserved)
            {
                ntpTime = currentUnixTime();
                if (ntpTime >= MIN_VALID_UNIX_TIME)
                    break;
            }
            delay(100);
        }

        if (!ntpSyncObserved || ntpTime < MIN_VALID_UNIX_TIME)
        {
            errorCode = "NTP_SYNC_FAILED";
            errorMessage = "No valid NTP time was received before the timeout.";
            return false;
        }

        if (!setSystemClock(ntpTime))
        {
            errorCode = "CLOCK_SET_FAILED";
            errorMessage = "The ESP32 system clock could not be updated.";
            return false;
        }
        const bool rtcWritten = writeRtc(ntpTime);
        if (!persistSyncMetadata("ntp", ntpTime))
        {
            errorCode = "TIME_STORAGE_FAILURE";
            errorMessage = "The clock was synchronized, but sync metadata could not be saved.";
            return false;
        }

        if (!rtcWritten && rtcAvailable)
            Serial.println("[TIME] WARNING: NTP succeeded, but DS3231 update did not verify");
        errorCode = "";
        errorMessage = "";
        Serial.printf("[TIME] Synchronized from NTP: %lu; RTC=%s\n",
                      static_cast<unsigned long>(ntpTime), rtcWritten ? "updated" : "unavailable or failed");
        return true;
    }

    bool setScheduleOffsetMinutes(int16_t offsetMinutes, String &errorCode, String &errorMessage)
    {
        if (!timeModuleReady)
        {
            errorCode = "TIME_NOT_READY";
            errorMessage = "The time module is not initialized.";
            return false;
        }
        if (offsetMinutes < MIN_SCHEDULE_OFFSET_MINUTES || offsetMinutes > MAX_SCHEDULE_OFFSET_MINUTES)
        {
            errorCode = "INVALID_SCHEDULE_OFFSET";
            errorMessage = "offset_minutes must be between -720 and 720.";
            return false;
        }

        Preferences p;
        if (!p.begin(TIME_NAMESPACE, false))
        {
            errorCode = "TIME_STORAGE_FAILURE";
            errorMessage = "Time settings storage could not be opened.";
            return false;
        }
        const size_t written = p.putShort(OFFSET_KEY, offsetMinutes);
        const int verified = p.getShort(OFFSET_KEY, 32767);
        p.end();
        if (written == 0 || verified != offsetMinutes)
        {
            errorCode = "TIME_STORAGE_FAILURE";
            errorMessage = "The schedule offset could not be saved and verified.";
            return false;
        }

        scheduleOffsetMinutes = offsetMinutes;
        errorCode = "";
        errorMessage = "";
        Serial.printf("[TIME] Aquarium schedule offset set to %d minutes\n", scheduleOffsetMinutes);
        return true;
    }

    void addTimeStatusJson(JsonObject &data)
    {
        const uint32_t now = currentUnixTime();
        const bool valid = isClockValid();
        data["clock_valid"] = valid;
        data["unix_time"] = valid ? now : 0;
        data["rtc_available"] = rtcAvailable;
        data["sync_source"] = syncSource;
        data["last_sync_unix"] = lastSuccessfulSync;
        data["schedule_offset_minutes"] = scheduleOffsetMinutes;
        data["schedule_unix_time"] = valid
                                         ? static_cast<uint32_t>(static_cast<int64_t>(now) + static_cast<int64_t>(scheduleOffsetMinutes) * 60)
                                         : 0;
        data["wifi_sta_connected"] = isWifiStaConnected();
    }
}
