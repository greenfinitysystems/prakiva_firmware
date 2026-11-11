#include "core/prakiva_wifi.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Preferences.h>
#include "core/prakiva_identity.h"
#include "core/prakiva_protocol.h"
#include "core/prakiva_indicator.h"

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

        const IPAddress AP_IP(192, 168, 10, 1), AP_GATEWAY(192, 168, 10, 1), AP_SUBNET(255, 255, 255, 0);
        WebServer server(HTTP_PORT);
        WiFiUDP discoveryUdp;
        bool wifiApiReady = false, apActive = false, pendingSwitch = false, configuredSta = false;
        String savedType = "AP", savedSsid = FACTORY_SSID, savedPassword = FACTORY_PASSWORD;
        String pendingType, pendingSsid, pendingPassword;
        uint32_t pendingAt = 0, nextRetryAt = 0;
        String statusText = "starting";

        void setStatus(const String &value)
        {
            if (statusText != value)
            {
                statusText = value;
                if (value == "connected")
                    setIndicatorState(IndicatorState::Operational);
                else if (value == "connecting" || value == "switching_network")
                    setIndicatorState(IndicatorState::WifiConnecting);
                else if (value == "reconnecting")
                    setIndicatorState(IndicatorState::WifiReconnecting);
                else if (value == "recovery_ap")
                    setIndicatorState(IndicatorState::RecoveryAccessPoint);
                else if (value == "configuration_error")
                    setIndicatorFault(IndicatorFault::WifiServiceInitialization);
            }
        }

        void loadConfig()
        {
            Preferences p;
            if (!p.begin(WIFI_NS, true))
                return;
            String t = p.getString(TYPE_KEY, "AP"), s = p.getString(SSID_KEY, FACTORY_SSID), pw = p.getString(PASS_KEY, FACTORY_PASSWORD);
            p.end();
            if ((t == "AP" || t == "STA") && !s.isEmpty())
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

            p.putString(TYPE_KEY, t);
            p.putString(SSID_KEY, s);
            p.putString(PASS_KEY, pw);

            const bool ok =
                p.getString(TYPE_KEY, "") == t &&
                p.getString(SSID_KEY, "") == s &&
                p.getString(PASS_KEY, "") == pw;

            p.end();
            return ok;
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
        // void handleProtocol()
        // {
        //     if (!server.hasArg("plain"))
        //     {
        //         server.send(400, "application/json", "{\"status\":\"error\",\"error\":{\"code\":\"MISSING_BODY\",\"message\":\"A JSON request body is required.\"}}");
        //         return;
        //     }
        //     String response = handleProtocolRequest(server.arg("plain"));
        //     server.send(200, "application/json", response);
        // }
        void handleProtocol()
        {
            if (!server.hasArg("plain"))
            {
                server.send(400, "application/json", "{\"status\":\"error\",\"error\":{\"code\":\"MISSING_BODY\",\"message\":\"A JSON request body is required.\"}}");
                return;
            }

            String requestBody = server.arg("plain");

            String response = handleProtocolRequest(requestBody);
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
            Serial.println("[WIFI] Switching to station mode");
        }
    } // namespace

    bool beginWifiApi()
    {
        if (wifiApiReady)
            return true;
        setIndicatorState(IndicatorState::WifiConnecting);
        loadConfig();
        if (savedType == "STA")
        {
            WiFi.mode(WIFI_STA);
            setStatus("connecting");
            bool connected = false;
            for (uint8_t i = 0; i < BOOT_CONNECT_ATTEMPTS && !connected; i++)
            {
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
            setIndicatorFault(IndicatorFault::DiscoveryInitialization);
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
