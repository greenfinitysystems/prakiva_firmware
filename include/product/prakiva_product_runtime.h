#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

#include "components/core/prakiva_component_registry.h"
#include "product/prakiva_product_spec_storage.h"
#include "product/prakiva_capability_resolver.h"

namespace prakiva
{

// Boot-time owner of the active product's component instances and projections.
// The registry and projection set remain alive for the lifetime of the firmware.
bool beginProductRuntime();

ComponentRegistry &productComponentRegistry();
const ProductProjectionSet &activeProductProjections();
bool productRuntimeReady();
bool productComponentsReady();
CapabilityResolver &productCapabilityResolver();
bool productCapabilityResolverReady();

// Controller-owned virtual sensor parameters are validated against the
// compiled Product Specification and applied to the active driver instances.
bool validateProductConfiguration(JsonObjectConst configuration, String &errorMessage);
bool applyProductConfiguration(JsonObjectConst configuration, String &errorMessage);

} // namespace prakiva
