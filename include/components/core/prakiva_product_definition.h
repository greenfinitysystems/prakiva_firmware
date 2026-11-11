#pragma once

#include <Arduino.h>

#include "components/core/prakiva_component_registry.h"

namespace prakiva
{

    enum class ProductDefinitionError : uint8_t
    {
        None = 0,
        InvalidJson,
        MissingProductId,
        MissingComponents,
        InvalidComponentEntry,
        DuplicateComponentId,
        ComponentAlreadyExists,
        UnknownDriver,
        InstanceCapacityExceeded,
        ComponentCreationFailed
    };

    struct ProductDefinitionResult
    {
        bool success = false;
        ProductDefinitionError error = ProductDefinitionError::None;
        String productId;
        size_t componentsCreated = 0;
    };

    // Loads a declarative JSON product definition into an existing registry.
    // Factories must be registered before this function is called.
    ProductDefinitionResult loadProductDefinition(
        const char *json,
        ComponentRegistry &registry);

} // namespace prakiva
