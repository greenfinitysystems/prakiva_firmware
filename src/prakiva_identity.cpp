
#include "prakiva_identity.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>
#include <cstring>

namespace prakiva
{
    namespace
    {

        constexpr char NVS_NAMESPACE[] = "prakiva";
        constexpr char DEVICE_ID_KEY[] = "device_id";

        constexpr size_t DEVICE_ID_BYTES = 16;
        constexpr size_t DEVICE_ID_HEX_LENGTH = DEVICE_ID_BYTES * 2;

        char deviceId[DEVICE_ID_HEX_LENGTH + 1] = {};
        bool identityReady = false;

        bool isValidDeviceId(const String &value)
        {
            if (value.length() != DEVICE_ID_HEX_LENGTH)
            {
                return false;
            }

            for (size_t i = 0; i < DEVICE_ID_HEX_LENGTH; ++i)
            {
                const char c = value[i];
                const bool isHex =
                    (c >= '0' && c <= '9') ||
                    (c >= 'a' && c <= 'f');

                if (!isHex)
                {
                    return false;
                }
            }

            return true;
        }

        void encodeHex(const uint8_t *bytes, char *output)
        {
            constexpr char HEX_DIGITS[] = "0123456789abcdef";

            for (size_t i = 0; i < DEVICE_ID_BYTES; ++i)
            {
                output[i * 2] = HEX_DIGITS[bytes[i] >> 4];
                output[i * 2 + 1] = HEX_DIGITS[bytes[i] & 0x0F];
            }

            output[DEVICE_ID_HEX_LENGTH] = '\0';
        }

        bool generateAndStoreId(Preferences &preferences)
        {
            uint8_t randomBytes[DEVICE_ID_BYTES];
            esp_fill_random(randomBytes, sizeof(randomBytes));
            encodeHex(randomBytes, deviceId);

            const size_t written =
                preferences.putString(DEVICE_ID_KEY, deviceId);

            const String storedId = preferences.getString(DEVICE_ID_KEY, "");

            if (written == 0 || storedId != deviceId)
            {
                memset(deviceId, 0, sizeof(deviceId));
                Serial.println("[IDENTITY] ERROR: Storage failed");
                return false;
            }

            return true;
        }

    } // namespace

    bool beginIdentity()
    {
        if (identityReady)
        {
            return true;
        }

        Preferences preferences;

        if (!preferences.begin(NVS_NAMESPACE, false))
        {
            Serial.println("[IDENTITY] ERROR: Cannot open NVS");
            return false;
        }

        if (!preferences.isKey(DEVICE_ID_KEY))
        {
            if (!generateAndStoreId(preferences))
            {
                preferences.end();
                return false;
            }
        }

        const String storedId = preferences.getString(DEVICE_ID_KEY, "");
        preferences.end();

        if (!isValidDeviceId(storedId))
        {
            Serial.println("[IDENTITY] ERROR: Device ID is corrupt");
            return false;
        }

        memcpy(deviceId, storedId.c_str(), DEVICE_ID_HEX_LENGTH + 1);

        identityReady = true;
        Serial.printf("[IDENTITY] Device ID: %s\n", deviceId);

        return true;
    }

    bool isIdentityReady()
    {
        return identityReady;
    }

    const char *getDeviceId()
    {
        return identityReady ? deviceId : nullptr;
    }

} // namespace prakiva
