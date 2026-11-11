#include "scheduler/prakiva_curve_evaluators.h"

#include <math.h>
#include <string.h>

#include "scheduler/prakiva_policy_types.h"

namespace prakiva
{
namespace
{
constexpr size_t MAX_CURVE_POINTS = 64;
constexpr float OUTPUT_EPSILON = 0.0005f;
constexpr double DAY_HOURS = 24.0;

struct CurvePoint
{
    double x;
    double y;
};

struct CurveRuntimeState
{
    uint8_t hasLast = 0;
    float lastOutput = 0.0f;
};

bool readCurve(JsonVariantConst data, CurvePoint *points, size_t &count,
               double &minimum, double &maximum)
{
    count = 0;
    if (!data.is<JsonObjectConst>())
        return false;

    JsonObjectConst object = data.as<JsonObjectConst>();
    JsonArrayConst array = object["points"].as<JsonArrayConst>();
    if (array.isNull() || array.size() < 2 || array.size() > MAX_CURVE_POINTS)
        return false;

    if (!object["minimum"].is<double>() || !object["maximum"].is<double>())
        return false;
    minimum = object["minimum"].as<double>();
    maximum = object["maximum"].as<double>();
    if (!isfinite(minimum) || !isfinite(maximum) || maximum <= minimum)
        return false;

    for (JsonVariantConst item : array)
    {
        if (!item.is<JsonObjectConst>())
            return false;
        JsonObjectConst point = item.as<JsonObjectConst>();
        if (!point["x"].is<double>() || !point["y"].is<double>())
            return false;
        const double x = point["x"].as<double>();
        const double y = point["y"].as<double>();
        if (!isfinite(x) || !isfinite(y) || x < 0.0 || x > DAY_HOURS || y < minimum || y > maximum)
            return false;
        points[count++] = {x, y};
    }

    // Insertion sort is deterministic and avoids heap allocation on the MCU.
    for (size_t i = 1; i < count; ++i)
    {
        CurvePoint value = points[i];
        size_t j = i;
        while (j > 0 && points[j - 1].x > value.x)
        {
            points[j] = points[j - 1];
            --j;
        }
        points[j] = value;
    }
    for (size_t i = 1; i < count; ++i)
        if (points[i].x - points[i - 1].x < 0.000001)
            return false;
    return true;
}

double clamp01(double value)
{
    if (value < 0.0) return 0.0;
    if (value > 1.0) return 1.0;
    return value;
}

double hermite(double y0, double y1, double m0, double m1, double h, double t)
{
    const double t2 = t * t;
    const double t3 = t2 * t;
    return (2.0 * t3 - 3.0 * t2 + 1.0) * y0 +
           (t3 - 2.0 * t2 + t) * h * m0 +
           (-2.0 * t3 + 3.0 * t2) * y1 +
           (t3 - t2) * h * m1;
}

// Mirrors CurveEditor.periodicMonotonePath(): shared midnight value, periodic
// neighbouring-day points, monotone tangent limiting, and cubic Hermite output.
bool evaluatePeriodicAnalog(const CurvePoint *source, size_t sourceCount,
                            double x, double &value)
{
    if (sourceCount < 2)
        return false;

    CurvePoint core[MAX_CURVE_POINTS + 2];
    size_t coreCount = 0;
    const double firstY = source[0].x <= 0.000001 ? source[0].y : source[0].y;
    const double lastY = source[sourceCount - 1].x >= DAY_HOURS - 0.000001
                             ? source[sourceCount - 1].y : firstY;
    const double sharedY = (firstY + lastY) / 2.0;
    core[coreCount++] = {0.0, sharedY};
    for (size_t i = 0; i < sourceCount; ++i)
        if (source[i].x > 0.000001 && source[i].x < DAY_HOURS - 0.000001)
            core[coreCount++] = source[i];
    core[coreCount++] = {DAY_HOURS, sharedY};

    CurvePoint extended[(MAX_CURVE_POINTS + 2) * 3];
    size_t n = 0;
    for (size_t i = 0; i + 1 < coreCount; ++i)
        extended[n++] = {core[i].x - DAY_HOURS, core[i].y};
    for (size_t i = 0; i < coreCount; ++i)
        extended[n++] = core[i];
    for (size_t i = 1; i < coreCount; ++i)
        extended[n++] = {core[i].x + DAY_HOURS, core[i].y};

    double secant[(MAX_CURVE_POINTS + 2) * 3] = {};
    double tangent[(MAX_CURVE_POINTS + 2) * 3] = {};
    for (size_t i = 0; i + 1 < n; ++i)
    {
        const double dx = extended[i + 1].x - extended[i].x;
        secant[i] = dx == 0.0 ? 0.0 : (extended[i + 1].y - extended[i].y) / dx;
    }
    tangent[0] = secant[0];
    tangent[n - 1] = secant[n - 2];
    for (size_t i = 1; i + 1 < n; ++i)
    {
        const double before = secant[i - 1];
        const double after = secant[i];
        if (before == 0.0 || after == 0.0 || ((before < 0.0) != (after < 0.0)))
            tangent[i] = 0.0;
        else
            tangent[i] = (before + after) / 2.0;
    }
    for (size_t i = 0; i + 1 < n; ++i)
    {
        if (secant[i] == 0.0)
        {
            tangent[i] = tangent[i + 1] = 0.0;
        }
        else
        {
            const double a = tangent[i] / secant[i];
            const double b = tangent[i + 1] / secant[i];
            const double norm = a * a + b * b;
            if (norm > 9.0)
            {
                const double tau = 3.0 / sqrt(norm);
                tangent[i] = tau * a * secant[i];
                tangent[i + 1] = tau * b * secant[i];
            }
        }
    }

    for (size_t i = 0; i + 1 < n; ++i)
    {
        const double x0 = extended[i].x;
        const double x1 = extended[i + 1].x;
        if (x <= x0 || x > x1)
            continue;
        const double h = x1 - x0;
        value = hermite(extended[i].y, extended[i + 1].y,
                        tangent[i], tangent[i + 1], h, (x - x0) / h);
        return isfinite(value);
    }
    value = sharedY;
    return true;
}

bool evaluateBinaryStep(const CurvePoint *points, size_t count, double x, double &value)
{
    if (count < 2)
        return false;
    // The editor's binary curve is a step function: the latest point at or
    // before the current time determines the state. The day wraps to point 0.
    value = points[count - 1].y;
    for (size_t i = 0; i < count; ++i)
    {
        if (points[i].x <= x)
            value = points[i].y;
        else
            break;
    }
    return true;
}

PolicyEvaluationResult makeResult(bool valid, float output,
                                  const EvaluatorState &previousState)
{
    PolicyEvaluationResult result;
    result.valid = valid;
    result.nextState = previousState;
    if (!valid)
        return result;

    CurveRuntimeState state{};
    if (previousState.version == 1 && previousState.size == sizeof(CurveRuntimeState))
        memcpy(&state, previousState.bytes, sizeof(state));

    if (!state.hasLast || fabsf(output - state.lastOutput) > OUTPUT_EPSILON)
        result.hasOutcome = true;
    result.outcome = output;
    state.hasLast = 1;
    state.lastOutput = output;
    result.nextState.clear();
    result.nextState.version = 1;
    result.nextState.size = sizeof(CurveRuntimeState);
    memcpy(result.nextState.bytes, &state, sizeof(state));
    return result;
}

bool getTimeOfDay(const PolicyEvaluationContext &context, double &hours)
{
    const SystemDateTimeValue &dt = context.inputs.datetime;
    if (!dt.valid || dt.hour < 0 || dt.hour > 23 || dt.minute < 0 || dt.minute > 59 ||
        dt.second < 0 || dt.second > 59)
        return false;
    hours = static_cast<double>(dt.hour) + static_cast<double>(dt.minute) / 60.0 +
            static_cast<double>(dt.second) / 3600.0;
    return true;
}
} // namespace

const char *AnalogCurveEvaluator::policyType() const { return POLICY_ANALOG_CURVE; }

PolicyEvaluationResult AnalogCurveEvaluator::evaluate(
    JsonVariantConst policyData, const PolicyEvaluationContext &context,
    const EvaluatorState &previousState) const
{
    CurvePoint points[MAX_CURVE_POINTS];
    size_t count = 0;
    double minimum = 0.0, maximum = 0.0, x = 0.0, value = 0.0;
    if (!readCurve(policyData, points, count, minimum, maximum) ||
        !getTimeOfDay(context, x) ||
        !evaluatePeriodicAnalog(points, count, x, value))
        return makeResult(false, 0.0f, previousState);
    const float normalized = static_cast<float>(clamp01((value - minimum) / (maximum - minimum)));
    return makeResult(true, normalized, previousState);
}

const char *BinaryCurveEvaluator::policyType() const { return POLICY_BINARY_CURVE; }

PolicyEvaluationResult BinaryCurveEvaluator::evaluate(
    JsonVariantConst policyData, const PolicyEvaluationContext &context,
    const EvaluatorState &previousState) const
{
    CurvePoint points[MAX_CURVE_POINTS];
    size_t count = 0;
    double minimum = 0.0, maximum = 0.0, x = 0.0, value = 0.0;
    if (!readCurve(policyData, points, count, minimum, maximum) ||
        !getTimeOfDay(context, x) ||
        !evaluateBinaryStep(points, count, x, value))
        return makeResult(false, 0.0f, previousState);
    const float normalized = value >= (minimum + maximum) / 2.0 ? 1.0f : 0.0f;
    return makeResult(true, normalized, previousState);
}

} // namespace prakiva
