#pragma once
#include <Arduino.h>

namespace prakiva
{
  bool beginWifiApi();
  void handleWifiApi();
  const char *wifiModeName();
  String wifiStatusName();
  bool configureWifi(const String &type, const String &ssid, const String &password, String &errorCode, String &errorMessage);
  void resetWifiToFactory();
}
