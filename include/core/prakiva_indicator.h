#pragma once

#include <Arduino.h>

namespace prakiva
{
    // Controller lifecycle states. A latched fault always takes precedence.
    enum class IndicatorState : uint8_t
    {
        Booting = 0,
        WifiConnecting,
        Operational,
        WifiReconnecting,
        RecoveryAccessPoint,
    };

    // Stable, user-documentable controller fault identifiers.
    enum class IndicatorFault : uint8_t
    {
        None = 0,
        IdentityInitialization = 1, // E001
        ConfigurationInitialization = 2, // E002
        StorageInitialization = 3, // E003
        WifiServiceInitialization = 4, // E004
        DiscoveryInitialization = 5, // E005
    };

    void beginIndicator();
    void setIndicatorState(IndicatorState state);
    void setIndicatorFault(IndicatorFault fault);
    void clearIndicatorFault();
    void updateIndicator();
}
