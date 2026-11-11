#include <ArduinoJson.h>

#include "product/prakiva_capability_resolver.h"

namespace prakiva
{

bool CapabilityResolver::initialize(
    const String &capabilitiesJson,
    const String &runtimeManifestJson,
    ComponentRegistry &registry)
{
    _count = 0;
    _mappedCount = 0;
    _ready = false;

    JsonDocument capabilitiesDoc;
    JsonDocument manifestDoc;

    if (deserializeJson(capabilitiesDoc, capabilitiesJson) ||
        deserializeJson(manifestDoc, runtimeManifestJson))
        return false;

    JsonArrayConst capabilities =
        capabilitiesDoc["capabilities"].as<JsonArrayConst>();
    JsonArrayConst components =
        manifestDoc["components"].as<JsonArrayConst>();
    JsonArrayConst mappings =
        manifestDoc["mappings"].as<JsonArrayConst>();

    if (capabilities.isNull() || components.isNull() || mappings.isNull())
        return false;

    // Index every declared public channel, including channels that do not
    // currently have a hardware/component mapping.
    for (JsonVariantConst capability : capabilities)
    {
        const char *capabilityId = capability["id"] | "";
        const char *kind = capability["kind"] | "";
        JsonArrayConst channels = capability["channels"].as<JsonArrayConst>();

        if (!capabilityId[0] || !kind[0] || channels.isNull())
            return false;

        for (JsonVariantConst channel : channels)
        {
            const char *channelId = channel["id"] | "";
            if (!channelId[0] || _count >= MAX_CHANNELS)
                return false;

            for (size_t i = 0; i < _count; ++i)
            {
                if (_entries[i].capabilityId == capabilityId &&
                    _entries[i].channelId == channelId)
                    return false;
            }

            Entry &entry = _entries[_count++];
            entry.capabilityId = capabilityId;
            entry.channelId = channelId;
            entry.kind = kind;
            entry.declaredValueType = channel["valueType"] | "";
            if (channel["min"].is<double>())
            {
                entry.hasMinimum = true;
                entry.minimum = channel["min"].as<double>();
            }
            if (channel["max"].is<double>())
            {
                entry.hasMaximum = true;
                entry.maximum = channel["max"].as<double>();
            }
        }
    }

    // Resolve private mappings to registry-owned component instances.
    for (JsonVariantConst mapping : mappings)
    {
        const char *capabilityId = mapping["capability_id"] | "";
        const char *channelId = mapping["channel_id"] | "";
        const char *componentId = mapping["component_id"] | "";
        const char *endpoint = mapping["endpoint"] | "";

        if (!capabilityId[0] || !channelId[0] ||
            !componentId[0] || !endpoint[0])
            return false;

        Entry *target = nullptr;
        for (size_t i = 0; i < _count; ++i)
        {
            if (_entries[i].capabilityId == capabilityId &&
                _entries[i].channelId == channelId)
            {
                target = &_entries[i];
                break;
            }
        }

        if (!target || target->mapped)
            return false;

        // The endpoint contract must agree with the declared capability kind.
        if ((target->kind == "sensor" && String(endpoint) != "get") ||
            (target->kind == "actuator" && String(endpoint) != "get_set") ||
            (target->kind != "sensor" && target->kind != "actuator"))
            return false;

        bool componentDeclared = false;
        for (JsonVariantConst component : components)
        {
            if (String(component["id"] | "") == componentId)
            {
                componentDeclared = true;
                break;
            }
        }
        if (!componentDeclared)
            return false;

        IComponent *instance = registry.find(componentId);
        if (!instance)
            return false;

        target->componentId = componentId;
        target->endpoint = endpoint;
        target->component = instance;
        target->mapped = true;
        ++_mappedCount;
    }

    _ready = true;
    return true;
}

CapabilityChannelResolution CapabilityResolver::resolve(
    const char *capabilityId,
    const char *channelId) const
{
    CapabilityChannelResolution result;
    if (!_ready || !capabilityId || !capabilityId[0] ||
        !channelId || !channelId[0])
        return result;

    for (size_t i = 0; i < _count; ++i)
    {
        const Entry &entry = _entries[i];
        if (entry.capabilityId != capabilityId ||
            entry.channelId != channelId)
            continue;

        result.capabilityId = entry.capabilityId;
        result.channelId = entry.channelId;
        result.kind = entry.kind;
        result.declaredValueType = entry.declaredValueType;
        result.hasMinimum = entry.hasMinimum;
        result.hasMaximum = entry.hasMaximum;
        result.minimum = entry.minimum;
        result.maximum = entry.maximum;

        if (!entry.mapped)
        {
            result.status = CapabilityResolutionStatus::SupportedOnly;
            return result;
        }

        result.endpoint = entry.endpoint;
        result.componentId = entry.componentId;
        result.component = entry.component;

        result.status = entry.component &&
                                entry.component->status() == ComponentStatus::Ready
                            ? CapabilityResolutionStatus::Operational
                            : CapabilityResolutionStatus::ComponentNotReady;
        return result;
    }

    return result;
}

bool CapabilityResolver::ready() const
{
    return _ready;
}

size_t CapabilityResolver::channelCount() const
{
    return _count;
}

size_t CapabilityResolver::mappedChannelCount() const
{
    return _mappedCount;
}

} // namespace prakiva
