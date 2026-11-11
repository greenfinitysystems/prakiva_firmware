#include <ArduinoJson.h>
#include <cmath>

#include "product/prakiva_component_initializer.h"
#include "core/prakiva_json.h"
#include "components/core/prakiva_virtual_sensor_driver.h"
#include "components/core/prakiva_virtual_actuator_driver.h"
#include "components/drivers/pwm_actuator_driver.h"
#include "components/drivers/binary_actuator_driver.h"

namespace prakiva
{
namespace
{
struct ComponentNode
{
    String id;
    String type;
    IComponent *component = nullptr;
    JsonObjectConst inputs;
    JsonObjectConst actuators;
};

ProductComponentInitializationResult failure(
    ProductComponentInitializationError error,
    const String &componentId = String(),
    size_t initialized = 0)
{
    ProductComponentInitializationResult result;
    result.error = error;
    result.componentId = componentId;
    result.initializedCount = initialized;
    return result;
}

int findNode(const ComponentNode *nodes, size_t count, const char *id)
{
    if (!id || !id[0])
        return -1;
    for (size_t i = 0; i < count; ++i)
        if (nodes[i].id == id)
            return static_cast<int>(i);
    return -1;
}

bool dependenciesProcessed(
    const ComponentNode &node,
    const ComponentNode *nodes,
    size_t count,
    const bool *processed)
{
    for (JsonPairConst input : node.inputs) { const int sourceIndex=findNode(nodes,count,input.value().as<const char *>()); if(sourceIndex<0 || !processed[sourceIndex]) return false; }
    for (JsonPairConst input : node.actuators) { const int sourceIndex=findNode(nodes,count,input.value().as<const char *>()); if(sourceIndex<0 || !processed[sourceIndex]) return false; }
    return true;
}

bool dependenciesReady(const ComponentNode &node, const ComponentNode *nodes, size_t count)
{
    for (JsonPairConst input : node.inputs) { const int sourceIndex=findNode(nodes,count,input.value().as<const char *>()); if(sourceIndex<0 || nodes[sourceIndex].component->status()!=ComponentStatus::Ready) return false; }
    for (JsonPairConst input : node.actuators) { const int sourceIndex=findNode(nodes,count,input.value().as<const char *>()); if(sourceIndex<0 || nodes[sourceIndex].component->status()!=ComponentStatus::Ready) return false; }
    return true;
}

bool configureVirtualSensors(
    const String &runtimeManifestJson,
    ComponentRegistry &registry,
    JsonObjectConst configuration,
    bool apply,
    String &errorMessage)
{
    JsonDocument manifest;
    if (runtimeManifestJson.length() == 0 || deserializeJson(manifest, runtimeManifestJson))
    {
        errorMessage = "The product runtime manifest is invalid.";
        return false;
    }

    JsonArrayConst components = manifest["components"].as<JsonArrayConst>();
    JsonObjectConst configuredSensors = configuration["virtual_sensors"].as<JsonObjectConst>();
    if (!configuration["virtual_sensors"].isUnbound() &&
        !configuration["virtual_sensors"].isNull() &&
        configuredSensors.isNull())
    {
        errorMessage = "virtual_sensors must be a JSON object.";
        return false;
    }

    // Reject stale or misspelled component IDs instead of silently ignoring
    // parameter values that can no longer be applied to this product.
    if (!configuredSensors.isNull())
    {
        for (JsonPairConst configured : configuredSensors)
        {
            bool declared = false;
            for (JsonVariantConst component : components)
            {
                if (String(component["id"] | "") == configured.key().c_str() &&
                    String(component["component_type"] | "") == "virtual_sensor")
                {
                    declared = true;
                    break;
                }
            }
            if (!declared)
            {
                errorMessage = String("Unknown virtual sensor component: ") + configured.key().c_str();
                return false;
            }
        }
    }

    for (JsonVariantConst declaration : components)
    {
        if (String(declaration["component_type"] | "") != "virtual_sensor")
            continue;

        const char *componentId = declaration["id"] | "";
        IComponent *component = registry.find(componentId);
        IVirtualSensorDriver *driver = component ? component->asVirtualSensorDriver() : nullptr;
        if (!driver)
        {
            errorMessage = String("Virtual sensor driver is unavailable: ") + componentId;
            return false;
        }

        // Unit is fixed product metadata, independent of app-editable numeric parameters.
        // Configure it only on application; validation must not mutate driver state.
        if (apply)
            driver->setMeasurementUnit(String(declaration["unit"] | "n/a"));

        JsonObjectConst schema = declaration["parameters"].as<JsonObjectConst>();
        JsonObjectConst supplied = configuredSensors[componentId]["parameters"].as<JsonObjectConst>();
        if (!configuredSensors[componentId].isUnbound() &&
            !configuredSensors[componentId].isNull() &&
            supplied.isNull())
        {
            errorMessage = String("Parameters for virtual sensor ") + componentId + " must be an object.";
            return false;
        }

        JsonDocument effectiveDocument;
        JsonObject effective = effectiveDocument.to<JsonObject>();

        if (!supplied.isNull())
        {
            for (JsonPairConst value : supplied)
            {
                if (schema.isNull() || schema[value.key().c_str()].isUnbound())
                {
                    errorMessage = String("Unknown parameter '") + value.key().c_str() +
                                   "' for virtual sensor " + componentId + ".";
                    return false;
                }
            }
        }

        for (JsonPairConst parameter : schema)
        {
            const char *parameterId = parameter.key().c_str();
            JsonObjectConst definition = parameter.value().as<JsonObjectConst>();
            if (String(definition["type"] | "") != "number" ||
                !definition["default"].is<float>())
            {
                errorMessage = String("Unsupported parameter schema for ") + componentId + "." + parameterId;
                return false;
            }

            const bool hasSupplied = !supplied.isNull() && !supplied[parameterId].isUnbound();
            JsonVariantConst value = hasSupplied
                ? supplied[parameterId]
                : definition["default"];

            if (!value.is<float>())
            {
                errorMessage = String("Parameter '") + parameterId + "' for " + componentId + " must be numeric.";
                return false;
            }

            const double numeric = value.as<double>();
            if (!std::isfinite(numeric) ||
                (definition["minimum"].is<float>() && numeric < definition["minimum"].as<double>()) ||
                (definition["maximum"].is<float>() && numeric > definition["maximum"].as<double>()))
            {
                errorMessage = String("Parameter '") + parameterId + "' for " + componentId + " is outside its declared range.";
                return false;
            }
            effective[parameterId].set(value);
        }

        const JsonObjectConst effectiveConst = effective;
        if (!driver->validateParameters(effectiveConst))
        {
            errorMessage = String("The virtual sensor rejected parameters for ") + componentId + ".";
            return false;
        }
        if (apply && !driver->applyParameters(effectiveConst))
        {
            errorMessage = String("The virtual sensor could not apply parameters for ") + componentId + ".";
            return false;
        }
    }

    errorMessage = "";
    return true;
}
} // namespace

bool validateVirtualSensorConfiguration(
    const String &runtimeManifestJson,
    ComponentRegistry &registry,
    JsonObjectConst configuration,
    String &errorMessage)
{
    return configureVirtualSensors(runtimeManifestJson, registry, configuration, false, errorMessage);
}

bool applyVirtualSensorConfiguration(
    const String &runtimeManifestJson,
    ComponentRegistry &registry,
    JsonObjectConst configuration,
    String &errorMessage)
{
    return configureVirtualSensors(runtimeManifestJson, registry, configuration, true, errorMessage);
}

ProductComponentInitializationResult initializeProductComponents(
    const String &runtimeManifestJson,
    ComponentRegistry &registry,
    const String &configurationJson)
{
    JsonDocument manifest;
    if (runtimeManifestJson.length() == 0 || deserializeJson(manifest, runtimeManifestJson))
        return failure(ProductComponentInitializationError::InvalidRuntimeManifest);

    JsonArrayConst components = manifest["components"].as<JsonArrayConst>();
    if (components.isNull() || components.size() == 0 ||
        components.size() > MAX_COMPONENT_INSTANCES)
        return failure(ProductComponentInitializationError::InvalidRuntimeManifest);

    const size_t count = components.size();
    if (registry.instanceCount() != count)
        return failure(ProductComponentInitializationError::ComponentCountMismatch);

    ComponentNode nodes[MAX_COMPONENT_INSTANCES];
    size_t index = 0;
    for (JsonVariantConst declaration : components)
    {
        const char *id = declaration["id"] | "";
        const char *type = declaration["component_type"] | "";
        if (!id[0] || findNode(nodes, index, id) >= 0)
            return failure(ProductComponentInitializationError::InvalidComponentDeclaration, id);

        IComponent *component = registry.find(id);
        if (!component)
            return failure(ProductComponentInitializationError::ComponentNotFound, id);

        nodes[index].id = id;
        nodes[index].type = type;
        nodes[index].component = component;
        nodes[index].inputs = declaration["inputs"].as<JsonObjectConst>();
        nodes[index].actuators = declaration["actuators"].as<JsonObjectConst>();

        if (nodes[index].type == "physical_sensor" && !component->asSensorDriver())
            return failure(ProductComponentInitializationError::SensorInterfaceMismatch, id);
        if (nodes[index].type == "physical_actuator" && !component->asActuatorDriver())
            return failure(ProductComponentInitializationError::VirtualActuatorInterfaceMismatch, id);

        if (nodes[index].type == "virtual_sensor" && !component->asVirtualSensorDriver())
            return failure(ProductComponentInitializationError::VirtualSensorInterfaceMismatch, id);
        if (nodes[index].type == "virtual_actuator" && !component->asVirtualActuatorDriver())
            return failure(ProductComponentInitializationError::VirtualActuatorInterfaceMismatch, id);

        if (nodes[index].type != "virtual_sensor" && nodes[index].type != "virtual_actuator" && !nodes[index].inputs.isNull())
            return failure(ProductComponentInitializationError::InvalidComponentDeclaration, id);
        if (nodes[index].type != "virtual_actuator" && !nodes[index].actuators.isNull())
            return failure(ProductComponentInitializationError::InvalidComponentDeclaration, id);

        ++index;
    }

    // Resolve physical output resources exclusively from the product manifest.
    // NullActuatorDriver deliberately does not claim its reserved GPIO.
    JsonArrayConst resources = manifest["resources"].as<JsonArrayConst>();
    if (resources.isNull())
        return failure(ProductComponentInitializationError::InvalidRuntimeManifest);
    bool claimedPins[49] = {};
    for (JsonVariantConst declaration : components)
    {
        const String driverType = declaration["driver"] | "";
        const bool pwm = driverType == "PwmActuatorDriver";
        const bool binary = driverType == "BinaryActuatorDriver";
        if (!pwm && !binary)
            continue;
        const char *id = declaration["id"] | "";
        JsonObjectConst refs = declaration["resources"].as<JsonObjectConst>();
        const char *resourceId = refs["output"] | "";
        if (!resourceId[0] || String(declaration["component_type"] | "") != "physical_actuator")
            return failure(ProductComponentInitializationError::InvalidComponentDeclaration, id);
        int pin = -1;
        for (JsonVariantConst resource : resources)
        {
            if (String(resource["id"] | "") != resourceId)
                continue;
            if (String(resource["type"] | "") != "gpio" ||
                String(resource["direction"] | "") != "output" ||
                !resource["gpio"].is<int>())
                return failure(ProductComponentInitializationError::InvalidComponentDeclaration, id);
            pin = resource["gpio"].as<int>();
            break;
        }
        // ESP32-S3 pin range alone does not imply electrical suitability.
        // Only GPIOs declared as safe physical outputs in this product are permitted.
        // These are generic S3 output exclusions, not an aquarium GPIO map.
        if (pin < 0 || pin > 48 || claimedPins[pin] ||
            pin == 0 || pin == 3 || pin == 19 || pin == 20 ||
            (pin >= 26 && pin <= 37) || pin == 45 || pin == 46)
            return failure(ProductComponentInitializationError::InvalidComponentDeclaration, id);
        IComponent *instance = registry.find(id);
        bool configured = false;
        if (pwm)
        {
            auto *driver = static_cast<PwmActuatorDriver *>(instance);
            configured = driver && driver->configurePin(pin);
        }
        else
        {
            auto *driver = static_cast<BinaryActuatorDriver *>(instance);
            configured = driver && driver->configurePin(pin);
        }
        if (!configured)
            return failure(ProductComponentInitializationError::InvalidComponentDeclaration, id);
        claimedPins[pin] = true;
        Serial.printf("[PRODUCT RUNTIME] Bound %s to GPIO%d (resource=%s)\n", id, pin, resourceId);
    }

    // Bind all inputs before any component is initialized. This allows a
    // virtual sensor to hold references to upstream drivers before begin().
    for (size_t i = 0; i < count; ++i)
    {
        if (nodes[i].type != "virtual_sensor")
            continue;

        IVirtualSensorDriver *virtualDriver = nodes[i].component->asVirtualSensorDriver();
        if (nodes[i].inputs.isNull())
            continue;

        for (JsonPairConst input : nodes[i].inputs)
        {
            const char *inputId = input.key().c_str();
            const char *sourceId = input.value().as<const char *>();
            const int sourceIndex = findNode(nodes, count, sourceId);
            if (sourceIndex < 0 || !nodes[sourceIndex].component->asSensorDriver())
                return failure(ProductComponentInitializationError::SensorInterfaceMismatch, nodes[i].id);

            if (!virtualDriver->bindInput(inputId, nodes[sourceIndex].component->asSensorDriver()))
                return failure(ProductComponentInitializationError::InputBindingFailed, nodes[i].id);
        }
    }

    // Virtual actuators may also consume sensor dependencies (for example a
    // water-level sensor) in addition to their private actuator dependencies.
    for (size_t i = 0; i < count; ++i)
    {
        if (nodes[i].type != "virtual_actuator" || nodes[i].inputs.isNull()) continue;
        IVirtualActuatorDriver *virtualDriver = nodes[i].component->asVirtualActuatorDriver();
        for (JsonPairConst input : nodes[i].inputs)
        {
            const char *inputId = input.key().c_str();
            const int sourceIndex = findNode(nodes, count, input.value().as<const char *>());
            if (sourceIndex < 0 || !nodes[sourceIndex].component->asSensorDriver())
                return failure(ProductComponentInitializationError::SensorInterfaceMismatch, nodes[i].id);
            if (!virtualDriver->bindSensor(inputId, nodes[sourceIndex].component->asSensorDriver()))
                return failure(ProductComponentInitializationError::InputBindingFailed, nodes[i].id);
        }
    }

    // Bind private physical actuator dependencies before initialization.
    for (size_t i = 0; i < count; ++i)
    {
        if (nodes[i].type != "virtual_actuator") continue;
        IVirtualActuatorDriver *virtualDriver = nodes[i].component->asVirtualActuatorDriver();
        if (nodes[i].actuators.isNull())
            return failure(ProductComponentInitializationError::InvalidComponentDeclaration, nodes[i].id);
        for (JsonPairConst dependency : nodes[i].actuators)
        {
            const char *dependencyId = dependency.key().c_str();
            const int sourceIndex = findNode(nodes, count, dependency.value().as<const char *>());
            if (sourceIndex < 0 || !nodes[sourceIndex].component->asActuatorDriver())
                return failure(ProductComponentInitializationError::ActuatorBindingFailed, nodes[i].id);
            if (!virtualDriver->bindActuator(dependencyId, nodes[sourceIndex].component->asActuatorDriver()))
                return failure(ProductComponentInitializationError::ActuatorBindingFailed, nodes[i].id);
        }
    }

    // Apply the controller-owned parameter values (or Product Specification
    // defaults) after inputs are bound and before any virtual sensor begins.
    JsonDocument configurationDocument;
    if (configurationJson.length() > 0)
    {
        const DeserializationError configurationParseError = deserializeJson(
            configurationDocument,
            configurationJson,
            DeserializationOption::NestingLimit(JSON_NESTING_LIMIT));
        if (configurationParseError)
        {
            Serial.printf(
                "[PRODUCT RUNTIME] ERROR: saved configuration parse failed: %s (bytes=%u, nesting_limit=%u)\n",
                configurationParseError.c_str(),
                static_cast<unsigned>(configurationJson.length()),
                JSON_NESTING_LIMIT);
            return failure(ProductComponentInitializationError::InvalidRuntimeManifest);
        }
    }

    String parameterError;
    if (!applyVirtualSensorConfiguration(
            runtimeManifestJson, registry, configurationDocument.as<JsonObjectConst>(), parameterError))
        return failure(ProductComponentInitializationError::InvalidComponentDeclaration, parameterError);

    // Stable Kahn-style ordering: scan in specification order and select the
    // first component whose dependencies have already been processed.
    bool processed[MAX_COMPONENT_INSTANCES] = {};
    size_t order[MAX_COMPONENT_INSTANCES] = {};
    size_t orderedCount = 0;
    while (orderedCount < count)
    {
        bool found = false;
        for (size_t i = 0; i < count; ++i)
        {
            if (processed[i] || !dependenciesProcessed(nodes[i], nodes, count, processed))
                continue;
            processed[i] = true;
            order[orderedCount++] = i;
            found = true;
            break;
        }
        if (!found)
            return failure(ProductComponentInitializationError::DependencyCycle);
    }

    ProductComponentInitializationResult result;
    for (size_t position = 0; position < orderedCount; ++position)
    {
        const ComponentNode &node = nodes[order[position]];
        if (!dependenciesReady(node, nodes, count))
        {
            result.success = false;
            result.error = ProductComponentInitializationError::DependencyNotReady;
            result.componentId = node.id;
            continue;
        }

        if (!registry.beginComponent(node.id.c_str()))
        {
            result.success = false;
            result.error = ProductComponentInitializationError::ComponentBeginFailed;
            result.componentId = node.id;
            continue;
        }
        ++result.initializedCount;
    }

    if (result.error == ProductComponentInitializationError::None)
        result.success = true;
    return result;
}

} // namespace prakiva
