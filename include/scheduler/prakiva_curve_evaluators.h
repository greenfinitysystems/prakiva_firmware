#pragma once

#include "scheduler/prakiva_policy_evaluator.h"

namespace prakiva
{

class AnalogCurveEvaluator final : public IPolicyEvaluator
{
public:
    const char *policyType() const override;
    PolicyEvaluationResult evaluate(JsonVariantConst policyData,
                                    const PolicyEvaluationContext &context,
                                    const EvaluatorState &previousState) const override;
};

class BinaryCurveEvaluator final : public IPolicyEvaluator
{
public:
    const char *policyType() const override;
    PolicyEvaluationResult evaluate(JsonVariantConst policyData,
                                    const PolicyEvaluationContext &context,
                                    const EvaluatorState &previousState) const override;
};

} // namespace prakiva
