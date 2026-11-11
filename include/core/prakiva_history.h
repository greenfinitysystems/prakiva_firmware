#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <stddef.h>
#include "components/core/prakiva_component_registry.h"

namespace prakiva {
class HistoryService {
public:
    bool begin(ComponentRegistry &registry, const String &runtimeManifestJson);
    void record();
    void flush();
    bool appendSensorHistory(const char *componentId, size_t limit, JsonArray output);
};
extern HistoryService history;
}
