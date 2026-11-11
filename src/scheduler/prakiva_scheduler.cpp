#include "scheduler/prakiva_scheduler.h"

#include <ArduinoJson.h>
#include <math.h>
#include <string.h>
#include <stdint.h>

#include "core/prakiva_data_acquisition.h"
#include "core/prakiva_storage.h"
#include "core/prakiva_json.h"
#include "product/prakiva_product_runtime.h"
#include "scheduler/prakiva_curve_evaluators.h"
#include "scheduler/prakiva_policy_types.h"
#include "components/core/prakiva_actuator_driver.h"
#include "components/core/prakiva_virtual_actuator_driver.h"

namespace prakiva
{

Scheduler scheduler;

namespace
{
AnalogCurveEvaluator analogCurveEvaluator;
BinaryCurveEvaluator binaryCurveEvaluator;

// Keep the established evaluation cadence while integrating driver dispatch.
constexpr uint32_t SCHEDULER_DIAGNOSTIC_INTERVAL_MS = 5000;

// Trigger schedules are evaluated independently of the 5-second curve cadence.
constexpr const char *CALENDAR_POLICY = "trigger";

const char *policyName(JsonVariantConst value)
{
    return value.is<const char *>() ? value.as<const char *>() : "";
}

JsonObjectConst findCurve(JsonArrayConst curves, const char *channelId)
{
    for (JsonVariantConst item : curves)
    {
        if (!item.is<JsonObjectConst>())
            continue;
        JsonObjectConst curve = item.as<JsonObjectConst>();
        if (curve["id"] == channelId)
            return curve;
    }
    return JsonObjectConst();
}
} // namespace

bool Scheduler::init()
{
    if (_initialized)
        return true;

    if (!productRuntimeReady() || !productComponentsReady() || !productCapabilityResolverReady())
    {
        Serial.println("[SCHEDULER] Cannot initialise: product runtime/resolver is not ready");
        return false;
    }

    _evaluators.clear();
    if (!_evaluators.registerEvaluator(analogCurveEvaluator) ||
        !_evaluators.registerEvaluator(binaryCurveEvaluator))
    {
        Serial.println("[SCHEDULER] Failed to register curve policy evaluators");
        _evaluators.clear();
        return false;
    }

    _initialized = true;
    Serial.println("[SCHEDULER] Scheduler facade initialised");
    Serial.printf("[SCHEDULER] Curve evaluators registered: %u\n",
                  static_cast<unsigned>(_evaluators.count()));

    if (!reloadPolicies())
        Serial.println("[SCHEDULER] WARNING: policy configuration could not be loaded; no policies active");
    Serial.println("[SCHEDULER] Product-resolved actuator driver dispatch enabled (no direct GPIO writes)");
    return true;
}

bool Scheduler::reloadPolicies()
{
    if (!_initialized)
        return false;

    // Stop any previously controlled outputs before replacing policy state.
    // Physical PWM drivers initialize at zero; this also handles app reconfiguration.
    for (size_t i = 0; i < _activePolicyCount; ++i)
    {
        const RuntimePolicy &old = _policies[i];
        if (!old.used) continue;
        const auto resolved = productCapabilityResolver().resolve(
            old.capabilityId.c_str(), old.channelId.c_str());
        IActuatorDriver *driver = resolved.component ? resolved.component->asActuatorDriver() : nullptr;
        if (resolved.isOperational() && driver && old.policyType != CALENDAR_POLICY)
            driver->setState(false);
    }

    for (size_t i = 0; i < MAX_ACTIVE_POLICIES; ++i)
        _policies[i] = RuntimePolicy{};
    _activePolicyCount = 0;
    _runtimeStates.clear();
    _hasEvaluated = false; // Evaluate the newly loaded configuration on the next scheduler step.

    String configurationJson;
    uint32_t configurationTimestamp = 0;
    String errorCode, errorMessage;
    if (!loadConfigurationJson(configurationJson, configurationTimestamp, errorCode, errorMessage))
    {
        if (errorCode == "CONFIGURATION_NOT_FOUND")
        {
            Serial.println("[SCHEDULER] No saved controller configuration; no curve policies active");
            return true;
        }
        Serial.printf("[SCHEDULER] Configuration load failed: %s (%s)\n",
                      errorCode.c_str(), errorMessage.c_str());
        return false;
    }

    JsonDocument configurationDoc;
    DeserializationError parseError = deserializeJson(
        configurationDoc, configurationJson,
        DeserializationOption::NestingLimit(JSON_NESTING_LIMIT));
    if (parseError)
    {
        Serial.printf("[SCHEDULER] Configuration JSON invalid: %s (bytes=%u, nesting_limit=%u)\n",
                      parseError.c_str(), static_cast<unsigned>(configurationJson.length()),
                      JSON_NESTING_LIMIT);
        return false;
    }

    JsonObjectConst appConfiguration = configurationDoc["app_configuration"].as<JsonObjectConst>();
    JsonObjectConst controls = appConfiguration["controls"].as<JsonObjectConst>();
    if (controls.isNull())
    {
        Serial.println("[SCHEDULER] No app_configuration.controls document; no curve policies active");
        return true;
    }

    size_t configuredCurvePolicies = 0;
    size_t rejectedPolicies = 0;

    for (JsonPairConst controlPair : controls)
    {
        const char *capabilityId = controlPair.key().c_str();
        if (!controlPair.value().is<JsonObjectConst>())
            continue;
        JsonObjectConst control = controlPair.value().as<JsonObjectConst>();
        JsonObjectConst channelPolicies = control["channelPolicies"].as<JsonObjectConst>();
        JsonArrayConst curves = control["curves"].as<JsonArrayConst>();

        for (JsonPairConst policyPair : channelPolicies)
        {
            const char *channelId = policyPair.key().c_str();
            const char *type = policyName(policyPair.value());
            const bool analog = strcmp(type, POLICY_ANALOG_CURVE) == 0;
            const bool binary = strcmp(type, POLICY_BINARY_CURVE) == 0;
            const bool manualBinary = strcmp(type, POLICY_MANUAL_BINARY) == 0;
            const bool calendar = strcmp(type, CALENDAR_POLICY) == 0;
            if (!analog && !binary && !manualBinary && !calendar)
                continue; // Other policy evaluators are introduced in later phases.

            ++configuredCurvePolicies;
            if (_activePolicyCount >= MAX_ACTIVE_POLICIES)
            {
                ++rejectedPolicies;
                break;
            }

            const CapabilityChannelResolution resolution =
                productCapabilityResolver().resolve(capabilityId, channelId);
            if (!resolution.isOperational() || resolution.kind != "actuator" || !resolution.component)
            {
                ++rejectedPolicies;
                continue;
            }

            if (calendar && (resolution.declaredValueType != "trigger" ||
                             !resolution.component->asVirtualActuatorDriver()))
            {
                ++rejectedPolicies;
                continue;
            }

            double minimum = (binary || manualBinary || calendar) ? 0.0 : resolution.minimum;
            double maximum = (binary || manualBinary) ? 1.0 : resolution.maximum;
            if (analog && (!resolution.hasMinimum || !resolution.hasMaximum || maximum <= minimum))
            {
                ++rejectedPolicies;
                continue;
            }

            String policyJson;
            if (calendar)
            {
                JsonObjectConst inlineConfigurations = control["inlineConfigurations"].as<JsonObjectConst>();
                JsonObjectConst inlineConfiguration = inlineConfigurations[channelId].as<JsonObjectConst>();
                if (inlineConfiguration.isNull()) { ++rejectedPolicies; continue; }
                serializeJson(inlineConfiguration, policyJson);
            }
            else if (manualBinary)
            {
                // A missing manualOn is safely interpreted as OFF.
                JsonObjectConst inlineConfigurations =
                    control["inlineConfigurations"].as<JsonObjectConst>();
                JsonObjectConst inlineConfiguration =
                    inlineConfigurations[channelId].as<JsonObjectConst>();
                JsonDocument policyDocument;
                policyDocument["manualOn"] = inlineConfiguration["manualOn"] == true;
                serializeJson(policyDocument, policyJson);
            }
            else
            {
                // Analogue curves live in control.curves[]. Binary curves are
                // policy-inline and live in inlineConfigurations[channelId].curve.
                JsonObjectConst curve;
                if (analog)
                    curve = findCurve(curves, channelId);
                else
                {
                    JsonObjectConst inlineConfigurations =
                        control["inlineConfigurations"].as<JsonObjectConst>();
                    JsonObjectConst inlineConfiguration =
                        inlineConfigurations[channelId].as<JsonObjectConst>();
                    curve = inlineConfiguration["curve"].as<JsonObjectConst>();
                }
                if (curve.isNull() || !curve["points"].is<JsonArrayConst>())
                {
                    ++rejectedPolicies;
                    continue;
                }

                JsonDocument policyDocument;
                policyDocument["minimum"] = minimum;
                policyDocument["maximum"] = maximum;
                policyDocument["points"] = curve["points"];
                serializeJson(policyDocument, policyJson);
            }
            if (!policyJson.length())
            {
                ++rejectedPolicies;
                continue;
            }

            RuntimePolicy &runtime = _policies[_activePolicyCount++];
            runtime.capabilityId = capabilityId;
            runtime.channelId = channelId;
            runtime.policyType = type;
            runtime.policyDataJson = policyJson;
            runtime.minimum = minimum;
            runtime.maximum = maximum;
            runtime.used = true;
        }
    }

    // Keep routine policy inventory out of the bench-test console. The two
    // selected channels are reported once per diagnostic evaluation interval.
    (void)configuredCurvePolicies;
    (void)rejectedPolicies;
    (void)configurationTimestamp;
    return true;
}

// Runs on each observed minute transition. On boot, clock recovery, or a jump
// over one or more minutes, no event is replayed.
void Scheduler::evaluateCalendarTriggers()
{
    const SystemDateTimeValue &dt = currentSensorSnapshot().datetime;
    if (!dt.valid) { _hasObservedMinute = false; return; }
    const uint64_t minuteKey = dt.unixTimestamp / 60;
    if (!_hasObservedMinute) {
        _lastObservedMinute = minuteKey;
        _hasObservedMinute = true;
        return;
    }
    if (minuteKey == _lastObservedMinute) return;
    const bool consecutive = minuteKey == _lastObservedMinute + 1;
    _lastObservedMinute = minuteKey;
    if (!consecutive) return;

    static const char *days[] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
    for (size_t i = 0; i < _activePolicyCount; ++i) {
        const RuntimePolicy &policy = _policies[i];
        if (!policy.used || policy.policyType != CALENDAR_POLICY) continue;
        JsonDocument doc;
        if (deserializeJson(doc, policy.policyDataJson)) continue;
        if (doc["enabled"] != true || doc["source"] != "schedule") continue;
        JsonObjectConst schedule = doc["schedule"].as<JsonObjectConst>();
        if (schedule.isNull()) continue;
        const char *recurrence = schedule["recurrence"] | "";
        const char *time = schedule["time"] | "";
        int hh=-1, mm=-1;
        if (strlen(time) != 5 || time[2] != ':' ||
            time[0] < '0' || time[0] > '9' || time[1] < '0' || time[1] > '9' ||
            time[3] < '0' || time[3] > '9' || time[4] < '0' || time[4] > '9') continue;
        hh=(time[0]-'0')*10+time[1]-'0';
        mm=(time[3]-'0')*10+time[4]-'0';
        if (hh > 23 || mm > 59) continue;
        bool match=false;
        if (strcmp(recurrence,"hourly") == 0) match=dt.minute==mm;
        else if (dt.hour==hh && dt.minute==mm) {
            if (strcmp(recurrence,"daily") == 0) match=true;
            else if (strcmp(recurrence,"weekly") == 0 && dt.weekday>=1 && dt.weekday<=7) {
                JsonArrayConst weekdays=schedule["weekdays"].as<JsonArrayConst>();
                for (JsonVariantConst day:weekdays) if (day == days[dt.weekday-1]) { match=true; break; }
            } else if (strcmp(recurrence,"monthly") == 0) {
                const int day=schedule["dayOfMonth"] | 0;
                match=day>=1 && day<=31 && dt.day==day;
            }
        }
        if (!match) continue;
        const auto resolved=productCapabilityResolver().resolve(policy.capabilityId.c_str(),policy.channelId.c_str());
        auto *driver=resolved.isOperational() && resolved.component
            ? resolved.component->asVirtualActuatorDriver() : nullptr;
        if (!driver) continue;
        NumericValue state;
        if (!driver->get(state) || state.numericValue != "0") {
            Serial.printf("[CALENDAR] %s.%s | BUSY (skipped)\n",policy.capabilityId.c_str(),policy.channelId.c_str());
            continue;
        }
        const bool accepted=driver->setState(true);
        Serial.printf("[CALENDAR] %04d-%02d-%02d %02d:%02d | %s.%s | %s\n",
            dt.year,dt.month,dt.day,dt.hour,dt.minute,policy.capabilityId.c_str(),
            policy.channelId.c_str(),accepted?"FIRED":"REJECTED");
    }
}

void Scheduler::step()
{
    if (!_initialized) return;
    evaluateCalendarTriggers();
    if (_activePolicyCount == 0) return;

    // The main loop remains fast for data acquisition and networking. Policy
    // evaluation for this bench test is deliberately throttled to one pass
    // every five seconds.
    const uint32_t nowMs = static_cast<uint32_t>(millis());
    if (_hasEvaluated && static_cast<uint32_t>(nowMs - _lastEvaluationMs) <
                             SCHEDULER_DIAGNOSTIC_INTERVAL_MS)
        return;
    _lastEvaluationMs = nowMs;
    _hasEvaluated = true;

    const SensorSnapshot &snapshot = currentSensorSnapshot();
    const PolicyEvaluationContext context{snapshot, static_cast<uint64_t>(nowMs)};
    const SystemDateTimeValue &dt = snapshot.datetime;
    const bool timestampValid = dt.valid;

    for (size_t i = 0; i < _activePolicyCount; ++i)
    {
        RuntimePolicy &runtime = _policies[i];
        if (!runtime.used || runtime.policyType == CALENDAR_POLICY)
            continue;

        const CapabilityChannelResolution resolved = productCapabilityResolver().resolve(
            runtime.capabilityId.c_str(), runtime.channelId.c_str());
        IActuatorDriver *driver = resolved.component ? resolved.component->asActuatorDriver() : nullptr;
        if (!resolved.isOperational() || resolved.kind != "actuator" || !driver)
            continue;
        const String channelName = runtime.capabilityId + "." + runtime.channelId;

        const bool manualBinary = runtime.policyType == POLICY_MANUAL_BINARY;
        JsonDocument policyDocument;
        DeserializationError parseError = deserializeJson(
            policyDocument, runtime.policyDataJson,
            DeserializationOption::NestingLimit(JSON_NESTING_LIMIT));
        if (parseError)
        {
            Serial.printf("[SCHEDULER] %s | INVALID policy_json\n", channelName.c_str());
            runtime.hasEvaluationStatus = true;
            runtime.lastEvaluationValid = false;
            continue;
        }

        PolicyEvaluationResult result{};
        if (manualBinary)
        {
            result.valid = true;
            result.hasOutcome = true;
            result.outcome = policyDocument["manualOn"] == true ? 1.0f : 0.0f;
        }
        else
        {
            const IPolicyEvaluator *evaluator = _evaluators.find(runtime.policyType.c_str());
            if (!evaluator)
            {
                Serial.printf("[SCHEDULER] %s | evaluator_missing\n", channelName.c_str());
                continue;
            }
            const String stateKey = runtime.capabilityId + "." + runtime.channelId;
            EvaluatorState previousState;
            _runtimeStates.get(stateKey.c_str(), previousState);
            result = evaluator->evaluate(
                policyDocument.as<JsonVariantConst>(), context, previousState);
            if (!result.valid)
            {
                if (timestampValid)
                    Serial.printf("[SCHEDULER] %04d-%02d-%02d %02d:%02d:%02d | %s | INVALID time_or_curve\n",
                                  dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second, channelName.c_str());
                else
                    Serial.printf("[SCHEDULER] time_invalid | %s | INVALID time_or_curve\n", channelName.c_str());
                runtime.hasEvaluationStatus = true;
                runtime.lastEvaluationValid = false;
                continue;
            }
            _runtimeStates.set(stateKey.c_str(), result.nextState);
        }

        runtime.hasEvaluationStatus = true;
        runtime.lastEvaluationValid = true;
        const double normalized = result.outcome < 0.0f ? 0.0 :
            (result.outcome > 1.0f ? 1.0 : static_cast<double>(result.outcome));
        const double engineeringValue = runtime.minimum +
            normalized * (runtime.maximum - runtime.minimum);

        // Quantize to the actual driver command. Every channel is resolved from
        // product capability mappings; no GPIO number is present in the scheduler.
        const bool binary = runtime.policyType == POLICY_BINARY_CURVE || manualBinary;
        const uint32_t target = binary ? (normalized >= 0.5 ? 1u : 0u)
            : static_cast<uint32_t>(engineeringValue * 1000.0 + 0.5);
        if (runtime.hasLastTarget && runtime.lastTarget == target)
            continue;

        NumericValue command;
        command.numericValue = binary ? (target ? "1" : "0")
            : String(static_cast<double>(target) / 1000.0, 3);
        command.valueType = binary ? ValueType::UInt32 : ValueType::Float32;
        const bool applied = driver->set(command);
        if (applied)
        {
            runtime.lastTarget = target;
            runtime.hasLastTarget = true;
        }
        if (!applied)
            Serial.printf("[SCHEDULER] %s | DRIVER_REJECTED\n", channelName.c_str());
    }
}

bool Scheduler::ready() const { return _initialized; }
size_t Scheduler::activePolicyCount() const { return _activePolicyCount; }

} // namespace prakiva
