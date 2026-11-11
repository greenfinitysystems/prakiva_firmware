#pragma once
#include <Arduino.h>
#include <stddef.h>
#include "components/core/prakiva_component_registry.h"
#include "components/core/prakiva_component_types.h"

namespace prakiva {
struct SystemDateTimeValue {
    uint64_t unixTimestamp = 0;
    int year = 0; int month = 0; int day = 0;
    int weekday = 0; // Monday=1 ... Sunday=7
    int hour = 0; int minute = 0; int second = 0;
    bool valid = false;
};

struct SensorSnapshotEntry {
    String componentId;
    SensorValue reading;
    bool readSucceeded = false;
};

struct SensorSnapshot {
    SystemDateTimeValue datetime;
    SensorSnapshotEntry sensors[MAX_COMPONENT_INSTANCES];
    size_t sensorCount = 0;
};

class DataAcquisitionService {
public:
    bool begin(ComponentRegistry &registry);
    void get();
};
extern DataAcquisitionService data_acquisition;
const SensorSnapshot &currentSensorSnapshot();
void updateDataAcquisition();
size_t sensorSnapshotCount();
const SensorSnapshotEntry *sensorSnapshotAt(size_t index);
const SensorSnapshotEntry *findSensorSnapshot(const char *componentId);
}
