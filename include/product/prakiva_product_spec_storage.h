#pragma once

#include <Arduino.h>

namespace prakiva
{

struct ProductProjectionSet
{
    String capabilitiesJson;
    String runtimeManifestJson;
    String configurationSchemaJson;
    String metadataJson;
};

struct ProductProjectionState
{
    uint32_t specificationChecksum = 0;
    uint32_t generation = 0;
};

// FNV-1a checksum of the exact compiled-in manufacturer specification.
uint32_t calculateProductSpecificationChecksum(const char *specificationJson);

// Loads the latest committed projection generation only when its checksum
// matches the currently compiled-in specification.
bool loadPersistedProductProjections(
    uint32_t expectedChecksum,
    ProductProjectionSet &projections,
    ProductProjectionState &state);

// Writes and verifies a complete new generation, then switches the active
// generation pointer. The previous pointer remains available for recovery.
bool persistProductProjections(
    uint32_t specificationChecksum,
    const ProductProjectionSet &projections,
    ProductProjectionState &state);

// Exercises newest-generation corruption and fallback in an isolated test directory.
bool runProductProjectionRecoverySelfTest(
    uint32_t specificationChecksum,
    const ProductProjectionSet &projections);

} // namespace prakiva
