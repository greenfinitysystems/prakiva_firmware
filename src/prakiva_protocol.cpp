#include "prakiva_protocol.h"

#include <ArduinoJson.h>

#include "prakiva_config.h"
#include "prakiva_device.h"
#include "prakiva_identity.h"
#include "prakiva_wifi.h"
#include "prakiva_time.h"

namespace prakiva
{
    namespace
    {

        constexpr char PROTOCOL_VERSION[] = "0.1";
        constexpr size_t MAX_REQUEST_BYTES = 512;

        String makeErrorResponse(
            const String &requestId,
            const char *errorCode,
            const char *message)
        {
            JsonDocument response;
            response["request_id"] = requestId;
            response["protocol_version"] = PROTOCOL_VERSION;
            response["status"] = "error";

            JsonObject error = response["error"].to<JsonObject>();
            error["code"] = errorCode;
            error["message"] = message;

            String output;
            serializeJson(response, output);
            return output;
        }

        String makeDeviceInfoResponse(const String &requestId)
        {
            const DeviceInfo &device = getDeviceInfo();

            JsonDocument response;
            response["request_id"] = requestId;
            response["protocol_version"] = PROTOCOL_VERSION;
            response["status"] = "ok";

            JsonObject data = response["data"].to<JsonObject>();
            data["device_id"] = getDeviceId();
            data["product"] = device.productName;
            data["firmware_version"] = device.firmwareVersion;
            data["hardware_family"] = device.hardwareFamily;

            String output;
            serializeJson(response, output);
            return output;
        }

        String makeSetupStatusResponse(const String &requestId)
        {
            JsonDocument response;
            response["request_id"] = requestId;
            response["protocol_version"] = PROTOCOL_VERSION;
            response["status"] = "ok";

            JsonObject data = response["data"].to<JsonObject>();
            data["device_id"] = getDeviceId();
            data["setup_state"] = getSetupStateName();
            data["setup_required"] = (getSetupState() == SetupState::Unclaimed);

            String output;
            serializeJson(response, output);
            return output;
        }

        String makeClaimResponse(const String &requestId, const JsonDocument &request)
        {
            JsonVariantConst userIdValue = request["user_id"];
            JsonVariantConst passwordValue = request["password"];
            JsonVariantConst resetPinValue = request["reset_pin"];
            if (!userIdValue.is<const char *>() || !passwordValue.is<const char *>() ||
                !resetPinValue.is<const char *>())
                return makeErrorResponse(requestId, "INVALID_REQUEST", "The user_id, password, and reset_pin fields must be strings.");

            const String userId = userIdValue.as<String>();
            const String password = passwordValue.as<String>();
            const String resetPin = resetPinValue.as<String>();
            const ClaimResult result = claimOwnerAccount(userId, password, resetPin);
            switch (result)
            {
            case ClaimResult::Success:
            {
                JsonDocument response;
                response["request_id"] = requestId;
                response["protocol_version"] = PROTOCOL_VERSION;
                response["status"] = "ok";
                JsonObject data = response["data"].to<JsonObject>();
                data["device_id"] = getDeviceId();
                data["setup_state"] = getSetupStateName();
                data["setup_required"] = false;
                data["owner_user_id"] = userId;
                String output;
                serializeJson(response, output);
                return output;
            }
            case ClaimResult::NotReady:
                return makeErrorResponse(requestId, "CONFIG_NOT_READY", "The configuration state is not initialized.");
            case ClaimResult::AlreadyClaimed:
                return makeErrorResponse(requestId, "ALREADY_CLAIMED", "This controller has already been claimed.");
            case ClaimResult::InvalidUserId:
                return makeErrorResponse(requestId, "INVALID_USER_ID", "User ID must be 3 to 32 characters using letters, numbers, dot, underscore, or hyphen.");
            case ClaimResult::InvalidPassword:
                return makeErrorResponse(requestId, "INVALID_PASSWORD", "Password must be between 8 and 64 characters.");
            case ClaimResult::InvalidResetPin:
                return makeErrorResponse(requestId, "INVALID_RESET_PIN", "Reset PIN must contain exactly 10 digits.");
            case ClaimResult::CryptoFailure:
                return makeErrorResponse(requestId, "CRYPTO_FAILURE", "The controller could not securely process the password.");
            case ClaimResult::StorageFailure:
                return makeErrorResponse(requestId, "STORAGE_FAILURE", "Owner credentials could not be saved. The controller remains unclaimed.");
            }
            return makeErrorResponse(requestId, "INTERNAL_ERROR", "The claim operation failed.");
        }

        String makeAuthenticateResponse(const String &requestId, const JsonDocument &request)
        {
            JsonVariantConst userIdValue = request["user_id"];
            JsonVariantConst passwordValue = request["password"];
            if (!userIdValue.is<const char *>() || !passwordValue.is<const char *>())
                return makeErrorResponse(requestId, "INVALID_REQUEST", "The user_id and password fields must be strings.");

            const AuthenticationResult result =
                authenticateOwner(userIdValue.as<String>(), passwordValue.as<String>());

            switch (result)
            {
            case AuthenticationResult::Success:
            {
                JsonDocument response;
                response["request_id"] = requestId;
                response["protocol_version"] = PROTOCOL_VERSION;
                response["status"] = "ok";
                const String sessionToken = createOwnerSession();
                if (sessionToken.isEmpty())
                    return makeErrorResponse(requestId, "SESSION_CREATION_FAILED", "The controller could not create an owner session.");

                JsonObject data = response["data"].to<JsonObject>();
                data["device_id"] = getDeviceId();
                data["authenticated"] = true;
                data["owner_user_id"] = userIdValue.as<String>();
                data["session_token"] = sessionToken;
                String output;
                serializeJson(response, output);
                return output;
            }
            case AuthenticationResult::NotReady:
                return makeErrorResponse(requestId, "CONFIG_NOT_READY", "The configuration state is not initialized.");
            case AuthenticationResult::NotClaimed:
                return makeErrorResponse(requestId, "NOT_CLAIMED", "This controller has not been claimed.");
            case AuthenticationResult::InvalidCredentials:
                return makeErrorResponse(requestId, "INVALID_CREDENTIALS", "The owner user ID or password is incorrect.");
            case AuthenticationResult::CryptoFailure:
                return makeErrorResponse(requestId, "CRYPTO_FAILURE", "The controller could not verify the password securely.");
            case AuthenticationResult::StorageFailure:
                return makeErrorResponse(requestId, "STORAGE_FAILURE", "Stored owner credentials could not be read or verified.");
            }
            return makeErrorResponse(requestId, "INTERNAL_ERROR", "Owner authentication failed.");
        }

        String makeValidateSessionResponse(const String &requestId, const JsonDocument &request)
        {
            JsonVariantConst tokenValue = request["session_token"];
            if (!tokenValue.is<const char *>())
                return makeErrorResponse(requestId, "INVALID_REQUEST", "The session_token field must be a string.");

            JsonDocument response;
            response["request_id"] = requestId;
            response["protocol_version"] = PROTOCOL_VERSION;
            response["status"] = "ok";
            JsonObject data = response["data"].to<JsonObject>();
            data["device_id"] = getDeviceId();
            data["session_valid"] = isOwnerSessionValid(tokenValue.as<String>());
            String output;
            serializeJson(response, output);
            return output;
        }

        String makeLogoutResponse(const String &requestId, const JsonDocument &request)
        {
            JsonVariantConst tokenValue = request["session_token"];
            if (!tokenValue.is<const char *>())
                return makeErrorResponse(requestId, "INVALID_REQUEST", "The session_token field must be a string.");

            const bool loggedOut = revokeOwnerSession(tokenValue.as<String>());
            JsonDocument response;
            response["request_id"] = requestId;
            response["protocol_version"] = PROTOCOL_VERSION;
            response["status"] = "ok";
            JsonObject data = response["data"].to<JsonObject>();
            data["device_id"] = getDeviceId();
            data["logged_out"] = loggedOut;
            String output;
            serializeJson(response, output);
            return output;
        }

        String makeHelloWorldResponse(const String &requestId, const JsonDocument &request)
        {
            JsonVariantConst tokenValue = request["session_token"];
            if (!tokenValue.is<const char *>())
                return makeErrorResponse(requestId, "INVALID_REQUEST", "The session_token field must be a string.");

            if (!isOwnerSessionValid(tokenValue.as<String>()))
                return makeErrorResponse(requestId, "UNAUTHENTICATED", "A valid owner session is required for this operation.");

            JsonDocument response;
            response["request_id"] = requestId;
            response["protocol_version"] = PROTOCOL_VERSION;
            response["status"] = "ok";
            JsonObject data = response["data"].to<JsonObject>();
            data["device_id"] = getDeviceId();
            data["message"] = "Hello, world!";
            String output;
            serializeJson(response, output);
            return output;
        }

        String makeWifiStatusResponse(const String &requestId)
        {
            JsonDocument response;
            response["request_id"] = requestId;
            response["protocol_version"] = PROTOCOL_VERSION;
            response["status"] = "ok";
            JsonObject data = response["data"].to<JsonObject>();
            data["wifi_type"] = prakiva::wifiModeName();
            data["wifi_status"] = prakiva::wifiStatusName();
            String output;
            serializeJson(response, output);
            return output;
        }

        String makeSetWifiResponse(const String &requestId, const JsonDocument &request)
        {
            JsonVariantConst tokenValue = request["session_token"];
            JsonVariantConst typeValue = request["wifi_type"];
            JsonVariantConst ssidValue = request["ssid"];
            JsonVariantConst passwordValue = request["password"];
            if (!tokenValue.is<const char *>() || !typeValue.is<const char *>() ||
                !ssidValue.is<const char *>() || !passwordValue.is<const char *>())
                return makeErrorResponse(requestId, "INVALID_REQUEST", "session_token, wifi_type, ssid, and password must be strings.");
            if (!isOwnerSessionValid(tokenValue.as<String>()))
                return makeErrorResponse(requestId, "UNAUTHENTICATED", "A valid owner session is required for this operation.");
            String errorCode, errorMessage;
            if (!configureWifi(typeValue.as<String>(), ssidValue.as<String>(), passwordValue.as<String>(), errorCode, errorMessage))
                return makeErrorResponse(requestId, errorCode.c_str(), errorMessage.c_str());
            JsonDocument response;
            response["request_id"] = requestId;
            response["protocol_version"] = PROTOCOL_VERSION;
            response["status"] = "ok";
            JsonObject data = response["data"].to<JsonObject>();
            data["wifi_type"] = typeValue.as<String>();
            data["wifi_status"] = "switching_network";
            data["reconnect_required"] = true;
            String output;
            serializeJson(response, output);
            return output;
        }

        String makeResetResponse(const String &requestId, const JsonDocument &request)
        {
            JsonVariantConst pinValue = request["pin"];
            if (!pinValue.is<const char *>())
                return makeErrorResponse(requestId, "INVALID_REQUEST", "The pin field must be a string.");

            const ResetResult result = resetController(pinValue.as<String>());
            switch (result)
            {
            case ResetResult::Success:
            {
                resetWifiToFactory();
                JsonDocument response;
                response["request_id"] = requestId;
                response["protocol_version"] = PROTOCOL_VERSION;
                response["status"] = "ok";
                JsonObject data = response["data"].to<JsonObject>();
                data["device_id"] = getDeviceId();
                data["setup_state"] = getSetupStateName();
                data["setup_required"] = true;
                String output;
                serializeJson(response, output);
                return output;
            }
            case ResetResult::NotReady:
                return makeErrorResponse(requestId, "CONFIG_NOT_READY", "The configuration state is not initialized.");
            case ResetResult::InvalidPin:
                return makeErrorResponse(requestId, "INVALID_RESET_PIN", "The reset PIN is invalid.");
            case ResetResult::StorageFailure:
                return makeErrorResponse(requestId, "STORAGE_FAILURE", "Controller reset could not be completed and verified.");
            }
            return makeErrorResponse(requestId, "INTERNAL_ERROR", "The reset operation failed.");
        }

        bool requireTimeSession(const String &requestId, const JsonDocument &request, String &errorResponse)
        {
            JsonVariantConst tokenValue = request["session_token"];
            if (!tokenValue.is<const char *>() || !isOwnerSessionValid(tokenValue.as<String>()))
            {
                errorResponse = makeErrorResponse(requestId, "UNAUTHENTICATED", "A valid owner session is required for this operation.");
                return false;
            }
            return true;
        }

        String makeTimeStatusResponse(const String &requestId)
        {
            JsonDocument response;
            response["request_id"] = requestId;
            response["protocol_version"] = PROTOCOL_VERSION;
            response["status"] = "ok";
            JsonObject data = response["data"].to<JsonObject>();
            data["device_id"] = getDeviceId();
            addTimeStatusJson(data);
            String output;
            serializeJson(response, output);
            return output;
        }

        String makeTimeSyncResponse(const String &requestId, const JsonDocument &request, bool fromNtp)
        {
            String authError;
            if (!requireTimeSession(requestId, request, authError))
                return authError;

            String errorCode, errorMessage;
            bool ok = false;
            if (fromNtp)
            {
                ok = syncTimeFromNtp(errorCode, errorMessage);
            }
            else
            {
                JsonVariantConst timestampValue = request["unix_time"];
                if (!timestampValue.is<uint32_t>())
                    return makeErrorResponse(requestId, "INVALID_REQUEST", "unix_time must be a Unix timestamp in seconds (integer). ");
                ok = syncTimeFromPhone(timestampValue.as<uint32_t>(), errorCode, errorMessage);
            }

            if (!ok)
                return makeErrorResponse(requestId, errorCode.c_str(), errorMessage.c_str());

            JsonDocument response;
            response["request_id"] = requestId;
            response["protocol_version"] = PROTOCOL_VERSION;
            response["status"] = "ok";
            JsonObject data = response["data"].to<JsonObject>();
            data["device_id"] = getDeviceId();
            data["sync_source"] = fromNtp ? "ntp" : "phone";
            addTimeStatusJson(data);
            String output;
            serializeJson(response, output);
            return output;
        }

        String makeSetScheduleOffsetResponse(const String &requestId, const JsonDocument &request)
        {
            String authError;
            if (!requireTimeSession(requestId, request, authError))
                return authError;

            JsonVariantConst offsetValue = request["offset_minutes"];
            if (!offsetValue.is<int>())
                return makeErrorResponse(requestId, "INVALID_REQUEST", "offset_minutes must be an integer between -720 and 720.");

            String errorCode, errorMessage;
            if (!setScheduleOffsetMinutes(static_cast<int16_t>(offsetValue.as<int>()), errorCode, errorMessage))
                return makeErrorResponse(requestId, errorCode.c_str(), errorMessage.c_str());

            JsonDocument response;
            response["request_id"] = requestId;
            response["protocol_version"] = PROTOCOL_VERSION;
            response["status"] = "ok";
            JsonObject data = response["data"].to<JsonObject>();
            data["device_id"] = getDeviceId();
            addTimeStatusJson(data);
            String output;
            serializeJson(response, output);
            return output;
        }

    } // namespace

    String handleProtocolRequest(const String &requestJson)
    {
        if (requestJson.length() > MAX_REQUEST_BYTES)
        {
            return makeErrorResponse(
                "",
                "REQUEST_TOO_LARGE",
                "The request exceeds the maximum permitted size.");
        }

        JsonDocument request;
        const DeserializationError parseError =
            deserializeJson(request, requestJson);

        if (parseError)
        {
            return makeErrorResponse(
                "",
                "INVALID_JSON",
                "The request is not valid JSON.");
        }

        if (!request.is<JsonObject>())
        {
            return makeErrorResponse(
                "",
                "INVALID_REQUEST",
                "The request must be a JSON object.");
        }

        JsonVariantConst requestIdValue = request["request_id"];
        JsonVariantConst operationValue = request["operation"];
        JsonVariantConst versionValue = request["protocol_version"];

        if (!requestIdValue.is<const char *>() ||
            !operationValue.is<const char *>() ||
            !versionValue.is<const char *>())
        {
            return makeErrorResponse(
                "",
                "INVALID_REQUEST",
                "Required fields must be strings.");
        }

        const String requestId = requestIdValue.as<String>();
        const String operation = operationValue.as<String>();
        const String version = versionValue.as<String>();

        if (requestId.isEmpty() || requestId.length() > 64)
        {
            return makeErrorResponse(
                "",
                "INVALID_REQUEST",
                "The request ID must contain 1 to 64 characters.");
        }

        if (version != PROTOCOL_VERSION)
        {
            return makeErrorResponse(
                requestId,
                "UNSUPPORTED_VERSION",
                "The requested protocol version is not supported.");
        }

        if (operation == "get_device_info")
        {
            return makeDeviceInfoResponse(requestId);
        }

        if (operation == "get_setup_status")
        {
            if (!isConfigReady())
            {
                return makeErrorResponse(
                    requestId,
                    "CONFIG_NOT_READY",
                    "The configuration state is not initialized.");
            }
            return makeSetupStatusResponse(requestId);
        }

        if (operation == "claim_controller")
        {
            if (!isConfigReady())
                return makeErrorResponse(requestId, "CONFIG_NOT_READY", "The configuration state is not initialized.");
            return makeClaimResponse(requestId, request);
        }

        if (operation == "authenticate_owner")
        {
            if (!isConfigReady())
                return makeErrorResponse(requestId, "CONFIG_NOT_READY", "The configuration state is not initialized.");
            return makeAuthenticateResponse(requestId, request);
        }

        if (operation == "validate_session")
        {
            if (!isConfigReady())
                return makeErrorResponse(requestId, "CONFIG_NOT_READY", "The configuration state is not initialized.");
            return makeValidateSessionResponse(requestId, request);
        }

        if (operation == "logout")
        {
            if (!isConfigReady())
                return makeErrorResponse(requestId, "CONFIG_NOT_READY", "The configuration state is not initialized.");
            return makeLogoutResponse(requestId, request);
        }

        if (operation == "hello_world")
        {
            if (!isConfigReady())
                return makeErrorResponse(requestId, "CONFIG_NOT_READY", "The configuration state is not initialized.");
            return makeHelloWorldResponse(requestId, request);
        }

        if (operation == "get_wifi_status")
        {
            if (!isConfigReady())
                return makeErrorResponse(requestId, "CONFIG_NOT_READY", "The configuration state is not initialized.");
            JsonVariantConst tokenValue = request["session_token"];
            if (!tokenValue.is<const char *>() || !isOwnerSessionValid(tokenValue.as<String>()))
                return makeErrorResponse(requestId, "UNAUTHENTICATED", "A valid owner session is required for this operation.");
            return makeWifiStatusResponse(requestId);
        }

        if (operation == "set_wifi_config")
        {
            if (!isConfigReady())
                return makeErrorResponse(requestId, "CONFIG_NOT_READY", "The configuration state is not initialized.");
            return makeSetWifiResponse(requestId, request);
        }

        if (operation == "reset_controller")
        {
            if (!isConfigReady())
                return makeErrorResponse(requestId, "CONFIG_NOT_READY", "The configuration state is not initialized.");
            return makeResetResponse(requestId, request);
        }

        if (operation == "get_time_status")
        {
            if (!isConfigReady())
                return makeErrorResponse(requestId, "CONFIG_NOT_READY", "The configuration state is not initialized.");
            String authError;
            if (!requireTimeSession(requestId, request, authError))
                return authError;
            if (!isTimeModuleReady())
                return makeErrorResponse(requestId, "TIME_NOT_READY", "The time module is not initialized.");
            return makeTimeStatusResponse(requestId);
        }

        if (operation == "sync_time_phone")
        {
            if (!isConfigReady())
                return makeErrorResponse(requestId, "CONFIG_NOT_READY", "The configuration state is not initialized.");
            if (!isTimeModuleReady())
                return makeErrorResponse(requestId, "TIME_NOT_READY", "The time module is not initialized.");
            return makeTimeSyncResponse(requestId, request, false);
        }

        if (operation == "sync_time_ntp")
        {
            if (!isConfigReady())
                return makeErrorResponse(requestId, "CONFIG_NOT_READY", "The configuration state is not initialized.");
            if (!isTimeModuleReady())
                return makeErrorResponse(requestId, "TIME_NOT_READY", "The time module is not initialized.");
            return makeTimeSyncResponse(requestId, request, true);
        }

        if (operation == "set_schedule_offset")
        {
            if (!isConfigReady())
                return makeErrorResponse(requestId, "CONFIG_NOT_READY", "The configuration state is not initialized.");
            if (!isTimeModuleReady())
                return makeErrorResponse(requestId, "TIME_NOT_READY", "The time module is not initialized.");
            return makeSetScheduleOffsetResponse(requestId, request);
        }

        return makeErrorResponse(
            requestId,
            "UNSUPPORTED_OPERATION",
            "The requested operation is not supported.");
    }

} // namespace prakiva
