#include <ArduinoJson.h>
#include <cmath>

#include "product/prakiva_product_spec_processor.h"

namespace prakiva
{
namespace
{
ProductSpecResult fail(ProductSpecError error, const String &productId = String(), size_t created = 0)
{
    ProductSpecResult result;
    result.error = error;
    result.productId = productId;
    result.componentsCreated = created;
    return result;
}

bool hasId(JsonArrayConst entries, const char *id)
{
    for (JsonVariantConst entry : entries)
    {
        if (String(entry["id"] | "") == id)
            return true;
    }
    return false;
}

bool hasCapability(JsonArrayConst capabilities, const char *id)
{
    return hasId(capabilities, id);
}

bool hasComponent(JsonArrayConst components, const char *id)
{
    return hasId(components, id);
}

bool hasResource(JsonArrayConst resources, const char *id)
{
    return hasId(resources, id);
}

uint32_t specificationChecksum(const char *data)
{
    uint32_t hash = 2166136261u;
    if (!data)
        return hash;
    while (*data)
    {
        hash ^= static_cast<uint8_t>(*data++);
        hash *= 16777619u;
    }
    return hash;
}

bool validateVirtualSensorParameterSchema(JsonObjectConst parameters)
{
    for (JsonPairConst parameter : parameters)
    {
        const char *id = parameter.key().c_str();
        JsonObjectConst definition = parameter.value().as<JsonObjectConst>();
        if (!id || !id[0] || definition.isNull() ||
            String(definition["type"] | "") != "number" ||
            !definition["label"].is<const char *>() ||
            !definition["default"].is<float>())
            return false;

        const double defaultValue = definition["default"].as<double>();
        if (!std::isfinite(defaultValue))
            return false;

        const bool hasMinimum = definition["minimum"].is<float>();
        const bool hasMaximum = definition["maximum"].is<float>();
        if (hasMinimum && !std::isfinite(definition["minimum"].as<double>()))
            return false;
        if (hasMaximum && !std::isfinite(definition["maximum"].as<double>()))
            return false;
        if (hasMinimum && hasMaximum &&
            definition["minimum"].as<double>() > definition["maximum"].as<double>())
            return false;
        if ((hasMinimum && defaultValue < definition["minimum"].as<double>()) ||
            (hasMaximum && defaultValue > definition["maximum"].as<double>()))
            return false;
        if (definition["step"].isUnbound() &&
            (!definition["step"].is<float>() ||
             !std::isfinite(definition["step"].as<double>()) ||
             definition["step"].as<double>() <= 0.0))
            return false;
    }
    return true;
}

bool validateUniqueIds(JsonArrayConst entries)
{
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const char *id = entries[i]["id"] | "";
        if (!id[0])
            return false;
        for (size_t j = i + 1; j < entries.size(); ++j)
        {
            if (String(id) == (entries[j]["id"] | ""))
                return false;
        }
    }
    return true;
}

} // namespace

ProductSpecResult processProductSpecification(
    const char *specificationJson,
    ComponentRegistry &registry,
    String &capabilitiesJson,
    String &runtimeManifestJson,
    String &configurationSchemaJson,
    String &metadataJson)
{
    capabilitiesJson = "";
    runtimeManifestJson = "";
    configurationSchemaJson = "";
    metadataJson = "";

    if (!specificationJson || !specificationJson[0])
        return fail(ProductSpecError::InvalidJson);

    JsonDocument source;
    if (deserializeJson(source, specificationJson) || !source.is<JsonObject>())
        return fail(ProductSpecError::InvalidJson);

    JsonObjectConst spec = source["specification"].as<JsonObjectConst>();
    const char *productId = spec["product_id"] | "";
    if (!spec || !productId[0])
        return fail(ProductSpecError::MissingProductId);

    JsonArrayConst resources = source["resources"].as<JsonArrayConst>();
    JsonArrayConst components = source["components"].as<JsonArrayConst>();
    JsonArrayConst capabilities = source["capabilities"].as<JsonArrayConst>();
    JsonArrayConst mappings = source["mappings"].as<JsonArrayConst>();
    if (resources.isNull() || components.isNull() || capabilities.isNull() || mappings.isNull())
        return fail(ProductSpecError::MissingArray, productId);

    if (!validateUniqueIds(resources) || !validateUniqueIds(components) ||
        !validateUniqueIds(capabilities))
        return fail(ProductSpecError::DuplicateId, productId);

    if (!source["history_policy"].isUnbound() && !source["history_policy"].isNull())
    {
        JsonObjectConst historyPolicy = source["history_policy"].as<JsonObjectConst>();
        if (historyPolicy.isNull() || !historyPolicy["enabled"].is<bool>() ||
            !historyPolicy["aggregation_window_seconds"].is<uint32_t>() ||
            !historyPolicy["retention_points"].is<uint16_t>() ||
            historyPolicy["aggregation_window_seconds"].as<uint32_t>() < 2 ||
            historyPolicy["aggregation_window_seconds"].as<uint32_t>() > 86400 ||
            historyPolicy["retention_points"].as<uint16_t>() == 0 ||
            historyPolicy["retention_points"].as<uint16_t>() > 288)
            return fail(ProductSpecError::InvalidHistoryPolicy, productId);
    }

    if (components.size() == 0 || capabilities.size() == 0 ||
        components.size() > MAX_COMPONENT_INSTANCES ||
        registry.instanceCount() + components.size() > MAX_COMPONENT_INSTANCES)
        return fail(ProductSpecError::RegistryCapacityExceeded, productId);

    // Public capability contract validation. Sensor min/max values define the
    // nominal display scale for gauges/bars; they do not clamp measurements.
    for (JsonVariantConst capability : capabilities)
    {
        const char *kind = capability["kind"] | "";
        JsonArrayConst channels = capability["channels"].as<JsonArrayConst>();
        if (!kind[0] || channels.isNull() || channels.size() == 0)
            return fail(ProductSpecError::InvalidEntry, productId);

        for (JsonVariantConst channel : channels)
        {
            const char *valueType = channel["valueType"] | "";
            if (!valueType[0])
                return fail(ProductSpecError::InvalidEntry, productId);

            const bool needsRange = String(kind) == "sensor" ||
                                    (String(kind) == "actuator" && String(valueType) == "analog");
            if (needsRange)
            {
                if (!channel["min"].is<float>() || !channel["max"].is<float>() ||
                    channel["min"].as<double>() >= channel["max"].as<double>())
                    return fail(ProductSpecError::InvalidEntry, productId);
            }
        }
    }

    // Validate resources and driver declarations before creating anything.
    for (JsonVariantConst resource : resources)
    {
        const char *id = resource["id"] | "";
        const char *type = resource["type"] | "";
        if (!id[0] || !type[0])
            return fail(ProductSpecError::InvalidEntry, productId);
        if (String(type) == "gpio")
        {
            if (!resource["gpio"].is<int>() || resource["gpio"].as<int>() < 0 ||
                resource["gpio"].as<int>() > 48)
                return fail(ProductSpecError::InvalidEntry, productId);
        }
    }

    for (JsonVariantConst component : components)
    {
        const char *id = component["id"] | "";
        const char *driver = component["driver"] | "";
        if (!id[0] || !driver[0])
            return fail(ProductSpecError::InvalidEntry, productId);
        if (registry.hasComponent(id))
            return fail(ProductSpecError::DuplicateId, productId);
        if (!registry.hasFactory(driver))
            return fail(ProductSpecError::UnknownDriver, productId);

        JsonObjectConst componentResources = component["resources"].as<JsonObjectConst>();
        for (JsonPairConst resourceRef : componentResources)
        {
            const char *resourceId = resourceRef.value().as<const char *>();
            if (!resourceId || !hasResource(resources, resourceId))
                return fail(ProductSpecError::UnknownResource, productId);
        }
    }

    // Virtual sensors and actuators declare private component dependencies.
    // Binding and dependency-ordered initialization are performed by runtime.
    for (JsonVariantConst component : components)
    {
        const char *componentId = component["id"] | "";
        const char *componentType = component["component_type"] | "";
        const bool hasInputs = !component["inputs"].isNull();
        const bool inputsAreObject = component["inputs"].is<JsonObjectConst>();

        if (componentType[0] &&
            String(componentType) != "physical_sensor" &&
            String(componentType) != "virtual_sensor" &&
            String(componentType) != "virtual_actuator" &&
            String(componentType) != "physical_actuator" &&
            String(componentType) != "actuator" &&
            String(componentType) != "service")
            return fail(ProductSpecError::InvalidVirtualSensorDeclaration, productId);

        // A virtual sensor may declare a fixed output unit; never treat it as a numeric parameter.
        if (String(componentType) == "virtual_sensor" &&
            !component["unit"].isUnbound() &&
            !component["unit"].is<const char *>())
            return fail(ProductSpecError::InvalidVirtualSensorDeclaration, productId);

        const bool hasParameters = !component["parameters"].isNull();
        if (String(componentType) == "virtual_sensor" || String(componentType) == "virtual_actuator")
        {
            JsonObjectConst parameters = component["parameters"].as<JsonObjectConst>();
            if (hasParameters &&
                (!component["parameters"].is<JsonObjectConst>() ||
                 !validateVirtualSensorParameterSchema(parameters)))
                return fail(ProductSpecError::InvalidVirtualSensorDeclaration, productId);

            JsonObjectConst inputs = component["inputs"].as<JsonObjectConst>();
            JsonObjectConst actuatorDependencies = component["actuators"].as<JsonObjectConst>();
            if (String(componentType) == "virtual_sensor" && (!hasInputs || !inputsAreObject))
                return fail(ProductSpecError::InvalidVirtualSensorDeclaration, productId);
            if (String(componentType) == "virtual_actuator" && !component["actuators"].is<JsonObjectConst>())
                return fail(ProductSpecError::InvalidVirtualSensorDeclaration, productId);

            for (JsonPairConst input : inputs)
            {
                const char *inputId = input.key().c_str();
                const char *sourceComponentId = input.value().as<const char *>();
                if (!inputId || !inputId[0] || !sourceComponentId || !sourceComponentId[0])
                    return fail(ProductSpecError::InvalidVirtualSensorDeclaration, productId);
                if (String(sourceComponentId) == componentId ||
                    !hasComponent(components, sourceComponentId))
                    return fail(ProductSpecError::UnknownComponentDependency, productId);

                bool sourceIsSensor = false;
                for (JsonVariantConst candidate : components)
                {
                    if (String(candidate["id"] | "") == sourceComponentId)
                    {
                        const String sourceType = candidate["component_type"] | "";
                        sourceIsSensor = sourceType == "physical_sensor" ||
                                         sourceType == "virtual_sensor";
                        break;
                    }
                }
                if (!sourceIsSensor)
                    return fail(ProductSpecError::InvalidVirtualSensorDeclaration, productId);
            }
            for (JsonPairConst dependency : actuatorDependencies)
            {
                const char *dependencyId = dependency.key().c_str();
                const char *sourceId = dependency.value().as<const char *>();
                if (!dependencyId || !dependencyId[0] || !sourceId || !sourceId[0] ||
                    String(sourceId) == componentId || !hasComponent(components, sourceId))
                    return fail(ProductSpecError::UnknownComponentDependency, productId);
                bool sourceIsActuator = false;
                for (JsonVariantConst candidate : components)
                    if (String(candidate["id"] | "") == sourceId) {
                        const String sourceType = candidate["component_type"] | "";
                        sourceIsActuator = sourceType == "physical_actuator" || sourceType == "virtual_actuator" || sourceType == "actuator";
                        break;
                    }
                if (!sourceIsActuator) return fail(ProductSpecError::InvalidVirtualSensorDeclaration, productId);
            }
        }
        else if (hasInputs || hasParameters || !component["actuators"].isNull())
        {
            return fail(ProductSpecError::InvalidVirtualSensorDeclaration, productId);
        }
    }

    // Validate every capability/channel mapping before creating component instances.
    for (JsonVariantConst mapping : mappings)
    {
        const char *capabilityId = mapping["capability_id"] | "";
        const char *channelId = mapping["channel_id"] | "";
        const char *componentId = mapping["component_id"] | "";
        const char *endpoint = mapping["endpoint"] | "";
        if (!capabilityId[0] || !channelId[0] || !componentId[0] || !endpoint[0])
            return fail(ProductSpecError::InvalidMapping, productId);
        if (!hasCapability(capabilities, capabilityId))
            return fail(ProductSpecError::UnknownCapability, productId);
        if (!hasComponent(components, componentId))
            return fail(ProductSpecError::UnknownComponent, productId);

        bool channelFound = false;
        for (JsonVariantConst capability : capabilities)
        {
            if (String(capability["id"] | "") != capabilityId)
                continue;
            JsonArrayConst channels = capability["channels"].as<JsonArrayConst>();
            for (JsonVariantConst channel : channels)
                if (String(channel["id"] | "") == channelId)
                    channelFound = true;
        }
        if (!channelFound)
            return fail(ProductSpecError::InvalidMapping, productId);
    }

    // Build all views before creating instances. The source document is never modified.
    JsonDocument capabilitiesView;
    capabilitiesView["product_id"] = productId;
    capabilitiesView["capabilities"].set(source["capabilities"]);
    JsonArray virtualSensorsView = capabilitiesView["virtual_sensors"].to<JsonArray>();
    for (JsonVariantConst component : components)
    {
        if (String(component["component_type"] | "") != "virtual_sensor")
            continue;

        const char *componentId = component["id"] | "";
        JsonObject virtualSensor = virtualSensorsView.add<JsonObject>();
        virtualSensor["component_id"] = componentId;
        virtualSensor["driver"] = component["driver"] | "";
        virtualSensor["parameters"].set(component["parameters"]);

        // Associate the component with its public sensor capability so the app
        // can offer its parameter editor from the corresponding sensor view.
        for (JsonVariantConst mapping : mappings)
        {
            if (String(mapping["component_id"] | "") != componentId)
                continue;
            virtualSensor["capability_id"] = mapping["capability_id"] | "";
            virtualSensor["channel_id"] = mapping["channel_id"] | "";
            break;
        }
    }

    JsonDocument runtimeView;
    runtimeView["product_id"] = productId;
    runtimeView["resources"].set(source["resources"]);
    runtimeView["components"].set(source["components"]);
    runtimeView["mappings"].set(source["mappings"]);
    if (source["history_policy"].is<JsonObject>()) runtimeView["history_policy"].set(source["history_policy"]);

    JsonDocument configurationView;
    configurationView["product_id"] = productId;
    if (source["configuration_schema"].is<JsonObject>())
        configurationView["configuration_schema"].set(source["configuration_schema"]);
    else
        configurationView["configuration_schema"].to<JsonObject>();

    JsonDocument metadataView;
    metadataView["schema_version"] = spec["schema_version"] | "1.0";
    metadataView["product_id"] = productId;
    metadataView["product_name"] = spec["product_name"] | "";
    metadataView["hardware_revision"] = spec["hardware_revision"] | "";
    metadataView["specification_checksum"] = specificationChecksum(specificationJson);

    if (serializeJson(capabilitiesView, capabilitiesJson) == 0 ||
        serializeJson(runtimeView, runtimeManifestJson) == 0 ||
        serializeJson(configurationView, configurationSchemaJson) == 0 ||
        serializeJson(metadataView, metadataJson) == 0)
        return fail(ProductSpecError::ProjectionFailed, productId);

    ProductSpecResult result;
    result.productId = productId;

    for (JsonVariantConst component : components)
    {
        const char *id = component["id"] | "";
        const char *driver = component["driver"] | "";
        if (!registry.create(id, driver))
            return fail(ProductSpecError::ComponentCreationFailed, productId, result.componentsCreated);
        ++result.componentsCreated;
    }

    result.success = true;
    result.error = ProductSpecError::None;
    return result;
}

} // namespace prakiva
