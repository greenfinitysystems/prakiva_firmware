#include "scheduler/prakiva_policy_state_store.h"

namespace prakiva
{

bool PolicyRuntimeStateStore::get(const char *policyInstanceId, EvaluatorState &state) const
{
    if (!policyInstanceId || !policyInstanceId[0])
        return false;

    for (const Entry &entry : _entries)
    {
        if (entry.used && entry.policyInstanceId == policyInstanceId)
        {
            state = entry.state;
            return true;
        }
    }
    return false;
}

bool PolicyRuntimeStateStore::set(const char *policyInstanceId, const EvaluatorState &state)
{
    if (!policyInstanceId || !policyInstanceId[0] || state.size > MAX_EVALUATOR_STATE_BYTES)
        return false;

    Entry *freeEntry = nullptr;
    for (Entry &entry : _entries)
    {
        if (entry.used && entry.policyInstanceId == policyInstanceId)
        {
            entry.state = state;
            return true;
        }
        if (!entry.used && !freeEntry)
            freeEntry = &entry;
    }

    if (!freeEntry)
        return false;

    freeEntry->policyInstanceId = policyInstanceId;
    freeEntry->state = state;
    freeEntry->used = true;
    return true;
}

bool PolicyRuntimeStateStore::erase(const char *policyInstanceId)
{
    if (!policyInstanceId || !policyInstanceId[0])
        return false;

    for (Entry &entry : _entries)
    {
        if (entry.used && entry.policyInstanceId == policyInstanceId)
        {
            entry.policyInstanceId = "";
            entry.state.clear();
            entry.used = false;
            return true;
        }
    }
    return false;
}

void PolicyRuntimeStateStore::clear()
{
    for (Entry &entry : _entries)
    {
        entry.policyInstanceId = "";
        entry.state.clear();
        entry.used = false;
    }
}

size_t PolicyRuntimeStateStore::count() const
{
    size_t result = 0;
    for (const Entry &entry : _entries)
        if (entry.used)
            ++result;
    return result;
}

} // namespace prakiva
