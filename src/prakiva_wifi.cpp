#include "prakiva_wifi.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Preferences.h>
#include "prakiva_identity.h"
#include "prakiva_protocol.h"

namespace prakiva
{
    namespace
    {
        constexpr char WIFI_NS[] = "prakwifi";
        constexpr char TYPE_KEY[] = "type";
        constexpr char SSID_KEY[] = "ssid";
        constexpr char PASS_KEY[] = "password";
        constexpr char FACTORY_SSID[] = "Prakiva WiFi";
        // DEVELOPMENT DEFAULT ONLY. Production firmware must provision a unique per-device password.
        constexpr char FACTORY_PASSWORD[] = "PrakivaSetup";
        constexpr uint16_t HTTP_PORT = 8080, DISCOVERY_PORT = 4210;
        constexpr char DISCOVERY_REQUEST[] = "PRAKIVA_DISCOVER_V1";
        constexpr uint8_t BOOT_CONNECT_ATTEMPTS = 5;
        constexpr uint32_t CONNECT_TIMEOUT_MS = 5000;
        constexpr uint32_t RETRY_INTERVAL_MS = 10000;
        constexpr uint32_t MODE_SWITCH_DELAY_MS = 1800;
        // ESP32 DevKit V1 common onboard user LED. If this clone uses another pin,
        // change WIFI_STATUS_LED_PIN after testing. The expansion-board PWR LED is untouched.
        constexpr uint8_t WIFI_STATUS_LED_PIN = 2;
        constexpr bool WIFI_STATUS_LED_ACTIVE_LOW = false;
        constexpr uint32_t LED_SLOW_BLINK_MS = 700;
        constexpr uint32_t LED_FAST_BLINK_MS = 180;
        constexpr uint32_t LED_PATTERN_PERIOD_MS = 1800;
        const IPAddress AP_IP(192, 168, 10, 1), AP_GATEWAY(192, 168, 10, 1), AP_SUBNET(255, 255, 255, 0);
        WebServer server(HTTP_PORT);
        WiFiUDP discoveryUdp;
        bool wifiApiReady = false, apActive = false, pendingSwitch = false, configuredSta = false;
        String savedType = "AP", savedSsid = FACTORY_SSID, savedPassword = FACTORY_PASSWORD;
        String pendingType, pendingSsid, pendingPassword;
        uint32_t pendingAt = 0, nextRetryAt = 0;
        String statusText = "starting";
        uint32_t ledPatternStartedAt = 0;
        uint32_t ledLastToggleAt = 0;
        bool ledOutputOn = false;

        void setStatus(const String &value)
        {
            if (statusText != value)
            {
                statusText = value;
                ledPatternStartedAt = millis();
                ledLastToggleAt = 0;
            }
        }
        void writeStatusLed(bool on)
        {
            ledOutputOn = on;
            digitalWrite(WIFI_STATUS_LED_PIN, (on != WIFI_STATUS_LED_ACTIVE_LOW) ? HIGH : LOW);
        }
        void beginStatusLed()
        {
            pinMode(WIFI_STATUS_LED_PIN, OUTPUT);
            writeStatusLed(false);
            ledPatternStartedAt = millis();
        }
        void updateStatusLed()
        {
            const uint32_t now = millis();
            bool on = false;
            if (statusText == "connected")
                on = true;
            else if (statusText == "connecting")
                on = ((now / LED_SLOW_BLINK_MS) % 2) == 0;
            else if (statusText == "reconnecting")
                on = ((now / LED_FAST_BLINK_MS) % 2) == 0;
            else if (statusText == "recovery_ap")
            {
                const uint32_t phase = (now - ledPatternStartedAt) % LED_PATTERN_PERIOD_MS;
                on = (phase < 120 || (phase >= 260 && phase < 380));
            }
            else if (statusText == "configuration_error")
            {
                const uint32_t phase = (now - ledPatternStartedAt) % LED_PATTERN_PERIOD_MS;
                on = (phase < 100 || (phase >= 220 && phase < 320) || (phase >= 440 && phase < 540));
            }
            else if (statusText == "switching_network")
                on = ((now / 250) % 2) == 0;
            writeStatusLed(on);
        }

        void loadConfig()
        {
            Preferences p;
            if (!p.begin(WIFI_NS, true))
                return;
            String t = p.getString(TYPE_KEY, "AP"), s = p.getString(SSID_KEY, FACTORY_SSID), pw = p.getString(PASS_KEY, FACTORY_PASSWORD);
            p.end();
            if ((t == "AP" || t == "STA") && !s.isEmpty() && (t != "STA" || !s.isEmpty()))
            {
                savedType = t;
                savedSsid = s;
                savedPassword = pw;
            }
        }
        bool persistConfig(const String &t, const String &s, const String &pw)
        {
            Preferences p;
            if (!p.begin(WIFI_NS, false))
                return false;

            bool ok = p.putString(TYPE_KEY, t) > 0 &&
                p.putString(SSID_KEY, s) > 0 &&
                static_cast<int>(p.putString(PASS_KEY, pw)) >= 0 &&
                p.getString(TYPE_KEY, "") == t &&
                p.getString(SSID_KEY, "") == s &&
                p.getString(PASS_KEY, "") == pw;

            return ok;

            // bool ok = p.putString(TYPE_KEY, t) > 0 && p.putString(SSID_KEY, s) > 0 && p.putString(PASS_KEY, pw) >= 0;


            // String vt = p.getString(TYPE_KEY, "");
            // String vs = p.getString(SSID_KEY, "");
            // String vp = p.getString(PASS_KEY, "\x01");
            // p.end();
            // return ok && vt == t && vs == s && vp == pw;
        }
        void startAp(const String &ssid, const String &password)
        {
            WiFi.disconnect(false, false);
            WiFi.mode(WIFI_AP);
            WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
            if (!WiFi.softAP(ssid.c_str(), password.c_str(), 1, false, 4))
            {
                setStatus("configuration_error");
                Serial.println("[WIFI] AP start failed");
                return;
            }
            apActive = true;
            configuredSta = false;
            setStatus("recovery_ap");
            Serial.printf("[WIFI] AP started: %s at %s\n", ssid.c_str(), WiFi.softAPIP().toString().c_str());
        }
        bool trySta(const String &ssid, const String &password, uint32_t timeout)
        {
            WiFi.begin(ssid.c_str(), password.c_str());
            uint32_t start = millis();
            while (WiFi.status() != WL_CONNECTED && millis() - start < timeout)
            {
                delay(100);
            }
            return WiFi.status() == WL_CONNECTED;
        }
        void handleHealth()
        {
            String body = "{\"status\":\"ok\",\"wifi_status\":\"" + statusText + "\"}";
            server.send(200, "application/json", body);
        }
        void handleProtocol()
        {
            if (!server.hasArg("plain"))
            {
                server.send(400, "application/json", "{\"status\":\"error\",\"error\":{\"code\":\"MISSING_BODY\",\"message\":\"A JSON request body is required.\"}}");
                return;
            }
            String response = handleProtocolRequest(server.arg("plain"));
            server.send(200, "application/json", response);
        }
        void handleDiscovery()
        {
            int packetSize = discoveryUdp.parsePacket();
            if (packetSize <= 0)
                return;
            constexpr size_t N = 64;
            if (packetSize >= N)
            {
                while (discoveryUdp.available())
                    discoveryUdp.read();
                return;
            }
            char packet[N];
            int n = discoveryUdp.read(packet, packetSize);
            if (n <= 0)
                return;
            packet[n] = '\0';
            if (strcmp(packet, DISCOVERY_REQUEST) != 0)
                return;
            String response = "{\"type\":\"prakiva_discovery_response\",\"protocol_version\":\"0.1\",\"device_id\":\"" + String(getDeviceId()) + "\",\"http_port\":" + String(HTTP_PORT) + ",\"wifi_status\":\"" + statusText + "\"}";
            discoveryUdp.beginPacket(discoveryUdp.remoteIP(), discoveryUdp.remotePort());
            discoveryUdp.write((const uint8_t *)response.c_str(), response.length());
            discoveryUdp.endPacket();
        }
        void performPendingSwitch()
        {
            if (!pendingSwitch || (uint32_t)(millis() - pendingAt) < MODE_SWITCH_DELAY_MS)
                return;
            pendingSwitch = false;
            if (pendingType == "AP")
            {
                savedType = pendingType;
                savedSsid = pendingSsid;
                savedPassword = pendingPassword;
                startAp(savedSsid, savedPassword);
                return;
            }
            WiFi.softAPdisconnect(true);
            WiFi.mode(WIFI_STA);
            WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
            apActive = false;
            configuredSta = true;
            setStatus("connecting");
            nextRetryAt = millis() + RETRY_INTERVAL_MS;
            Serial.printf("[WIFI] Switching to STA: %s\n", savedSsid.c_str());
        }
    } // namespace

    bool beginWifiApi()
    {
        if (wifiApiReady)
            return true;
        beginStatusLed();
        loadConfig();
        if (savedType == "STA")
        {
            WiFi.mode(WIFI_STA);
            setStatus("connecting");
            bool connected = false;
            for (uint8_t i = 0; i < BOOT_CONNECT_ATTEMPTS && !connected; i++)
            {
                Serial.printf("[WIFI] Boot connection attempt %u/%u\n", i + 1, BOOT_CONNECT_ATTEMPTS);
                connected = trySta(savedSsid, savedPassword, CONNECT_TIMEOUT_MS);
            }
            if (connected)
            {
                configuredSta = true;
                setStatus("connected");
                Serial.printf("[WIFI] Connected, IP: %s\n", WiFi.localIP().toString().c_str());
            }
            else
            {
                Serial.println("[WIFI] Boot attempts exhausted; starting factory recovery AP");
                startAp(FACTORY_SSID, FACTORY_PASSWORD);
            }
        }
        else
            startAp(savedSsid, savedPassword);
        server.on("/health", HTTP_GET, handleHealth);
        server.on("/api/v1", HTTP_POST, handleProtocol);
        server.onNotFound([]()
                          { server.send(404, "application/json", "{\"status\":\"error\",\"error\":{\"code\":\"NOT_FOUND\",\"message\":\"Endpoint not found.\"}}"); });
        server.begin();
        if (!discoveryUdp.begin(DISCOVERY_PORT))
        {
            Serial.println("[DISCOVERY] UDP begin failed");
            return false;
        }
        wifiApiReady = true;
        return true;
    }
    void handleWifiApi()
    {
        if (!wifiApiReady)
            return;
        performPendingSwitch();
        if (configuredSta && !pendingSwitch)
        {
            if (WiFi.status() == WL_CONNECTED)
            {
                setStatus("connected");
            }
            else
            {
                setStatus("reconnecting");
                if ((int32_t)(millis() - nextRetryAt) >= 0)
                {
                    WiFi.reconnect();
                    nextRetryAt = millis() + RETRY_INTERVAL_MS;
                }
            }
        }
        server.handleClient();
        handleDiscovery();
        updateStatusLed();
    }
    const char *wifiModeName() { return savedType.c_str(); }
    String wifiStatusName() { return statusText; }
    bool configureWifi(const String &type, const String &ssid, const String &password, String &errorCode, String &errorMessage)
    {
        if (type != "AP" && type != "STA")
        {
            errorCode = "INVALID_WIFI_TYPE";
            errorMessage = "wifi_type must be AP or STA.";
            return false;
        }
        if (ssid.isEmpty() || ssid.length() > 32)
        {
            errorCode = "INVALID_SSID";
            errorMessage = "SSID must contain 1 to 32 characters.";
            return false;
        }
        if (password.length() > 63 || (type == "AP" && password.length() > 0 && password.length() < 8))
        {
            errorCode = "INVALID_PASSWORD";
            errorMessage = "Password must be empty or at least 8 characters for AP mode, and no more than 63 characters.";
            return false;
        }
        if (type == "AP")
        {
            if (!persistConfig("AP", ssid, password))
            {
                errorCode = "STORAGE_FAILURE";
                errorMessage = "Wi-Fi settings could not be saved.";
                return false;
            }
            pendingType = "AP";
            pendingSsid = ssid;
            pendingPassword = password;
            pendingSwitch = true;
            pendingAt = millis();
            setStatus("switching_network");
            errorCode = "";
            errorMessage = "";
            return true;
        }
        // Disconnect any existing station first so WL_CONNECTED cannot accidentally
        // make the proposed credentials appear successful.
        WiFi.disconnect(false, false);
        WiFi.mode(WIFI_AP_STA);
        delay(100);
        WiFi.begin(ssid.c_str(), password.c_str());
        const uint32_t started = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - started < 20000)
        {
            delay(100);
        }
        if (WiFi.status() != WL_CONNECTED)
        {
            Serial.printf("[WIFI] Candidate network failed: %s\n", ssid.c_str());
            // Do not persist the failed candidate. Restore a known way to reach the device.
            startAp(FACTORY_SSID, FACTORY_PASSWORD);
            errorCode = "WIFI_CONNECTION_FAILED";
            errorMessage = "Could not connect to that network. Credentials were not saved; connect to the Prakiva recovery WiFi and try again.";
            return false;
        }
        if (!persistConfig("STA", ssid, password))
        {
            Serial.println("[WIFI] Connected to candidate, but persistence verification failed");
            startAp(FACTORY_SSID, FACTORY_PASSWORD);
            errorCode = "STORAGE_FAILURE";
            errorMessage = "Wi-Fi connection succeeded, but settings could not be verified in storage. Factory recovery WiFi has been started.";
            return false;
        }
        savedType = "STA";
        savedSsid = ssid;
        savedPassword = password;
        pendingType = "STA";
        pendingSsid = ssid;
        pendingPassword = password;
        pendingSwitch = true;
        pendingAt = millis();
        setStatus("switching_network");
        errorCode = "";
        errorMessage = "";
        return true;
    }
    void resetWifiToFactory()
    {
        if (persistConfig("AP", FACTORY_SSID, FACTORY_PASSWORD))
        {
            savedType = "AP";
            savedSsid = FACTORY_SSID;
            savedPassword = FACTORY_PASSWORD;
            pendingType = "AP";
            pendingSsid = savedSsid;
            pendingPassword = savedPassword;
            pendingSwitch = true;
            pendingAt = millis();
            setStatus("switching_network");
        }
    }
} // namespace prakiva
