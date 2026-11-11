#include <Arduino.h>

#include "core/prakiva_config.h"
#include "core/prakiva_identity.h"
#include "core/prakiva_wifi.h"
#include "core/prakiva_time.h"
#include "core/prakiva_storage.h"
#include "core/prakiva_indicator.h"
#include "core/prakiva_data_acquisition.h"
#include "core/prakiva_history.h"

#include "product/prakiva_product_runtime.h"
#include "components/core/prakiva_actuator_driver.h"
#include "scheduler/prakiva_scheduler.h"

#include <LittleFS.h>

// The editor configuration is nested JSON; allow adequate stack for bounded parsing.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

namespace
{
    constexpr unsigned long SERIAL_BAUD_RATE = 115200;

} // namespace

void setup()
{
    Serial.begin(SERIAL_BAUD_RATE);
    delay(500);

    Serial.println();
    Serial.println("Prakiva controller");
    Serial.println("==================");
    prakiva::beginIndicator();
    prakiva::updateIndicator();

    if (!prakiva::beginIdentity())
    {
        Serial.println("[IDENTITY] Initialization FAILED");
        prakiva::setIndicatorFault(prakiva::IndicatorFault::IdentityInitialization);
        return;
    }

    if (!prakiva::beginConfig())
    {
        Serial.println("[CONFIG] Initialization FAILED");
        prakiva::setIndicatorFault(prakiva::IndicatorFault::ConfigurationInitialization);
        return;
    }

    if (!prakiva::beginTime())
    {
        Serial.println("[TIME] Initialization FAILED; continuing without time settings");
    }

    if (!prakiva::beginStorage())
    {
        Serial.println("[STORAGE] Initialization FAILED; configuration persistence unavailable");
        prakiva::setIndicatorFault(prakiva::IndicatorFault::StorageInitialization);
    }

    // LittleFS.remove("/configuration.json");
    // LittleFS.remove("/configuration.bak");
    // LittleFS.remove("/configuration.tmp");

    if (!prakiva::beginProductRuntime())
    {
        Serial.println("[PRODUCT RUNTIME] Initialization FAILED");
        return;
    }

    if (!prakiva::data_acquisition.begin(prakiva::productComponentRegistry()))
    {
        Serial.println("[DATA ACQUISITION] Initialization FAILED");
    }
    if (!prakiva::history.begin(prakiva::productComponentRegistry(), prakiva::activeProductProjections().runtimeManifestJson))
    {
        Serial.println("[HISTORY] Initialization FAILED; controller will continue without history");
    }

    if (!prakiva::scheduler.init())
    {
        Serial.println("[SCHEDULER] Initialization FAILED; controller will continue without policy execution");
    }

    if (!prakiva::beginWifiApi())
    {
        Serial.println("[WIFI] Initialization FAILED");
        return;
    }
}

// Progress actuator-owned operations independently of policy evaluation.
// Registry owns the drivers; no additional actuator state or allocations are needed.
static void updateActuators()
{
    auto &registry = prakiva::productComponentRegistry();
    for (size_t i = 0; i < registry.instanceCount(); ++i)
    {
        const char *id = registry.componentIdAt(i);
        if (!id || registry.getStatus(id) != prakiva::ComponentStatus::Ready)
            continue;
        auto *component = registry.find(id);
        auto *actuator = component ? component->asActuatorDriver() : nullptr;
        if (actuator)
            actuator->update();
    }
}

void loop()
{
    updateActuators();
    prakiva::data_acquisition.get();
    prakiva::history.record();
    prakiva::history.flush();
    prakiva::scheduler.step();
    prakiva::handleWifiApi();
    prakiva::updateIndicator();
    delay(2);
}
