#include "product/prakiva_product_spec_storage.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <cstdio>

#include "core/prakiva_storage.h"

namespace prakiva
{
namespace
{
constexpr char CAP_SUFFIX[] = "_capabilities.json";
constexpr char RUNTIME_SUFFIX[] = "_runtime.json";
constexpr char CONFIG_SUFFIX[] = "_config_schema.json";
constexpr char META_SUFFIX[] = "_metadata.json";

struct PointerRecord
{
    bool valid = false;
    uint32_t sequence = 0;
    uint32_t checksum = 0;
    String generationName;
};

uint32_t fnv1a(const char *data)
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

String generationPath(const String &root, const String &generation, const char *suffix)
{
    return root + "/" + generation + suffix;
}

String pointerPath(const String &root, bool slotA)
{
    return root + (slotA ? "/active_a.json" : "/active_b.json");
}

bool readJsonString(const String &path, String &output)
{
    File file = LittleFS.open(path, "r");
    if (!file)
        return false;
    const size_t size = file.size();
    if (size == 0 || size > 64 * 1024 || !output.reserve(size + 1))
    {
        file.close();
        return false;
    }
    output = "";
    while (file.available())
        output += static_cast<char>(file.read());
    file.close();

    JsonDocument check;
    return !deserializeJson(check, output) && check.is<JsonObject>();
}

bool writeVerifiedJson(const String &path, const String &json)
{
    if (json.length() == 0 || json.length() > 64 * 1024)
        return false;

    const String tempPath = path + ".tmp";
    LittleFS.remove(tempPath);
    File file = LittleFS.open(tempPath, "w");
    if (!file)
        return false;

    const size_t written = file.write(
        reinterpret_cast<const uint8_t *>(json.c_str()), json.length());
    file.flush();
    file.close();

    if (written != json.length())
    {
        LittleFS.remove(tempPath);
        return false;
    }

    String readBack;
    if (!readJsonString(tempPath, readBack) || readBack != json)
    {
        LittleFS.remove(tempPath);
        return false;
    }

    LittleFS.remove(path);
    if (!LittleFS.rename(tempPath, path))
    {
        LittleFS.remove(tempPath);
        return false;
    }
    return true;
}

PointerRecord readPointer(const char *path)
{
    PointerRecord record;
    File file = LittleFS.open(path, "r");
    if (!file)
        return record;

    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, file);
    file.close();
    if (error || !doc.is<JsonObject>())
        return record;

    record.sequence = doc["sequence"] | 0u;
    record.checksum = doc["specification_checksum"] | 0u;
    const char *name = doc["generation"] | "";
    if (!record.sequence || !record.checksum || !name[0])
        return record;

    // Generation names are generated internally and must not contain path
    // separators or traversal characters when read back from persistent storage.
    for (const char *p = name; *p; ++p)
    {
        const bool allowed = (*p >= 'a' && *p <= 'z') ||
                             (*p >= 'A' && *p <= 'Z') ||
                             (*p >= '0' && *p <= '9') || *p == '_';
        if (!allowed)
            return record;
    }

    record.generationName = name;
    record.valid = true;
    return record;
}

PointerRecord newestPointer(const String &root)
{
    const PointerRecord a = readPointer(pointerPath(root, true).c_str());
    const PointerRecord b = readPointer(pointerPath(root, false).c_str());
    if (!a.valid)
        return b;
    if (!b.valid)
        return a;
    return a.sequence >= b.sequence ? a : b;
}

bool projectionsExistAndParse(const String &root, const String &generation, ProductProjectionSet &p)
{
    return readJsonString(generationPath(root, generation, CAP_SUFFIX), p.capabilitiesJson) &&
           readJsonString(generationPath(root, generation, RUNTIME_SUFFIX), p.runtimeManifestJson) &&
           readJsonString(generationPath(root, generation, CONFIG_SUFFIX), p.configurationSchemaJson) &&
           readJsonString(generationPath(root, generation, META_SUFFIX), p.metadataJson);
}

} // namespace

uint32_t calculateProductSpecificationChecksum(const char *specificationJson)
{
    return fnv1a(specificationJson);
}

static bool loadPersistedProductProjectionsAt(
    const String &root, uint32_t expectedChecksum,
    ProductProjectionSet &projections, ProductProjectionState &state)
{
    if (!isStorageReady() || !expectedChecksum)
        return false;

    const PointerRecord a = readPointer(pointerPath(root, true).c_str());
    const PointerRecord b = readPointer(pointerPath(root, false).c_str());

    // Try newest first, but only accept a generation whose four documents and
    // metadata provenance are valid. A damaged newest generation must not hide
    // the previous committed generation.
    const PointerRecord *candidates[2] = {&a, &b};
    if ((!a.valid && b.valid) ||
        (a.valid && b.valid && b.sequence > a.sequence))
    {
        candidates[0] = &b;
        candidates[1] = &a;
    }

    for (const PointerRecord *pointer : candidates)
    {
        if (!pointer->valid || pointer->checksum != expectedChecksum)
            continue;

        ProductProjectionSet loaded;
        if (!projectionsExistAndParse(root, pointer->generationName, loaded))
            continue;

        JsonDocument metadata;
        if (deserializeJson(metadata, loaded.metadataJson) ||
            metadata["specification_checksum"].as<uint32_t>() != expectedChecksum)
            continue;

        projections = loaded;
        state.specificationChecksum = pointer->checksum;
        state.generation = pointer->sequence;
        return true;
    }

    return false;
}

static bool persistProductProjectionsAt(
    const String &root, uint32_t specificationChecksum,
    const ProductProjectionSet &projections, ProductProjectionState &state)
{
    if (!isStorageReady() || !specificationChecksum)
        return false;

    if (!LittleFS.exists(root) && !LittleFS.mkdir(root))
        return false;

    // Validate all four projections before writing any generation files.
    const String *documents[] = {
        &projections.capabilitiesJson,
        &projections.runtimeManifestJson,
        &projections.configurationSchemaJson,
        &projections.metadataJson
    };
    for (const String *json : documents)
    {
        JsonDocument parsed;
        if (json->length() == 0 || deserializeJson(parsed, *json) || !parsed.is<JsonObject>())
            return false;
    }

    JsonDocument metadata;
    if (deserializeJson(metadata, projections.metadataJson) ||
        metadata["specification_checksum"].as<uint32_t>() != specificationChecksum)
        return false;

    const PointerRecord previous = newestPointer(root);
    const uint32_t nextSequence = previous.valid ? previous.sequence + 1 : 1;
    char generationBuffer[32] = {};
    snprintf(generationBuffer, sizeof(generationBuffer), "g%08lx_%08lx",
             static_cast<unsigned long>(nextSequence),
             static_cast<unsigned long>(specificationChecksum));
    const String generation = generationBuffer;

    // The generation name is unique, so the previous committed generation
    // remains untouched while this one is written and verified.
    if (!writeVerifiedJson(generationPath(root, generation, CAP_SUFFIX), projections.capabilitiesJson) ||
        !writeVerifiedJson(generationPath(root, generation, RUNTIME_SUFFIX), projections.runtimeManifestJson) ||
        !writeVerifiedJson(generationPath(root, generation, CONFIG_SUFFIX), projections.configurationSchemaJson) ||
        !writeVerifiedJson(generationPath(root, generation, META_SUFFIX), projections.metadataJson))
        return false;

    // Add provenance to metadata as a separate persisted commit record.
    JsonDocument pointerDocument;
    pointerDocument["sequence"] = nextSequence;
    pointerDocument["specification_checksum"] = specificationChecksum;
    pointerDocument["generation"] = generation;

    String pointerJson;
    if (serializeJson(pointerDocument, pointerJson) == 0)
        return false;

    const bool writeA = (nextSequence % 2u) == 1u;
    const String target = pointerPath(root, writeA);
    if (!writeVerifiedJson(target, pointerJson))
        return false;

    // readPointer validates the committed pointer and all referenced files are
    // already present. The other pointer slot remains as the prior generation.
    const PointerRecord committed = readPointer(target.c_str());
    if (!committed.valid || committed.sequence != nextSequence ||
        committed.checksum != specificationChecksum ||
        committed.generationName != generation)
        return false;

    state.specificationChecksum = specificationChecksum;
    state.generation = nextSequence;
    return true;
}


bool loadPersistedProductProjections(
    uint32_t expectedChecksum, ProductProjectionSet &projections,
    ProductProjectionState &state)
{
    return loadPersistedProductProjectionsAt("/product", expectedChecksum, projections, state);
}

bool persistProductProjections(
    uint32_t specificationChecksum, const ProductProjectionSet &projections,
    ProductProjectionState &state)
{
    return persistProductProjectionsAt("/product", specificationChecksum, projections, state);
}

// Runs exclusively under /product_recovery_test. The live /product generation,
// active pointers, and user configuration are never read, written, or removed.
bool runProductProjectionRecoverySelfTest(
    uint32_t specificationChecksum, const ProductProjectionSet &projections)
{
    if (!isStorageReady() || !specificationChecksum)
        return false;

    const String root = "/product_recovery_test";
    const char *suffixes[] = {CAP_SUFFIX, RUNTIME_SUFFIX, CONFIG_SUFFIX, META_SUFFIX};
    for (const char *suffix : suffixes)
    {
        // Remove only files belonging to this dedicated test namespace.
        for (uint32_t sequence = 1; sequence <= 2; ++sequence)
        {
            char name[32] = {};
            snprintf(name, sizeof(name), "g%08lx_%08lx",
                     static_cast<unsigned long>(sequence),
                     static_cast<unsigned long>(specificationChecksum));
            LittleFS.remove(generationPath(root, name, suffix));
            LittleFS.remove(generationPath(root, name, suffix) + ".tmp");
        }
    }
    LittleFS.remove(pointerPath(root, true));
    LittleFS.remove(pointerPath(root, false));
    LittleFS.remove(pointerPath(root, true) + ".tmp");
    LittleFS.remove(pointerPath(root, false) + ".tmp");
    if (!LittleFS.exists(root) && !LittleFS.mkdir(root))
        return false;

    ProductProjectionState firstState;
    if (!persistProductProjectionsAt(root, specificationChecksum, projections, firstState))
        return false;
    ProductProjectionState secondState;
    if (!persistProductProjectionsAt(root, specificationChecksum, projections, secondState))
        return false;

    // Resolve the committed pointer rather than relying on formatted path details.
    const PointerRecord newest = newestPointer(root);
    if (!newest.valid || newest.sequence != secondState.generation)
        return false;
    const String damagedFile = generationPath(root, newest.generationName, CAP_SUFFIX);
    File corrupt = LittleFS.open(damagedFile, "w");
    if (!corrupt)
        return false;
    corrupt.print("{\"deliberately_corrupt\":");
    corrupt.close();

    ProductProjectionSet recovered;
    ProductProjectionState recoveredState;
    const bool fallbackWorked = loadPersistedProductProjectionsAt(
        root, specificationChecksum, recovered, recoveredState) &&
        recoveredState.generation == firstState.generation &&
        recovered.capabilitiesJson == projections.capabilitiesJson &&
        recovered.runtimeManifestJson == projections.runtimeManifestJson &&
        recovered.configurationSchemaJson == projections.configurationSchemaJson &&
        recovered.metadataJson == projections.metadataJson;

    // Remove only the isolated test artefacts, regardless of the test outcome.
    for (const char *suffix : suffixes)
    {
        for (uint32_t sequence = 1; sequence <= 2; ++sequence)
        {
            char name[32] = {};
            snprintf(name, sizeof(name), "g%08lx_%08lx",
                     static_cast<unsigned long>(sequence),
                     static_cast<unsigned long>(specificationChecksum));
            LittleFS.remove(generationPath(root, name, suffix));
            LittleFS.remove(generationPath(root, name, suffix) + ".tmp");
        }
    }
    LittleFS.remove(pointerPath(root, true));
    LittleFS.remove(pointerPath(root, false));
    LittleFS.remove(pointerPath(root, true) + ".tmp");
    LittleFS.remove(pointerPath(root, false) + ".tmp");
    LittleFS.rmdir(root);
    return fallbackWorked;
}

} // namespace prakiva
