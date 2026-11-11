#pragma once

#include <Arduino.h>
#include <stddef.h>
#include "scheduler/prakiva_policy_evaluator.h"

namespace prakiva
{

constexpr size_t MAX_POLICY_RUNTIME_STATES = 64;

// Fixed-capacity store avoids per-step allocation and keeps evaluator state
// ownership entirely inside Scheduler. The state payload remains opaque.
class PolicyRuntimeStateStore
{
public:
    bool get(const char *policyInstanceId, EvaluatorState &state) const;
    bool set(const char *policyInstanceId, const EvaluatorState &state);
    bool erase(const char *policyInstanceId);
    void clear();
    size_t count() const;

private:
    struct Entry
    {
        String policyInstanceId;
        EvaluatorState state;
        bool used = false;
    };

    Entry _entries[MAX_POLICY_RUNTIME_STATES];
};

} // namespace prakiva
