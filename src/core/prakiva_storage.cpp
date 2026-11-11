#include "core/prakiva_storage.h"

#include "core/prakiva_json.h"

#include <LittleFS.h>
#include <cstdio>

namespace prakiva
{
    namespace
    {
        constexpr char CONFIG_PATH[] = "/configuration.json";
        constexpr char TEMP_PATH[] = "/configuration.tmp";
        constexpr char BACKUP_PATH[] = "/configuration.bak";
        // The app editor document can contain multiple curves and saved profiles.
        // Keep a bounded 48 KB configuration record within the LittleFS partition.
        constexpr size_t MAX_CONFIGURATION_BYTES = 48 * 1024;
        bool storageReady = false;

        bool readJsonFile(const char *path, JsonDocument &document)
        {
            File file = LittleFS.open(path, "r");
            if (!file)
                return false;
            const DeserializationError error = deserializeJson(
                document,
                file,
                DeserializationOption::NestingLimit(JSON_NESTING_LIMIT));
            file.close();
            return !error && document.is<JsonObject>();
        }

        bool isValidConfigurationRecord(JsonDocument &document)
        {
            if (!document.is<JsonObject>())
                return false;

            // New format: configuration and controller timestamp are stored together.
            if (document["configuration"].is<JsonObject>() &&
                document["configuration_timestamp"].is<uint32_t>() &&
                document["configuration_timestamp"].as<uint32_t>() != 0)
                return true;

            // Legacy format: the configuration itself was the root JSON object.
            return document["configuration_timestamp"].isUnbound() &&
                   document["configuration"].isUnbound();
        }

    }

    bool beginStorage()
    {
        if (storageReady)
            return true;

        // The existing partition is named "spiffs" in the supplied 16 MB table.
        // It is reused as a LittleFS data partition; no partition offsets or OTA slots change.
        // Never auto-format on a mount failure: preserve existing configuration data
        // and report the storage error for explicit recovery.
        storageReady = LittleFS.begin(false, "/littlefs", 10, "spiffs");
        if (!storageReady)
        {
            Serial.println("[STORAGE] LittleFS mount failed");
            return false;
        }

        // Recover the last known-good configuration if the active file is missing or corrupt.
        bool activeValid = false;
        if (LittleFS.exists(CONFIG_PATH))
        {
            JsonDocument active;
            activeValid = readJsonFile(CONFIG_PATH, active) && isValidConfigurationRecord(active);
        }
        if (!activeValid && LittleFS.exists(BACKUP_PATH))
        {
            LittleFS.remove(CONFIG_PATH);
            if (LittleFS.rename(BACKUP_PATH, CONFIG_PATH))
                Serial.println("[STORAGE] Recovered configuration from backup");
        }
        else if (!activeValid && LittleFS.exists(CONFIG_PATH))
        {
            Serial.println("[STORAGE] Warning: active configuration is invalid and no backup is available");
        }

        return true;
    }

    bool isStorageReady()
    {
        return storageReady;
    }

    bool saveConfiguration(JsonVariantConst configuration, uint32_t configurationTimestamp, String &errorCode, String &errorMessage)
    {
        if (!storageReady)
        {
            errorCode = "STORAGE_NOT_READY";
            errorMessage = "Persistent storage is not initialized.";
            return false;
        }
        if (!configuration.is<JsonObjectConst>())
        {
            errorCode = "INVALID_CONFIGURATION";
            errorMessage = "configuration must be a JSON object.";
            return false;
        }

        if (configurationTimestamp == 0)
        {
            errorCode = "TIME_INVALID";
            errorMessage = "A valid controller timestamp is required to save configuration.";
            return false;
        }

        // Stream the timestamped record directly to LittleFS. This avoids making
        // a second full JsonDocument and a full serialized String while the HTTP
        // request document is still resident in RAM.
        constexpr char RECORD_PREFIX[] = "{\"configuration_timestamp\":";
        constexpr char CONFIGURATION_PREFIX[] = ",\"configuration\":";
        char timestampText[11] = {};
        const int timestampLength = snprintf(
            timestampText,
            sizeof(timestampText),
            "%lu",
            static_cast<unsigned long>(configurationTimestamp));
        if (timestampLength <= 0 || static_cast<size_t>(timestampLength) >= sizeof(timestampText))
        {
            errorCode = "TIME_INVALID";
            errorMessage = "The controller timestamp could not be serialized.";
            return false;
        }

        const size_t configurationBytes = measureJson(configuration);
        const size_t expectedBytes =
            (sizeof(RECORD_PREFIX) - 1) +
            static_cast<size_t>(timestampLength) +
            (sizeof(CONFIGURATION_PREFIX) - 1) +
            configurationBytes + 1; // closing brace
        if (configurationBytes == 0 || expectedBytes > MAX_CONFIGURATION_BYTES)
        {
            errorCode = "CONFIGURATION_TOO_LARGE";
            errorMessage = "The configuration is empty or exceeds the 48 KB limit.";
            return false;
        }

        LittleFS.remove(TEMP_PATH);
        File temp = LittleFS.open(TEMP_PATH, "w");
        if (!temp)
        {
            errorCode = "STORAGE_WRITE_FAILED";
            errorMessage = "Could not create the temporary configuration file.";
            return false;
        }

        size_t written = 0;
        written += temp.print(RECORD_PREFIX);
        written += temp.print(timestampText);
        written += temp.print(CONFIGURATION_PREFIX);
        written += serializeJson(configuration, temp);
        written += temp.print("}");
        temp.flush();
        temp.close();

        if (written != expectedBytes)
        {
            LittleFS.remove(TEMP_PATH);
            errorCode = "STORAGE_WRITE_FAILED";
            errorMessage = "The complete configuration could not be written.";
            return false;
        }

        // Read back and validate the temporary file before touching the active copy.
        // Keep this full-document verification in its own scope so its allocation is
        // released before the commit phase.
        bool temporaryValid = false;
        {
            JsonDocument verification;
            temporaryValid =
                readJsonFile(TEMP_PATH, verification) &&
                isValidConfigurationRecord(verification) &&
                verification["configuration_timestamp"].as<uint32_t>() == configurationTimestamp;
        }
        if (!temporaryValid)
        {
            LittleFS.remove(TEMP_PATH);
            errorCode = "STORAGE_VERIFY_FAILED";
            errorMessage = "The temporary configuration failed read-back validation.";
            return false;
        }

        const bool hadActive = LittleFS.exists(CONFIG_PATH);
        LittleFS.remove(BACKUP_PATH);
        if (hadActive && !LittleFS.rename(CONFIG_PATH, BACKUP_PATH))
        {
            LittleFS.remove(TEMP_PATH);
            errorCode = "STORAGE_BACKUP_FAILED";
            errorMessage = "The previous configuration could not be preserved.";
            return false;
        }

        if (!LittleFS.rename(TEMP_PATH, CONFIG_PATH))
        {
            if (hadActive && LittleFS.exists(BACKUP_PATH))
                LittleFS.rename(BACKUP_PATH, CONFIG_PATH);
            errorCode = "STORAGE_COMMIT_FAILED";
            errorMessage = "The new configuration could not be activated; the previous copy was restored where possible.";
            return false;
        }

        // The committed file is the exact file already parsed and verified above.
        // Rename does not change its contents, so verify its presence and size rather
        // than allocating another full JSON document for a duplicate parse.
        bool committedValid = false;
        File committedFile = LittleFS.open(CONFIG_PATH, "r");
        if (committedFile)
        {
            committedValid = committedFile.size() == expectedBytes;
            committedFile.close();
        }
        if (!committedValid)
        {
            LittleFS.remove(CONFIG_PATH);
            if (hadActive && LittleFS.exists(BACKUP_PATH))
                LittleFS.rename(BACKUP_PATH, CONFIG_PATH);
            errorCode = "STORAGE_VERIFY_FAILED";
            errorMessage = "The committed configuration failed size verification.";
            return false;
        }

        errorCode = "";
        errorMessage = "";
        Serial.printf(
            "[CONFIGURATION] Saved %u bytes; timestamp=%lu\n",
            static_cast<unsigned>(expectedBytes),
            static_cast<unsigned long>(configurationTimestamp));
        return true;
    }

    bool loadConfigurationJson(String &configurationJson, uint32_t &configurationTimestamp, String &errorCode, String &errorMessage)
    {
        if (!storageReady)
        {
            errorCode = "STORAGE_NOT_READY";
            errorMessage = "Persistent storage is not initialized.";
            return false;
        }
        if (!LittleFS.exists(CONFIG_PATH))
        {
            errorCode = "CONFIGURATION_NOT_FOUND";
            errorMessage = "No configuration has been saved yet.";
            return false;
        }

        // Parse the stored record once, then serialize only the configuration
        // value. Avoid copying the deeply nested editor document into another
        // JsonDocument; that copy adds recursive traversal and stack usage.
        JsonDocument record;
        if (!readJsonFile(CONFIG_PATH, record))
        {
            errorCode = "CONFIGURATION_READ_FAILED";
            errorMessage = "The saved configuration is not valid JSON.";
            return false;
        }

        JsonObjectConst storedConfiguration;
        if (record["configuration"].is<JsonObjectConst>() &&
            record["configuration_timestamp"].is<uint32_t>())
        {
            storedConfiguration = record["configuration"].as<JsonObjectConst>();
            configurationTimestamp = record["configuration_timestamp"].as<uint32_t>();
        }
        else
        {
            // Backward compatibility: older firmware stored the configuration
            // itself as the root object, without a trustworthy timestamp.
            storedConfiguration = record.as<JsonObjectConst>();
            configurationTimestamp = 0;
            Serial.println("[STORAGE] Legacy configuration format detected; timestamp unavailable until next save");
        }

        const size_t jsonSize = measureJson(storedConfiguration);
        if (jsonSize == 0)
        {
            errorCode = "CONFIGURATION_READ_FAILED";
            errorMessage = "The saved configuration is empty or invalid.";
            return false;
        }

        configurationJson = "";
        if (!configurationJson.reserve(jsonSize + 1) ||
            serializeJson(storedConfiguration, configurationJson) != jsonSize)
        {
            configurationJson = "";
            errorCode = "CONFIGURATION_READ_FAILED";
            errorMessage = "The saved configuration could not be serialized.";
            return false;
        }

        errorCode = "";
        errorMessage = "";
        return true;
    }

}
