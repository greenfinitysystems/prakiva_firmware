#include "product/prakiva_product_runtime.h"

#include "components/drivers/prakiva_driver_registration.h"
#include "product/prakiva_product_specification.h"
#include "product/prakiva_product_spec_processor.h"
#include "product/prakiva_component_initializer.h"
#include "core/prakiva_storage.h"
#include <ArduinoJson.h>

namespace prakiva
{
namespace
{
ComponentRegistry runtimeRegistry;
ProductProjectionSet runtimeProjections;
bool runtimeStarted = false;
bool runtimeReady = false;
bool componentsReady = false;
CapabilityResolver runtimeCapabilityResolver;
bool capabilityResolverReady = false;

void logComponentStatuses()
{
    for (size_t i = 0; i < runtimeRegistry.instanceCount(); ++i)
    {
        const char *id = runtimeRegistry.componentIdAt(i);
        if (!id)
            continue;

        const ComponentStatus status = runtimeRegistry.getStatus(id);
        const char *label = status == ComponentStatus::Ready
                                ? "READY"
                                : status == ComponentStatus::NotInitialized
                                      ? "NOT_INITIALIZED"
                                      : "ERROR";
        Serial.printf("[PRODUCT RUNTIME] Component %s: %s\n", id, label);
    }
}
} // namespace

bool beginProductRuntime()
{
    if (runtimeStarted)
        return runtimeReady;
    runtimeStarted = true;

    Serial.println();
    Serial.println("=== Product Runtime Initialization ===");

    if (!registerPlatformDrivers(runtimeRegistry))
    {
        Serial.println("[PRODUCT RUNTIME] ERROR: driver factory registration failed");
        return false;
    }
    Serial.printf("[PRODUCT RUNTIME] Registered %u driver factories\n",
                  static_cast<unsigned>(runtimeRegistry.factoryCount()));

    // Processing validates the immutable source, creates the component instances,
    // and builds projections in RAM. It is deliberately done on every boot:
    // persisted projections are cache artefacts, never the authority for runtime
    // component construction.
    ProductProjectionSet generated;
    const ProductSpecResult processed = processProductSpecification(
        PRAKIVA_AQUARIUM_PRODUCT_SPEC,
        runtimeRegistry,
        generated.capabilitiesJson,
        generated.runtimeManifestJson,
        generated.configurationSchemaJson,
        generated.metadataJson);

    if (!processed.success)
    {
        Serial.printf("[PRODUCT RUNTIME] ERROR: specification processing failed (code=%u)\n",
                      static_cast<unsigned>(processed.error));
        return false;
    }

    const uint32_t checksum =
        calculateProductSpecificationChecksum(PRAKIVA_AQUARIUM_PRODUCT_SPEC);
    if (checksum == 0)
    {
        Serial.println("[PRODUCT RUNTIME] ERROR: invalid specification checksum");
        return false;
    }

    ProductProjectionState projectionState;
    if (isStorageReady() &&
        loadPersistedProductProjections(checksum, runtimeProjections, projectionState))
    {
        Serial.printf("[PRODUCT RUNTIME] Loaded persisted projections: generation=%lu checksum=%08lx\n",
                      static_cast<unsigned long>(projectionState.generation),
                      static_cast<unsigned long>(projectionState.specificationChecksum));
    }
    else
    {
        runtimeProjections = generated;
        if (!isStorageReady())
        {
            Serial.println("[PRODUCT RUNTIME] WARNING: LittleFS unavailable; using generated in-memory projections");
        }
        else if (persistProductProjections(checksum, generated, projectionState))
        {
            runtimeProjections = generated;
            Serial.printf("[PRODUCT RUNTIME] Generated and committed projections: generation=%lu checksum=%08lx\n",
                          static_cast<unsigned long>(projectionState.generation),
                          static_cast<unsigned long>(projectionState.specificationChecksum));
        }
        else
        {
            // Persistence is a cache optimisation. A valid compiled-in spec and
            // generated in-memory views are sufficient to run this boot.
            Serial.println("[PRODUCT RUNTIME] WARNING: projection persistence failed; using generated in-memory projections");
        }
    }

    // Restore controller-owned virtual sensor parameters before component
    // initialization. Missing configuration means Product Specification defaults.
    String savedConfigurationJson;
    uint32_t savedConfigurationTimestamp = 0;
    String configurationErrorCode, configurationErrorMessage;
    if (isStorageReady() &&
        !loadConfigurationJson(savedConfigurationJson, savedConfigurationTimestamp,
                               configurationErrorCode, configurationErrorMessage))
    {
        if (configurationErrorCode != "CONFIGURATION_NOT_FOUND")
            Serial.printf("[PRODUCT RUNTIME] WARNING: saved configuration unavailable (%s); using defaults\\n",
                          configurationErrorCode.c_str());
        savedConfigurationJson = "{}";
    }

    const ProductComponentInitializationResult initialization =
        initializeProductComponents(runtimeProjections.runtimeManifestJson, runtimeRegistry,
                                    savedConfigurationJson);
    componentsReady = initialization.success;
    logComponentStatuses();
    if (!componentsReady)
    {
        Serial.printf("[PRODUCT RUNTIME] ERROR: component initialization failed (code=%u component=%s initialized=%u)\n",
                      static_cast<unsigned>(initialization.error),
                      initialization.componentId.c_str(),
                      static_cast<unsigned>(initialization.initializedCount));
    }

    capabilityResolverReady = runtimeCapabilityResolver.initialize(
        runtimeProjections.capabilitiesJson,
        runtimeProjections.runtimeManifestJson,
        runtimeRegistry);

    if (capabilityResolverReady)
    {
        Serial.printf("[PRODUCT RUNTIME] Capability resolver ready: channels=%u mapped=%u\n",
                      static_cast<unsigned>(runtimeCapabilityResolver.channelCount()),
                      static_cast<unsigned>(runtimeCapabilityResolver.mappedChannelCount()));
    }
    else
    {
        Serial.println("[PRODUCT RUNTIME] ERROR: capability resolver initialization failed");
    }

    runtimeReady = true;
    Serial.printf("[PRODUCT RUNTIME] Ready: product=%s components=%u\n",
                  processed.productId.c_str(),
                  static_cast<unsigned>(runtimeRegistry.instanceCount()));
    Serial.println("=== Product Runtime Initialization Complete ===");
    return true;
}

ComponentRegistry &productComponentRegistry()
{
    return runtimeRegistry;
}

const ProductProjectionSet &activeProductProjections()
{
    return runtimeProjections;
}

bool productRuntimeReady()
{
    return runtimeReady;
}

bool productComponentsReady()
{
    return componentsReady;
}

CapabilityResolver &productCapabilityResolver()
{
    return runtimeCapabilityResolver;
}

bool productCapabilityResolverReady()
{
    return capabilityResolverReady;
}

bool validateProductConfiguration(JsonObjectConst configuration, String &errorMessage)
{
    if (!runtimeReady || !componentsReady)
    {
        errorMessage = "The product runtime is not ready.";
        return false;
    }
    return validateVirtualSensorConfiguration(
        runtimeProjections.runtimeManifestJson, runtimeRegistry, configuration, errorMessage);
}

bool applyProductConfiguration(JsonObjectConst configuration, String &errorMessage)
{
    if (!runtimeReady || !componentsReady)
    {
        errorMessage = "The product runtime is not ready.";
        return false;
    }
    return applyVirtualSensorConfiguration(
        runtimeProjections.runtimeManifestJson, runtimeRegistry, configuration, errorMessage);
}

} // namespace prakiva
