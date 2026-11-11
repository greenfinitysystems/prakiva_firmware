#include <string.h>
#include "scheduler/prakiva_evaluator_registry.h"

namespace prakiva
{

bool PolicyEvaluatorRegistry::registerEvaluator(const IPolicyEvaluator &evaluator)
{
    const char *type = evaluator.policyType();
    if (!type || !type[0] || _count >= MAX_POLICY_EVALUATORS)
        return false;

    if (find(type))
        return false;

    _evaluators[_count++] = &evaluator;
    return true;
}

const IPolicyEvaluator *PolicyEvaluatorRegistry::find(const char *policyType) const
{
    if (!policyType || !policyType[0])
        return nullptr;

    for (size_t i = 0; i < _count; ++i)
    {
        const char *registeredType = _evaluators[i]->policyType();
        if (registeredType && strcmp(registeredType, policyType) == 0)
            return _evaluators[i];
    }
    return nullptr;
}

size_t PolicyEvaluatorRegistry::count() const { return _count; }

void PolicyEvaluatorRegistry::clear()
{
    for (size_t i = 0; i < MAX_POLICY_EVALUATORS; ++i)
        _evaluators[i] = nullptr;
    _count = 0;
}

} // namespace prakiva
