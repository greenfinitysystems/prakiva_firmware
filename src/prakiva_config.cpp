#include "prakiva_config.h"

#include <Preferences.h>
#include <esp_system.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

namespace prakiva
{
    namespace
    {
        constexpr char NVS_NAMESPACE[] = "prakiva";
        constexpr char SETUP_STATE_KEY[] = "setup_state";
        constexpr char OWNER_ID_KEY[] = "owner_id";
        constexpr char OWNER_SALT_KEY[] = "owner_salt";
        constexpr char OWNER_HASH_KEY[] = "owner_hash";
        constexpr char RESET_PIN_SALT_KEY[] = "reset_pin_salt";
        constexpr char RESET_PIN_HASH_KEY[] = "reset_pin_hash";

// DEVELOPMENT ONLY: disable this compile-time flag before production.
#ifndef PRAKIVA_ENABLE_TEST_MASTER_PIN
#define PRAKIVA_ENABLE_TEST_MASTER_PIN 1
#endif
#if PRAKIVA_ENABLE_TEST_MASTER_PIN
        constexpr char TEST_MASTER_PIN[] = "0123456789";
#endif

        constexpr size_t SALT_BYTES = 16;
        constexpr size_t HASH_BYTES = 32;
        constexpr uint32_t PBKDF2_ITERATIONS = 100000;

        bool configReady = false;
        SetupState setupState = SetupState::Unclaimed;

        // Deliberately RAM-only. A reboot invalidates every session automatically.
        constexpr size_t SESSION_TOKEN_BYTES = 32;
        String activeSessionToken;
        bool activeSession = false;

        bool constantTimeTokenEquals(const String &candidate, const String &expected)
        {
            if (candidate.length() != expected.length())
                return false;
            uint8_t difference = 0;
            for (size_t i = 0; i < candidate.length(); ++i)
                difference |= static_cast<uint8_t>(candidate[i] ^ expected[i]);
            return difference == 0;
        }

        bool isValidSetupState(uint8_t value)
        {
            return value == static_cast<uint8_t>(SetupState::Unclaimed) ||
                   value == static_cast<uint8_t>(SetupState::Claimed);
        }

        bool isValidUserId(const String &userId)
        {
            if (userId.length() < 3 || userId.length() > 32)
                return false;
            for (size_t i = 0; i < userId.length(); ++i)
            {
                const char c = userId[i];
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
                    return false;
            }
            return true;
        }

        bool fromHex(const String &hex, uint8_t *bytes, size_t length)
        {
            if (hex.length() != length * 2)
                return false;

            auto nibble = [](char c) -> int
            {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                if (c >= 'A' && c <= 'F')
                    return c - 'A' + 10;
                return -1;
            };

            for (size_t i = 0; i < length; ++i)
            {
                const int high = nibble(hex[i * 2]);
                const int low = nibble(hex[i * 2 + 1]);
                if (high < 0 || low < 0)
                    return false;
                bytes[i] = static_cast<uint8_t>((high << 4) | low);
            }
            return true;
        }

        bool isValidResetPin(const String &pin)
        {
            if (pin.length() != 10)
                return false;
            for (size_t i = 0; i < pin.length(); ++i)
                if (pin[i] < '0' || pin[i] > '9')
                    return false;
            return true;
        }

        String toHex(const uint8_t *bytes, size_t length)
        {
            static constexpr char HEX_DIGITS[] = "0123456789abcdef";
            String output;
            output.reserve(length * 2);
            for (size_t i = 0; i < length; ++i)
            {
                output += HEX_DIGITS[(bytes[i] >> 4) & 0x0F];
                output += HEX_DIGITS[bytes[i] & 0x0F];
            }
            return output;
        }

        bool derivePasswordHash(const String &password, const uint8_t *salt, uint8_t *hash)
        {
            const mbedtls_md_info_t *mdInfo = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
            if (mdInfo == nullptr)
                return false;

            mbedtls_md_context_t context;
            mbedtls_md_init(&context);
            int result = mbedtls_md_setup(&context, mdInfo, 1);
            if (result == 0)
            {
                result = mbedtls_pkcs5_pbkdf2_hmac_ext(
                    MBEDTLS_MD_SHA256,
                    reinterpret_cast<const unsigned char *>(password.c_str()),
                    password.length(), salt, SALT_BYTES,
                    PBKDF2_ITERATIONS, HASH_BYTES, hash);
            }
            mbedtls_md_free(&context);
            return result == 0;
        }
    }

    bool beginConfig()
    {
        if (configReady)
            return true;

        Preferences preferences;
        if (!preferences.begin(NVS_NAMESPACE, false))
        {
            Serial.println("[CONFIG] ERROR: Cannot open NVS");
            return false;
        }

        if (!preferences.isKey(SETUP_STATE_KEY))
        {
            const size_t written = preferences.putUChar(SETUP_STATE_KEY, static_cast<uint8_t>(SetupState::Unclaimed));
            if (written == 0)
            {
                preferences.end();
                Serial.println("[CONFIG] ERROR: Cannot persist initial setup state");
                return false;
            }
        }

        const uint8_t storedValue = preferences.getUChar(SETUP_STATE_KEY, static_cast<uint8_t>(SetupState::Unclaimed));
        preferences.end();
        if (!isValidSetupState(storedValue))
        {
            Serial.println("[CONFIG] ERROR: Stored setup state is invalid");
            return false;
        }

        setupState = static_cast<SetupState>(storedValue);
        configReady = true;
        Serial.printf("[CONFIG] Setup state: %s\n", getSetupStateName());
        return true;
    }

    bool isConfigReady() { return configReady; }
    SetupState getSetupState() { return setupState; }

    const char *getSetupStateName()
    {
        switch (setupState)
        {
        case SetupState::Unclaimed:
            return "unclaimed";
        case SetupState::Claimed:
            return "claimed";
        default:
            return "invalid";
        }
    }

    ClaimResult claimOwnerAccount(const String &userId, const String &password, const String &resetPin)
    {
        if (!configReady)
            return ClaimResult::NotReady;
        if (setupState == SetupState::Claimed)
            return ClaimResult::AlreadyClaimed;
        if (!isValidUserId(userId))
            return ClaimResult::InvalidUserId;
        if (password.length() < 8 || password.length() > 64)
            return ClaimResult::InvalidPassword;
        if (!isValidResetPin(resetPin))
            return ClaimResult::InvalidResetPin;

        uint8_t salt[SALT_BYTES];
        uint8_t hash[HASH_BYTES];
        uint8_t resetSalt[SALT_BYTES];
        uint8_t resetHash[HASH_BYTES];
        esp_fill_random(salt, sizeof(salt));
        esp_fill_random(resetSalt, sizeof(resetSalt));
        if (!derivePasswordHash(password, salt, hash) ||
            !derivePasswordHash(resetPin, resetSalt, resetHash))
            return ClaimResult::CryptoFailure;

        const String saltHex = toHex(salt, sizeof(salt));
        const String hashHex = toHex(hash, sizeof(hash));
        const String resetSaltHex = toHex(resetSalt, sizeof(resetSalt));
        const String resetHashHex = toHex(resetHash, sizeof(resetHash));
        Preferences preferences;
        if (!preferences.begin(NVS_NAMESPACE, false))
            return ClaimResult::StorageFailure;

        // Store and verify credentials before marking the controller claimed.
        const size_t ownerWritten = preferences.putString(OWNER_ID_KEY, userId);
        const size_t saltWritten = preferences.putString(OWNER_SALT_KEY, saltHex);
        const size_t hashWritten = preferences.putString(OWNER_HASH_KEY, hashHex);
        const size_t resetSaltWritten = preferences.putString(RESET_PIN_SALT_KEY, resetSaltHex);
        const size_t resetHashWritten = preferences.putString(RESET_PIN_HASH_KEY, resetHashHex);
        const bool verified = ownerWritten == userId.length() &&
                              saltWritten == saltHex.length() && hashWritten == hashHex.length() &&
                              resetSaltWritten == resetSaltHex.length() &&
                              resetHashWritten == resetHashHex.length() &&
                              preferences.getString(OWNER_ID_KEY, "") == userId &&
                              preferences.getString(OWNER_SALT_KEY, "") == saltHex &&
                              preferences.getString(OWNER_HASH_KEY, "") == hashHex &&
                              preferences.getString(RESET_PIN_SALT_KEY, "") == resetSaltHex &&
                              preferences.getString(RESET_PIN_HASH_KEY, "") == resetHashHex;

        if (!verified)
        {
            preferences.end();
            return ClaimResult::StorageFailure;
        }

        const size_t stateWritten = preferences.putUChar(SETUP_STATE_KEY, static_cast<uint8_t>(SetupState::Claimed));
        const uint8_t verifiedState = preferences.getUChar(SETUP_STATE_KEY, 255);
        preferences.end();
        if (stateWritten == 0 || verifiedState != static_cast<uint8_t>(SetupState::Claimed))
            return ClaimResult::StorageFailure;

        setupState = SetupState::Claimed;
        return ClaimResult::Success;
    }

    AuthenticationResult authenticateOwner(const String &userId, const String &password)
    {
        if (!configReady)
            return AuthenticationResult::NotReady;
        if (setupState != SetupState::Claimed)
            return AuthenticationResult::NotClaimed;
        if (userId.length() < 3 || userId.length() > 32 ||
            password.length() < 8 || password.length() > 64)
            return AuthenticationResult::InvalidCredentials;

        Preferences preferences;
        if (!preferences.begin(NVS_NAMESPACE, true))
            return AuthenticationResult::StorageFailure;

        const String storedUserId = preferences.getString(OWNER_ID_KEY, "");
        const String saltHex = preferences.getString(OWNER_SALT_KEY, "");
        const String hashHex = preferences.getString(OWNER_HASH_KEY, "");
        preferences.end();

        uint8_t salt[SALT_BYTES];
        uint8_t expectedHash[HASH_BYTES];
        uint8_t actualHash[HASH_BYTES];
        if (storedUserId.isEmpty() || !fromHex(saltHex, salt, sizeof(salt)) ||
            !fromHex(hashHex, expectedHash, sizeof(expectedHash)))
            return AuthenticationResult::StorageFailure;

        if (!derivePasswordHash(password, salt, actualHash))
            return AuthenticationResult::CryptoFailure;

        // Compare all bytes rather than returning at the first mismatch.
        uint8_t difference = 0;
        for (size_t i = 0; i < HASH_BYTES; ++i)
            difference |= static_cast<uint8_t>(expectedHash[i] ^ actualHash[i]);

        if (storedUserId != userId || difference != 0)
            return AuthenticationResult::InvalidCredentials;

        return AuthenticationResult::Success;
    }

    String createOwnerSession()
    {
        if (!configReady || setupState != SetupState::Claimed)
            return String();

        uint8_t tokenBytes[SESSION_TOKEN_BYTES];
        esp_fill_random(tokenBytes, sizeof(tokenBytes));
        String token = toHex(tokenBytes, sizeof(tokenBytes));
        if (token.length() != SESSION_TOKEN_BYTES * 2)
            return String();

        // A successful new login replaces the previous controller session.
        activeSessionToken = token;
        activeSession = true;
        return token;
    }

    bool isOwnerSessionValid(const String &sessionToken)
    {
        return configReady && setupState == SetupState::Claimed && activeSession &&
               !sessionToken.isEmpty() &&
               constantTimeTokenEquals(sessionToken, activeSessionToken);
    }

    bool revokeOwnerSession(const String &sessionToken)
    {
        if (!isOwnerSessionValid(sessionToken))
            return false;
        invalidateOwnerSession();
        return true;
    }

    void invalidateOwnerSession()
    {
        activeSessionToken = String();
        activeSession = false;
    }

    ResetResult resetController(const String &pin)
    {
        if (!configReady)
            return ResetResult::NotReady;

        bool authorized = false;

        // A user-created reset PIN is the normal reset credential.
        if (isValidResetPin(pin) && setupState == SetupState::Claimed)
        {
            Preferences readPreferences;
            if (!readPreferences.begin(NVS_NAMESPACE, true))
                return ResetResult::StorageFailure;

            const String resetSaltHex = readPreferences.getString(RESET_PIN_SALT_KEY, "");
            const String resetHashHex = readPreferences.getString(RESET_PIN_HASH_KEY, "");
            readPreferences.end();

            uint8_t resetSalt[SALT_BYTES];
            uint8_t expectedHash[HASH_BYTES];
            uint8_t actualHash[HASH_BYTES];
            if (fromHex(resetSaltHex, resetSalt, sizeof(resetSalt)) &&
                fromHex(resetHashHex, expectedHash, sizeof(expectedHash)))
            {
                if (!derivePasswordHash(pin, resetSalt, actualHash))
                    return ResetResult::StorageFailure;

                uint8_t difference = 0;
                for (size_t i = 0; i < HASH_BYTES; ++i)
                    difference |= static_cast<uint8_t>(expectedHash[i] ^ actualHash[i]);
                authorized = (difference == 0);
            }
        }

// Temporary development bypass. Disable PRAKIVA_ENABLE_TEST_MASTER_PIN
// before production. This bypass is intentionally separate from the
// owner-created reset PIN.
#if PRAKIVA_ENABLE_TEST_MASTER_PIN
        if (pin == TEST_MASTER_PIN)
            authorized = true;
#endif

        if (!authorized)
            return ResetResult::InvalidPin;

        Preferences preferences;
        if (!preferences.begin(NVS_NAMESPACE, false))
            return ResetResult::StorageFailure;

        // Remove ownership and reset-PIN verifier. Keep device identity
        // untouched: identity is stored independently of this configuration namespace.
        preferences.remove(OWNER_ID_KEY);
        preferences.remove(OWNER_SALT_KEY);
        preferences.remove(OWNER_HASH_KEY);
        preferences.remove(RESET_PIN_SALT_KEY);
        preferences.remove(RESET_PIN_HASH_KEY);

        const bool credentialsCleared =
            !preferences.isKey(OWNER_ID_KEY) &&
            !preferences.isKey(OWNER_SALT_KEY) &&
            !preferences.isKey(OWNER_HASH_KEY) &&
            !preferences.isKey(RESET_PIN_SALT_KEY) &&
            !preferences.isKey(RESET_PIN_HASH_KEY);

        if (!credentialsCleared)
        {
            preferences.end();
            return ResetResult::StorageFailure;
        }

        // Mark unclaimed only after credential removal has been verified.
        const size_t stateWritten =
            preferences.putUChar(SETUP_STATE_KEY, static_cast<uint8_t>(SetupState::Unclaimed));
        const uint8_t verifiedState = preferences.getUChar(SETUP_STATE_KEY, 255);
        preferences.end();

        if (stateWritten == 0 ||
            verifiedState != static_cast<uint8_t>(SetupState::Unclaimed))
            return ResetResult::StorageFailure;

        setupState = SetupState::Unclaimed;
        invalidateOwnerSession();
        Serial.println("[CONFIG] Controller ownership reset; setup is unclaimed.");
        return ResetResult::Success;
    }
}
