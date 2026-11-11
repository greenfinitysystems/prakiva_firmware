#include "core/prakiva_protocol.h"

#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>

#include "core/prakiva_config.h"
#include "core/prakiva_device.h"
#include "core/prakiva_identity.h"
#include "core/prakiva_wifi.h"
#include "core/prakiva_time.h"
#include "core/prakiva_storage.h"
#include "core/prakiva_json.h"
#include "core/prakiva_history.h"

#include "product/prakiva_product_runtime.h"
#include "components/core/prakiva_virtual_actuator_driver.h"
#include "product/prakiva_capability_resolver.h"
#include "product/prakiva_capability_operations.h"
#include "scheduler/prakiva_scheduler.h"

namespace prakiva
{
    namespace
    {

        constexpr char PROTOCOL_VERSION[] = "0.1";
        // The app editor document (curves, policies and profiles) is stored in app_configuration.
        // Allow room for the complete JSON envelope while retaining a bounded request size.
        constexpr size_t MAX_REQUEST_BYTES = 64 * 1024;

        String makeErrorResponse(const String &requestId, const char *errorCode, const char *message)
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

        String makeCapabilitiesResponse(const String &requestId)
        {
            if (!productRuntimeReady())
                return makeErrorResponse(requestId, "PRODUCT_NOT_READY", "The product runtime is not initialized.");

            const DeviceInfo &device = getDeviceInfo();
            const ProductProjectionSet &projections = activeProductProjections();
            JsonDocument capabilityProjection;
            DeserializationError parseError = deserializeJson(capabilityProjection, projections.capabilitiesJson);
            if (parseError || !capabilityProjection["capabilities"].is<JsonArray>())
                return makeErrorResponse(requestId, "CAPABILITIES_UNAVAILABLE", "The generated capability projection is invalid.");

            JsonDocument response;
            response["request_id"] = requestId;
            response["protocol_version"] = PROTOCOL_VERSION;
            response["status"] = "ok";

            JsonObject data = response["data"].to<JsonObject>();
            data["device_id"] = getDeviceId();

            JsonObject controller = data["controller"].to<JsonObject>();
            controller["product"] = device.productName;
            controller["firmware_version"] = device.firmwareVersion;
            controller["hardware_family"] = device.hardwareFamily;
            controller["flash_mb"] = 16;
            controller["psram_mb"] = 8;

            JsonArray interfaces = data["interfaces"].to<JsonArray>();
            interfaces.add("http_json_api");
            interfaces.add("udp_discovery");
            interfaces.add("wifi_ap_sta");
            interfaces.add("owner_authentication");
            interfaces.add("rtc_time");
            interfaces.add("ntp_time_sync");
            interfaces.add("persistent_json_configuration");

            JsonObject operations = data["operations"].to<JsonObject>();
            operations["get_capabilities"] = true;
            operations["invoke_actuator"] = true;
            operations["get_actuator_execution_state"] = true;
            operations["get_capability"] = true;
            operations["get_sensor_history"] = true;
            operations["set_capability"] = true;
            operations["set_configuration"] = true;
            operations["get_configuration"] = true;
            operations["get_timestamp"] = true;
            operations["get_time_status"] = true;
            operations["sync_time_phone"] = true;
            operations["sync_time_ntp"] = true;

            data["capabilities"].set(capabilityProjection["capabilities"]);
            if (capabilityProjection["virtual_sensors"].is<JsonArray>())
                data["virtual_sensors"].set(capabilityProjection["virtual_sensors"]);

            String output;
            serializeJson(response, output);
            return output;
        }

        String makeGetSensorHistoryResponse(const String &requestId, const JsonDocument &request)
        {
            JsonVariantConst capabilityIdValue=request["capability_id"];
            JsonVariantConst channelIdValue=request["channel_id"];
            if(!capabilityIdValue.is<const char *>() || !channelIdValue.is<const char *>())
                return makeErrorResponse(requestId,"INVALID_REQUEST","capability_id and channel_id must be strings.");
            const String capabilityId=capabilityIdValue.as<String>();
            const String channelId=channelIdValue.as<String>();
            size_t limit=request["limit"].is<unsigned int>() ? request["limit"].as<unsigned int>() : 288;
            if(capabilityId.isEmpty() || channelId.isEmpty() || capabilityId.length()>64 || channelId.length()>64 || limit>288)
                return makeErrorResponse(requestId,"INVALID_REQUEST","Invalid sensor identifiers or history limit (maximum 288).");
            if(!productCapabilityResolverReady())
                return makeErrorResponse(requestId,"CAPABILITY_RUNTIME_NOT_READY","The capability resolver is not initialized.");
            const CapabilityChannelResolution resolution=productCapabilityResolver().resolve(capabilityId.c_str(),channelId.c_str());
            if(resolution.status==CapabilityResolutionStatus::NotFound || resolution.kind!="sensor")
                return makeErrorResponse(requestId,"SENSOR_CAPABILITY_NOT_FOUND","The requested public sensor channel was not found.");
            if(!resolution.isOperational())
                return makeErrorResponse(requestId,"SENSOR_NOT_OPERATIONAL","The requested sensor is not operational.");
            JsonDocument response;
            response["request_id"]=requestId; response["protocol_version"]=PROTOCOL_VERSION; response["status"]="ok";
            JsonObject data=response["data"].to<JsonObject>();
            data["capability_id"]=capabilityId; data["channel_id"]=channelId;
            data["component_id"]=resolution.componentId;
            JsonArray points=data["points"].to<JsonArray>();
            if(!history.appendSensorHistory(resolution.componentId.c_str(),limit,points))
                return makeErrorResponse(requestId,"HISTORY_UNAVAILABLE","History is not available for the requested sensor.");
            String output; serializeJson(response,output); return output;
        }

        String makeGetCapabilityResponse(const String &requestId, const JsonDocument &request)
        {
            JsonVariantConst capabilityIdValue = request["capability_id"];
            JsonVariantConst channelIdValue = request["channel_id"];
            if (!capabilityIdValue.is<const char *>() || !channelIdValue.is<const char *>())
                return makeErrorResponse(requestId, "INVALID_REQUEST", "capability_id and channel_id must be strings.");

            const String capabilityId = capabilityIdValue.as<String>();
            const String channelId = channelIdValue.as<String>();
            if (capabilityId.isEmpty() || capabilityId.length() > 64 ||
                channelId.isEmpty() || channelId.length() > 64)
                return makeErrorResponse(requestId, "INVALID_REQUEST", "capability_id and channel_id must contain 1 to 64 characters.");

            const CapabilityReadResult result = readCapabilityChannel(
                capabilityId.c_str(), channelId.c_str());

            switch (result.status)
            {
            case CapabilityOperationStatus::ResolverNotReady:
                return makeErrorResponse(requestId, "CAPABILITY_RUNTIME_NOT_READY", "The capability resolver is not initialized.");
            case CapabilityOperationStatus::NotFound:
                return makeErrorResponse(requestId, "CAPABILITY_NOT_FOUND", "The requested capability channel is not declared.");
            case CapabilityOperationStatus::NotOperational:
                return makeErrorResponse(requestId, "CAPABILITY_NOT_OPERATIONAL", "The requested capability channel is not operational.");
            case CapabilityOperationStatus::InterfaceMismatch:
                return makeErrorResponse(requestId, "CAPABILITY_INTERFACE_MISMATCH", "The component does not implement the interface required by its capability kind.");
            // These statuses are write-specific. Keep the GET switch exhaustive
            // so future enum additions cannot silently bypass error handling.
            case CapabilityOperationStatus::NotActuator:
            case CapabilityOperationStatus::InvalidValue:
            case CapabilityOperationStatus::OutOfRange:
            case CapabilityOperationStatus::DriverRejected:
                return makeErrorResponse(requestId, "INTERNAL_ERROR",
                    "An actuator-write status was returned while processing a capability read.");
            case CapabilityOperationStatus::Ok:
                break;
            }

            JsonDocument response;
            response["request_id"] = requestId;
            response["protocol_version"] = PROTOCOL_VERSION;
            response["status"] = "ok";
            JsonObject data = response["data"].to<JsonObject>();
            data["capability_id"] = capabilityId;
            data["channel_id"] = channelId;
            data["kind"] = result.kind;
            data["available"] = result.available;

            if (result.kind == "sensor")
            {
                const SensorValue &reading = result.sensorValue;
                data["measurement_valid"] = result.available && reading.valid;
                data["measurement_status"] = result.available
                    ? measurementStatusName(reading.status) : "read_failed";
                if (result.available)
                {
                    JsonObject value = data["value"].to<JsonObject>();
                    value["numeric_value"] = reading.value.numericValue;
                    value["value_type"] = valueTypeName(reading.value.valueType);
                    data["unit"] = reading.unit;
                    data["timestamp"] = reading.timestamp;
                }
            }
            else
            {
                if (result.available)
                {
                    JsonObject value = data["value"].to<JsonObject>();
                    value["numeric_value"] = result.actuatorValue.numericValue;
                    value["value_type"] = valueTypeName(result.actuatorValue.valueType);
                }
                else
                {
                    data["read_status"] = "read_failed";
                }
            }

            String output;
            serializeJson(response, output);
            return output;
        }

        bool parseCapabilityValueType(const String &name, ValueType &type)
        {
            if (name == "int32") type = ValueType::Int32;
            else if (name == "uint32") type = ValueType::UInt32;
            else if (name == "float32") type = ValueType::Float32;
            else if (name == "float64") type = ValueType::Float64;
            else return false;
            return true;
        }

        // Explicit operation invocation: only virtual actuator channels are eligible.
        // No GPIO/physical actuator can be invoked through this endpoint.
        String makeInvokeActuatorResponse(const String &requestId, const JsonDocument &request)
        {
            JsonVariantConst cap = request["capability_id"];
            JsonVariantConst channel = request["channel_id"];
            if (!cap.is<const char *>() || !channel.is<const char *>())
                return makeErrorResponse(requestId, "INVALID_REQUEST", "capability_id and channel_id must be strings.");
            const String capId = cap.as<String>();
            const String channelId = channel.as<String>();
            if (capId.isEmpty() || channelId.isEmpty() || capId.length() > 64 || channelId.length() > 64)
                return makeErrorResponse(requestId, "INVALID_REQUEST", "Invalid capability or channel identifier.");
            if (!productCapabilityResolverReady())
                return makeErrorResponse(requestId, "CAPABILITY_RUNTIME_NOT_READY", "Capability resolver is not initialized.");
            const CapabilityChannelResolution resolved = productCapabilityResolver().resolve(capId.c_str(), channelId.c_str());
            if (resolved.kind != "actuator" || !resolved.isOperational() || !resolved.component)
                return makeErrorResponse(requestId, "ACTUATOR_NOT_OPERATIONAL", "Actuator channel is unavailable.");
            auto *driver = resolved.component->asVirtualActuatorDriver();
            if (!driver)
                return makeErrorResponse(requestId, "NOT_TRIGGERABLE", "Only virtual actuator channels may be invoked.");
            NumericValue state;
            if (!driver->get(state) || state.numericValue != "0")
                return makeErrorResponse(requestId, "ACTUATOR_BUSY", "Virtual actuator is not idle.");
            if (!driver->setState(true))
                return makeErrorResponse(requestId, "ACTUATOR_REJECTED", "Virtual actuator rejected the invocation.");
            JsonDocument response;
            response["request_id"] = requestId;
            response["protocol_version"] = PROTOCOL_VERSION;
            response["status"] = "ok";
            JsonObject payload = response["data"].to<JsonObject>();
            payload["capability_id"] = capId;
            payload["channel_id"] = channelId;
            payload["accepted"] = true;
            String output;
            serializeJson(response, output);
            return output;
        }

        // Read-only runtime state, sourced directly from the virtual actuator driver.
        String makeActuatorExecutionStateResponse(const String &requestId, const JsonDocument &request)
        {
            JsonVariantConst cap = request["capability_id"];
            JsonVariantConst channel = request["channel_id"];
            if (!cap.is<const char *>() || !channel.is<const char *>())
                return makeErrorResponse(requestId, "INVALID_REQUEST", "capability_id and channel_id must be strings.");
            const String capId = cap.as<String>();
            const String channelId = channel.as<String>();
            if (capId.isEmpty() || channelId.isEmpty() || capId.length() > 64 || channelId.length() > 64)
                return makeErrorResponse(requestId, "INVALID_REQUEST", "Invalid capability or channel identifier.");
            if (!productCapabilityResolverReady())
                return makeErrorResponse(requestId, "CAPABILITY_RUNTIME_NOT_READY", "Capability resolver is not initialized.");
            const CapabilityChannelResolution resolved = productCapabilityResolver().resolve(capId.c_str(), channelId.c_str());
            if (resolved.kind != "actuator" || !resolved.isOperational() || !resolved.component)
                return makeErrorResponse(requestId, "ACTUATOR_NOT_OPERATIONAL", "Actuator channel is unavailable.");
            auto *driver = resolved.component->asVirtualActuatorDriver();
            if (!driver)
                return makeErrorResponse(requestId, "NOT_TRIGGERABLE", "Only virtual actuator channels have execution state.");
            NumericValue state;
            if (!driver->get(state) || (state.numericValue != "0" && state.numericValue != "1"))
                return makeErrorResponse(requestId, "ACTUATOR_STATE_UNAVAILABLE", "Virtual actuator execution state is unavailable.");
            JsonDocument response;
            response["request_id"] = requestId;
            response["protocol_version"] = PROTOCOL_VERSION;
            response["status"] = "ok";
            JsonObject data = response["data"].to<JsonObject>();
            data["capability_id"] = capId;
            data["channel_id"] = channelId;
            data["execution_state"] = state.numericValue == "1" ? "RUNNING" : "IDLE";
            String output;
            serializeJson(response, output);
            return output;
        }

        String makeSetCapabilityResponse(const String &requestId, const JsonDocument &request)
        {
            JsonVariantConst capabilityIdValue = request["capability_id"];
            JsonVariantConst channelIdValue = request["channel_id"];
            JsonVariantConst valueObject = request["value"];

            if (!capabilityIdValue.is<const char *>() ||
                !channelIdValue.is<const char *>() ||
                !valueObject.is<JsonObjectConst>())
                return makeErrorResponse(requestId, "INVALID_REQUEST",
                    "capability_id, channel_id, and value object are required.");

            const String capabilityId = capabilityIdValue.as<String>();
            const String channelId = channelIdValue.as<String>();
            if (capabilityId.isEmpty() || capabilityId.length() > 64 ||
                channelId.isEmpty() || channelId.length() > 64)
                return makeErrorResponse(requestId, "INVALID_REQUEST",
                    "capability_id and channel_id must contain 1 to 64 characters.");

            JsonVariantConst numericValue = valueObject["numeric_value"];
            JsonVariantConst valueTypeValue = valueObject["value_type"];
            if (!numericValue.is<const char *>() || !valueTypeValue.is<const char *>())
                return makeErrorResponse(requestId, "INVALID_REQUEST",
                    "value.numeric_value and value.value_type must be strings.");

            NumericValue value;
            value.numericValue = numericValue.as<String>();
            if (!parseCapabilityValueType(valueTypeValue.as<String>(), value.valueType))
                return makeErrorResponse(requestId, "INVALID_VALUE_TYPE",
                    "value.value_type must be int32, uint32, float32, or float64.");

            const CapabilityWriteResult result = writeCapabilityChannel(
                capabilityId.c_str(), channelId.c_str(), value);

            switch (result.status)
            {
            case CapabilityOperationStatus::ResolverNotReady:
                return makeErrorResponse(requestId, "CAPABILITY_RUNTIME_NOT_READY",
                    "The capability resolver is not initialized.");
            case CapabilityOperationStatus::NotFound:
                return makeErrorResponse(requestId, "CAPABILITY_NOT_FOUND",
                    "The requested capability channel is not declared.");
            case CapabilityOperationStatus::NotOperational:
                return makeErrorResponse(requestId, "CAPABILITY_NOT_OPERATIONAL",
                    "The requested capability channel is not operational.");
            case CapabilityOperationStatus::NotActuator:
                return makeErrorResponse(requestId, "CAPABILITY_NOT_ACTUATOR",
                    "Only actuator channels can be modified.");
            case CapabilityOperationStatus::InterfaceMismatch:
                return makeErrorResponse(requestId, "CAPABILITY_INTERFACE_MISMATCH",
                    "The component does not implement the required actuator interface.");
            case CapabilityOperationStatus::InvalidValue:
                return makeErrorResponse(requestId, "INVALID_CAPABILITY_VALUE",
                    "The supplied value is invalid for the declared channel type.");
            case CapabilityOperationStatus::OutOfRange:
                return makeErrorResponse(requestId, "CAPABILITY_VALUE_OUT_OF_RANGE",
                    "The supplied value is outside the channel's declared range.");
            case CapabilityOperationStatus::DriverRejected:
                return makeErrorResponse(requestId, "ACTUATOR_WRITE_FAILED",
                    "The actuator driver rejected the requested value.");
            case CapabilityOperationStatus::Ok:
                break;
            }

            JsonDocument response;
            response["request_id"] = requestId;
            response["protocol_version"] = PROTOCOL_VERSION;
            response["status"] = "ok";
            JsonObject data = response["data"].to<JsonObject>();
            data["capability_id"] = capabilityId;
            data["channel_id"] = channelId;
            data["kind"] = result.kind;
            data["write_applied"] = result.writeApplied;
            data["readback_available"] = result.readbackAvailable;

            JsonObject applied = data["applied_value"].to<JsonObject>();
            applied["numeric_value"] = result.appliedValue.numericValue;
            applied["value_type"] = valueTypeName(result.appliedValue.valueType);

            if (result.readbackAvailable)
            {
                JsonObject current = data["current_value"].to<JsonObject>();
                current["numeric_value"] = result.currentValue.numericValue;
                current["value_type"] = valueTypeName(result.currentValue.valueType);
            }

            String output;
            serializeJson(response, output);
            return output;
        }

        String makeSetConfigurationResponse(const String &requestId, const JsonDocument &request)
        {
            JsonVariantConst tokenValue = request["session_token"];
            if (!tokenValue.is<const char *>())
                return makeErrorResponse(requestId, "INVALID_REQUEST", "session_token must be a string.");
            if (!isOwnerSessionValid(tokenValue.as<String>()))
                return makeErrorResponse(requestId, "UNAUTHENTICATED", "A valid owner session is required for this operation.");

            JsonVariantConst configuration = request["configuration"];
            if (!configuration.is<JsonObjectConst>())
                return makeErrorResponse(requestId, "INVALID_REQUEST", "configuration must be a JSON object.");

            String validationMessage;
            if (!validateProductConfiguration(configuration.as<JsonObjectConst>(), validationMessage))
                return makeErrorResponse(requestId, "INVALID_VIRTUAL_SENSOR_CONFIGURATION",
                                         validationMessage.c_str());

            uint32_t configurationTimestamp = 0;
            if (!getCurrentUnixTime(configurationTimestamp))
                return makeErrorResponse(requestId, "TIME_INVALID", "The controller clock must be valid before saving configuration.");

            String errorCode, errorMessage;
            if (!saveConfiguration(configuration, configurationTimestamp, errorCode, errorMessage))
                return makeErrorResponse(requestId, errorCode.c_str(), errorMessage.c_str());

            // Validation ran before persistence; after the durable commit, activate
            // the same values in the live virtual sensor instances.
            if (!applyProductConfiguration(configuration.as<JsonObjectConst>(), validationMessage))
                return makeErrorResponse(requestId, "VIRTUAL_SENSOR_APPLY_FAILED",
                                         validationMessage.c_str());

            // Rebuild the diagnostic policy runtime from the just-committed
            // configuration. Actuator dispatch remains deliberately disabled.
            if (scheduler.ready() && !scheduler.reloadPolicies())
                return makeErrorResponse(requestId, "SCHEDULER_RELOAD_FAILED",
                                         "Configuration was saved, but the scheduler could not reload its policy definitions.");

            JsonDocument response;
            response["request_id"] = requestId;
            response["protocol_version"] = PROTOCOL_VERSION;
            response["status"] = "ok";
            JsonObject data = response["data"].to<JsonObject>();
            data["saved"] = true;
            data["storage"] = "littlefs";
            data["path"] = "/configuration.json";
            data["configuration_timestamp"] = configurationTimestamp;
            String output;
            serializeJson(response, output);
            return output;
        }

        String makeGetConfigurationResponse(const String &requestId, const JsonDocument &request)
        {
            JsonVariantConst tokenValue = request["session_token"];
            if (!tokenValue.is<const char *>() || !isOwnerSessionValid(tokenValue.as<String>()))
                return makeErrorResponse(requestId, "UNAUTHENTICATED", "A valid owner session is required for this operation.");

            String configurationJson;
            uint32_t configurationTimestamp = 0;
            String errorCode, errorMessage;
            if (!loadConfigurationJson(configurationJson, configurationTimestamp, errorCode, errorMessage))
            {
                if (errorCode == "CONFIGURATION_NOT_FOUND")
                {
                    JsonDocument response;
                    response["request_id"] = requestId;
                    response["protocol_version"] = PROTOCOL_VERSION;
                    response["status"] = "ok";
                    JsonObject data = response["data"].to<JsonObject>();
                    data["storage"] = "littlefs";
                    data["configuration_exists"] = false;
                    data["configuration_timestamp"] = 0;
                    data["configuration"] = nullptr;
                    String output;
                    serializeJson(response, output);
                    return output;
                }
                return makeErrorResponse(requestId, errorCode.c_str(), errorMessage.c_str());
            }
            // Build only the shallow response envelope as a JsonDocument.
            // Append the already-serialized configuration as raw JSON rather
            // than copying its deeply nested tree into a second JsonDocument.
            JsonDocument response;
            response["request_id"] = requestId;
            response["protocol_version"] = PROTOCOL_VERSION;
            response["status"] = "ok";
            JsonObject data = response["data"].to<JsonObject>();
            data["storage"] = "littlefs";
            data["configuration_exists"] = true;
            data["configuration_timestamp"] = configurationTimestamp;

            String output;
            output.reserve(measureJson(response) + configurationJson.length() + 32);
            serializeJson(response, output);

            // The serialized shallow envelope ends with two braces: data and root.
            if (output.length() < 2 || output[output.length() - 2] != '}' || output[output.length() - 1] != '}')
                return makeErrorResponse(requestId, "INTERNAL_ERROR", "The configuration response could not be assembled.");

            output.remove(output.length() - 2);
            output += ",\"configuration\":";
            output += configurationJson;
            output += "}}";
            return output;
        }

        String makeTimestampResponse(const String &requestId)
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

        bool requireOwnerSession(const String &requestId, const JsonDocument &request, String &errorResponse)
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
            if (!requireOwnerSession(requestId, request, authError))
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
            if (!requireOwnerSession(requestId, request, authError))
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
        const DeserializationError parseError = deserializeJson(
            request,
            requestJson,
            DeserializationOption::NestingLimit(JSON_NESTING_LIMIT));

        if (parseError)
        {
            Serial.printf(
                "[API] JSON parse failed: %s; body length: %u bytes\n",
                parseError.c_str(),
                static_cast<unsigned>(requestJson.length()));

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

        if (operation == "get_actuator_execution_state")
        {
            String authError;
            if (!requireOwnerSession(requestId, request, authError))
                return authError;
            return makeActuatorExecutionStateResponse(requestId, request);
        }

        if (operation == "invoke_actuator")
        {
            String authError;
            if (!requireOwnerSession(requestId, request, authError))
                return authError;
            return makeInvokeActuatorResponse(requestId, request);
        }

        if (operation == "get_capabilities")
        {
            // Capability details are part of the authenticated controller state.
            // Unauthenticated setup uses get_device_info/get_setup_status and UDP discovery.
            String authError;
            if (!requireOwnerSession(requestId, request, authError))
                return authError;
            return makeCapabilitiesResponse(requestId);
        }

        if (operation == "get_capability")
        {
            String authError;
            if (!requireOwnerSession(requestId, request, authError))
                return authError;
            return makeGetCapabilityResponse(requestId, request);
        }

        if (operation == "get_sensor_history")
        {
            String authError;
            if (!requireOwnerSession(requestId, request, authError))
                return authError;
            return makeGetSensorHistoryResponse(requestId, request);
        }

        if (operation == "set_capability")
        {
            String authError;
            if (!requireOwnerSession(requestId, request, authError))
                return authError;
            return makeSetCapabilityResponse(requestId, request);
        }

        if (operation == "set_configuration")
        {
            if (!isConfigReady())
                return makeErrorResponse(requestId, "CONFIG_NOT_READY", "The configuration state is not initialized.");
            if (!isStorageReady())
                return makeErrorResponse(requestId, "STORAGE_NOT_READY", "Persistent storage is not initialized.");
            return makeSetConfigurationResponse(requestId, request);
        }

        if (operation == "get_configuration")
        {
            if (!isConfigReady())
                return makeErrorResponse(requestId, "CONFIG_NOT_READY", "The configuration state is not initialized.");
            if (!isStorageReady())
                return makeErrorResponse(requestId, "STORAGE_NOT_READY", "Persistent storage is not initialized.");
            return makeGetConfigurationResponse(requestId, request);
        }

        if (operation == "get_timestamp")
        {
            if (!isTimeModuleReady())
                return makeErrorResponse(requestId, "TIME_NOT_READY", "The time module is not initialized.");
            if (!isClockValid())
                return makeErrorResponse(requestId, "TIME_INVALID", "The controller clock has not been synchronized.");
            return makeTimestampResponse(requestId);
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
            if (!requireOwnerSession(requestId, request, authError))
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
