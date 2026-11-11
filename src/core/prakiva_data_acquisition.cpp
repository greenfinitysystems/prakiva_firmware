#include "core/prakiva_data_acquisition.h"
#include "components/core/prakiva_sensor_driver.h"
#include "core/prakiva_time.h"
#include <time.h>

namespace prakiva {
namespace {
ISensorDriver *drivers[MAX_COMPONENT_INSTANCES] = {};
size_t count = 0;
bool ready = false;
SensorSnapshot currentSnapshot;
}

bool beginDataAcquisition(ComponentRegistry &registry) {
    count = 0;
    for (size_t i=0; i<registry.instanceCount() && count<MAX_COMPONENT_INSTANCES; ++i) {
        const char *id = registry.componentIdAt(i);
        IComponent *component = id ? registry.find(id) : nullptr;
        ISensorDriver *driver = component ? component->asSensorDriver() : nullptr;
        if (!driver) continue;
        currentSnapshot.sensors[count].componentId = id;
        currentSnapshot.sensors[count].reading = SensorValue{};
        currentSnapshot.sensors[count].readSucceeded = false;
        drivers[count] = driver;
        ++count;
    }
    currentSnapshot.sensorCount = count;
    ready = true;
    Serial.printf("[DATA ACQUISITION] Ready: sensors=%u\n", static_cast<unsigned>(count));
    return true;
}

const SensorSnapshot &currentSensorSnapshot() { return currentSnapshot; }

void updateDataAcquisition() {
    if (!ready) return;
    uint32_t now=0;
    currentSnapshot.datetime = SystemDateTimeValue{};
    if (getCurrentUnixTime(now)) {
        const time_t localEpoch = static_cast<time_t>(static_cast<int64_t>(now) + static_cast<int64_t>(getScheduleOffsetMinutes())*60);
        struct tm parts;
        if (gmtime_r(&localEpoch, &parts)) {
            currentSnapshot.datetime.unixTimestamp=now;
            currentSnapshot.datetime.year=parts.tm_year+1900;
            currentSnapshot.datetime.month=parts.tm_mon+1;
            currentSnapshot.datetime.day=parts.tm_mday;
            currentSnapshot.datetime.weekday=parts.tm_wday==0?7:parts.tm_wday;
            currentSnapshot.datetime.hour=parts.tm_hour;
            currentSnapshot.datetime.minute=parts.tm_min;
            currentSnapshot.datetime.second=parts.tm_sec;
            currentSnapshot.datetime.valid=true;
        }
    }
    for (size_t i=0; i<count; ++i) {
        SensorValue latest;
        const bool succeeded = drivers[i]->get(latest);
        currentSnapshot.sensors[i].readSucceeded = succeeded;
        if (succeeded) currentSnapshot.sensors[i].reading = latest;
        else {
            currentSnapshot.sensors[i].reading.valid = false;
            currentSnapshot.sensors[i].reading.status = MeasurementStatus::Error;
        }
    }
}

size_t sensorSnapshotCount() { return ready ? count : 0; }
const SensorSnapshotEntry *sensorSnapshotAt(size_t index) { return (ready && index<count) ? &currentSnapshot.sensors[index] : nullptr; }
const SensorSnapshotEntry *findSensorSnapshot(const char *id) {
    if (!ready || !id) return nullptr;
    for (size_t i=0; i<count; ++i) if (currentSnapshot.sensors[i].componentId == id) return &currentSnapshot.sensors[i];
    return nullptr;
}
DataAcquisitionService data_acquisition;
bool DataAcquisitionService::begin(ComponentRegistry &registry) { return beginDataAcquisition(registry); }
void DataAcquisitionService::get() { updateDataAcquisition(); }

}
