#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "core/prakiva_data_acquisition.h"

namespace prakiva
{

// Evaluator state is owned by Scheduler. Evaluators may interpret the bytes,
// but the scheduler never inspects their contents.
constexpr size_t MAX_EVALUATOR_STATE_BYTES = 64;

struct EvaluatorState
{
    uint16_t version = 0;
    uint16_t size = 0;
    alignas(uint32_t) uint8_t bytes[MAX_EVALUATOR_STATE_BYTES] = {};

    void clear()
    {
        version = 0;
        size = 0;
        memset(bytes, 0, sizeof(bytes));
    }
};

struct PolicyEvaluationResult
{
    // valid=false means evaluation could not be completed. It is not an OFF.
    bool valid = false;

    // hasOutcome=false means no new command/value should be dispatched.
    // This is distinct from a valid outcome of 0.0.
    bool hasOutcome = false;

    // Normalised actuator value, inclusive range [0.0, 1.0].
    float outcome = 0.0f;

    // Complete replacement state for the next evaluation.
    EvaluatorState nextState;
};

struct PolicyEvaluationContext
{
    const SensorSnapshot &inputs;
    uint64_t monotonicMilliseconds = 0;
};

class IPolicyEvaluator
{
public:
    virtual const char *policyType() const = 0;

    virtual PolicyEvaluationResult evaluate(
        JsonVariantConst policyData,
        const PolicyEvaluationContext &context,
        const EvaluatorState &previousState) const = 0;

    virtual ~IPolicyEvaluator() = default;
};

} // namespace prakiva
