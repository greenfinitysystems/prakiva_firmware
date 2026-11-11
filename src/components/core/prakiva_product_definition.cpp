#include <ArduinoJson.h>

#include "components/core/prakiva_product_definition.h"

namespace prakiva
{
namespace
{
    constexpr size_t PRODUCT_JSON_CAPACITY = 2048;

    ProductDefinitionResult failure(
        ProductDefinitionError error,
        const String &productId = String(),
        size_t created = 0)
    {
        ProductDefinitionResult result;
        result.error = error;
        result.productId = productId;
        result.componentsCreated = created;
        return result;
    }

} // namespace

ProductDefinitionResult loadProductDefinition(
    const char *json,
    ComponentRegistry &registry)
{
    if (json == nullptr || json[0] == '\0')
        return failure(ProductDefinitionError::InvalidJson);

    JsonDocument document;
    const DeserializationError parseError = deserializeJson(document, json);
    if (parseError || !document.is<JsonObject>())
        return failure(ProductDefinitionError::InvalidJson);

    const char *productId = document["product_id"] | "";
    if (productId[0] == '\0')
        return failure(ProductDefinitionError::MissingProductId);

    JsonArray components = document["components"].as<JsonArray>();
    if (components.isNull())
        return failure(ProductDefinitionError::MissingComponents, productId);

    if (components.size() == 0)
        return failure(ProductDefinitionError::MissingComponents, productId);

    if (components.size() > MAX_COMPONENT_INSTANCES ||
        registry.instanceCount() + components.size() > MAX_COMPONENT_INSTANCES)
        return failure(ProductDefinitionError::InstanceCapacityExceeded, productId);

    // Validate every entry before creating any instances.
    for (JsonVariant entry : components)
    {
        if (!entry.is<JsonObject>())
            return failure(ProductDefinitionError::InvalidComponentEntry, productId);

        const char *componentId = entry["id"] | "";
        const char *driverType = entry["driver"] | "";

        if (componentId[0] == '\0' || driverType[0] == '\0')
            return failure(ProductDefinitionError::InvalidComponentEntry, productId);

        if (registry.hasComponent(componentId))
            return failure(ProductDefinitionError::ComponentAlreadyExists, productId);

        if (!registry.hasFactory(driverType))
            return failure(ProductDefinitionError::UnknownDriver, productId);
    }

    // Reject duplicate IDs within this product definition.
    for (size_t i = 0; i < components.size(); ++i)
    {
        const char *leftId = components[i]["id"] | "";
        for (size_t j = i + 1; j < components.size(); ++j)
        {
            const char *rightId = components[j]["id"] | "";
            if (String(leftId) == rightId)
                return failure(ProductDefinitionError::DuplicateComponentId, productId);
        }
    }

    ProductDefinitionResult result;
    result.productId = productId;

    for (JsonVariant entry : components)
    {
        const char *componentId = entry["id"] | "";
        const char *driverType = entry["driver"] | "";

        if (registry.create(componentId, driverType) == nullptr)
            return failure(ProductDefinitionError::ComponentCreationFailed,
                           productId, result.componentsCreated);

        ++result.componentsCreated;
    }

    result.success = true;
    result.error = ProductDefinitionError::None;
    return result;
}

} // namespace prakiva
