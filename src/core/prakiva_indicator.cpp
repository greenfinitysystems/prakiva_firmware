#include "core/prakiva_indicator.h"

#include <Adafruit_NeoPixel.h>

namespace prakiva
{
    namespace
    {
        constexpr uint8_t RGB_PIN = 48; // ESP32-S3 onboard addressable RGB LED
        constexpr uint8_t PIXEL_COUNT = 1;
        constexpr uint8_t BRIGHTNESS = 32;

        // All pulse patterns use the same timing so they can be learned reliably:
        // 120 ms ON, 180 ms OFF between flashes, 2 s total cycle.
        constexpr uint32_t PULSE_ON_MS = 120;
        constexpr uint32_t PULSE_OFF_MS = 180;
        constexpr uint32_t CYCLE_MS = 2000;

        Adafruit_NeoPixel pixel(PIXEL_COUNT, RGB_PIN, NEO_GRB + NEO_KHZ800);
        IndicatorState currentState = IndicatorState::Booting;
        IndicatorFault currentFault = IndicatorFault::None;
        uint32_t patternStartedAt = 0;
        bool initialized = false;
        bool hasRendered = false;
        bool lastRenderedOn = false;
        uint32_t lastRenderedColor = 0;

        uint32_t colorForState(IndicatorState state)
        {
            switch (state)
            {
            case IndicatorState::Booting:
            case IndicatorState::WifiConnecting:
                return pixel.Color(0, 0, 255); // Blue
            case IndicatorState::Operational:
                return pixel.Color(0, 180, 0); // Green
            case IndicatorState::WifiReconnecting:
                return pixel.Color(255, 80, 0); // Amber
            case IndicatorState::RecoveryAccessPoint:
                return pixel.Color(0, 180, 220); // Cyan
            }
            return 0;
        }

        uint8_t pulseCountForState(IndicatorState state)
        {
            switch (state)
            {
            case IndicatorState::Booting: return 1;
            case IndicatorState::WifiConnecting: return 2;
            case IndicatorState::WifiReconnecting: return 1;
            case IndicatorState::RecoveryAccessPoint: return 2;
            case IndicatorState::Operational: return 0;
            }
            return 0;
        }

        bool pulsePatternIsOn(uint32_t elapsed, uint8_t pulseCount)
        {
            if (pulseCount == 0)
                return false;

            const uint32_t cyclePosition = elapsed % CYCLE_MS;
            for (uint8_t pulse = 0; pulse < pulseCount; ++pulse)
            {
                const uint32_t pulseStart = static_cast<uint32_t>(pulse) * (PULSE_ON_MS + PULSE_OFF_MS);
                if (cyclePosition >= pulseStart && cyclePosition < pulseStart + PULSE_ON_MS)
                    return true;
            }
            return false;
        }

        uint32_t colorForFault()
        {
            return pixel.Color(255, 0, 0); // Red
        }

        uint8_t pulseCountForFault(IndicatorFault fault)
        {
            return static_cast<uint8_t>(fault);
        }

        void render(bool on, uint32_t color)
        {
            if (hasRendered && on == lastRenderedOn && color == lastRenderedColor)
                return;
            lastRenderedOn = on;
            lastRenderedColor = color;
            hasRendered = true;
            pixel.setPixelColor(0, on ? color : 0);
            pixel.show();
        }
    }

    void beginIndicator()
    {
        pixel.begin();
        pixel.setBrightness(BRIGHTNESS);
        pixel.clear();
        pixel.show();
        initialized = true;
        hasRendered = false;
        currentState = IndicatorState::Booting;
        currentFault = IndicatorFault::None;
        patternStartedAt = millis();
    }

    void setIndicatorState(IndicatorState state)
    {
        if (currentState == state)
            return;
        currentState = state;
        patternStartedAt = millis();
    }

    void setIndicatorFault(IndicatorFault fault)
    {
        if (fault == IndicatorFault::None)
        {
            clearIndicatorFault();
            return;
        }
        if (currentFault == fault)
            return;
        currentFault = fault;
        patternStartedAt = millis();
    }

    void clearIndicatorFault()
    {
        if (currentFault == IndicatorFault::None)
            return;
        currentFault = IndicatorFault::None;
        patternStartedAt = millis();
    }

    void updateIndicator()
    {
        if (!initialized)
            return;

        if (currentFault != IndicatorFault::None)
        {
            const uint32_t elapsed = millis() - patternStartedAt;
            render(pulsePatternIsOn(elapsed, pulseCountForFault(currentFault)), colorForFault());
            return;
        }

        if (currentState == IndicatorState::Operational)
        {
            render(true, colorForState(currentState));
            return;
        }

        const uint32_t elapsed = millis() - patternStartedAt;
        render(pulsePatternIsOn(elapsed, pulseCountForState(currentState)), colorForState(currentState));
    }
}
