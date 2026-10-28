#pragma once

#include <Arduino.h>

namespace prakiva
{

    enum class SetupState : uint8_t
    {
        Unclaimed = 0,
        Claimed = 1,
    };

    bool beginConfig();
    bool isConfigReady();
    SetupState getSetupState();
    const char *getSetupStateName();
    enum class ClaimResult : uint8_t
    {
        Success = 0,
        NotReady,
        AlreadyClaimed,
        InvalidUserId,
        InvalidPassword,
        InvalidResetPin,
        CryptoFailure,
        StorageFailure,
    };

    ClaimResult claimOwnerAccount(const String &userId, const String &password, const String &resetPin);

    enum class AuthenticationResult : uint8_t
    {
        Success = 0,
        NotReady,
        NotClaimed,
        InvalidCredentials,
        CryptoFailure,
        StorageFailure,
    };

    AuthenticationResult authenticateOwner(const String &userId, const String &password);

    // RAM-only session: one active owner session per controller, invalidated on reboot.
    String createOwnerSession();
    bool isOwnerSessionValid(const String &sessionToken);
    bool revokeOwnerSession(const String &sessionToken);
    void invalidateOwnerSession();

    enum class ResetResult : uint8_t
    {
        Success = 0,
        NotReady,
        InvalidPin,
        StorageFailure,
    };

    ResetResult resetController(const String &pin);

} // namespace prakiva
