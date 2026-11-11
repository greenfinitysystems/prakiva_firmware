#pragma once

#include <stddef.h>
#include "scheduler/prakiva_policy_evaluator.h"

namespace prakiva
{

constexpr size_t MAX_POLICY_EVALUATORS = 6;

class PolicyEvaluatorRegistry
{
public:
    bool registerEvaluator(const IPolicyEvaluator &evaluator);
    const IPolicyEvaluator *find(const char *policyType) const;
    size_t count() const;
    void clear();

private:
    const IPolicyEvaluator *_evaluators[MAX_POLICY_EVALUATORS] = {};
    size_t _count = 0;
};

} // namespace prakiva
