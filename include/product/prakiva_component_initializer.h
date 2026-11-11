#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "components/core/prakiva_component_registry.h"

namespace prakiva
{

enum class ProductComponentInitializationError : uint8_t
{
    None = 0,
    InvalidRuntimeManifest,
    ComponentCountMismatch,
    ComponentNotFound,
    InvalidComponentDeclaration,
    SensorInterfaceMismatch,
    VirtualSensorInterfaceMismatch,
    VirtualActuatorInterfaceMismatch,
    InputBindingFailed,
    ActuatorBindingFailed,
    DependencyCycle,
    DependencyNotReady,
    ComponentBeginFailed
};

struct ProductComponentInitializationResult
{
    bool success = false;
    ProductComponentInitializationError error = ProductComponentInitializationError::None;
    String componentId;
    size_t initializedCount = 0;
};

// Binds declared virtual-sensor inputs and initializes all components in a
// stable dependency order. Independent components retain specification order.
ProductComponentInitializationResult initializeProductComponents(
    const String &runtimeManifestJson,
    ComponentRegistry &registry,
    const String &configurationJson = String());

// Validate and apply virtual-sensor parameter values against the compiled
// Product Specification schema. Validation never mutates a driver.
bool validateVirtualSensorConfiguration(
    const String &runtimeManifestJson,
    ComponentRegistry &registry,
    JsonObjectConst configuration,
    String &errorMessage);

bool applyVirtualSensorConfiguration(
    const String &runtimeManifestJson,
    ComponentRegistry &registry,
    JsonObjectConst configuration,
    String &errorMessage);

} // namespace prakiva
