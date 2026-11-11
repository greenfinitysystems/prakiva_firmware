#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include "scheduler/prakiva_evaluator_registry.h"
#include "scheduler/prakiva_policy_state_store.h"

namespace prakiva
{

// Policy evaluation and dispatch through product-resolved actuator drivers.
class Scheduler
{
public:
    bool init();
    bool reloadPolicies();
    void step();
    bool ready() const;
    size_t activePolicyCount() const;

private:
    struct RuntimePolicy
    {
        String capabilityId;
        String channelId;
        String policyType;
        String policyDataJson;
        double minimum = 0.0;
        double maximum = 1.0;
        bool lastEvaluationValid = false;
        bool hasEvaluationStatus = false;
        bool used = false;
        bool hasLastTarget = false;
        uint32_t lastTarget = 0;
    };

    static constexpr size_t MAX_ACTIVE_POLICIES = 64;
    bool _initialized = false;
    size_t _activePolicyCount = 0;
    RuntimePolicy _policies[MAX_ACTIVE_POLICIES];
    PolicyEvaluatorRegistry _evaluators;
    PolicyRuntimeStateStore _runtimeStates;
    uint32_t _lastEvaluationMs = 0;
    bool _hasEvaluated = false;
    bool _hasObservedMinute = false;
    uint64_t _lastObservedMinute = 0;
    void evaluateCalendarTriggers();
};

extern Scheduler scheduler;

} // namespace prakiva
