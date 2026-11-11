#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "components/core/prakiva_component_registry.h"

namespace prakiva
{

enum class ProductSpecError : uint8_t
{
    None = 0,
    InvalidJson,
    MissingSpecification,
    MissingProductId,
    MissingArray,
    InvalidEntry,
    DuplicateId,
    UnknownDriver,
    UnknownResource,
    UnknownCapability,
    UnknownComponent,
    UnknownComponentDependency,
    InvalidVirtualSensorDeclaration,
    InvalidHistoryPolicy,
    InvalidMapping,
    RegistryCapacityExceeded,
    ComponentCreationFailed,
    ProjectionFailed
};

struct ProductSpecResult
{
    bool success = false;
    ProductSpecError error = ProductSpecError::None;
    String productId;
    size_t componentsCreated = 0;
};

// Parses and validates the canonical manufacturer specification, creates its
// component instances, and produces stakeholder-specific JSON projections.
// Projection documents are returned as JSON strings for the caller to persist.
ProductSpecResult processProductSpecification(
    const char *specificationJson,
    ComponentRegistry &registry,
    String &capabilitiesJson,
    String &runtimeManifestJson,
    String &configurationSchemaJson,
    String &metadataJson);

} // namespace prakiva
