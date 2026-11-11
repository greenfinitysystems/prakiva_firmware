#pragma once

#include <Arduino.h>
#include <stddef.h>

#include "components/core/prakiva_component_registry.h"

namespace prakiva
{

enum class CapabilityResolutionStatus : uint8_t
{
    NotFound = 0,
    SupportedOnly,
    ComponentNotReady,
    Operational
};

struct CapabilityChannelResolution
{
    CapabilityResolutionStatus status = CapabilityResolutionStatus::NotFound;
    String capabilityId;
    String channelId;
    String kind;
    String endpoint;
    String componentId;
    String declaredValueType;
    bool hasMinimum = false;
    bool hasMaximum = false;
    double minimum = 0.0;
    double maximum = 0.0;
    IComponent *component = nullptr;

    bool isSupported() const
    {
        return status != CapabilityResolutionStatus::NotFound;
    }

    bool isOperational() const
    {
        return status == CapabilityResolutionStatus::Operational;
    }
};

// Builds a fixed-size in-memory index once during boot. It contains only
// application-facing identifiers and private runtime pointers; it is never
// serialized or exposed through the protocol.
class CapabilityResolver
{
public:
    static constexpr size_t MAX_CHANNELS = 64;

    bool initialize(
        const String &capabilitiesJson,
        const String &runtimeManifestJson,
        ComponentRegistry &registry);

    CapabilityChannelResolution resolve(
        const char *capabilityId,
        const char *channelId) const;

    bool ready() const;
    size_t channelCount() const;
    size_t mappedChannelCount() const;

private:
    struct Entry
    {
        String capabilityId;
        String channelId;
        String kind;
        String endpoint;
        String componentId;
        String declaredValueType;
        bool hasMinimum = false;
        bool hasMaximum = false;
        double minimum = 0.0;
        double maximum = 0.0;
        IComponent *component = nullptr;
        bool mapped = false;
    };

    Entry _entries[MAX_CHANNELS];
    size_t _count = 0;
    size_t _mappedCount = 0;
    bool _ready = false;
};

} // namespace prakiva
