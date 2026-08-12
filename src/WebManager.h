#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "SensorsManager.h"

class HueManager; // Forward declaration

class WebManager {
public:
    WebManager(SensorsManager* sensors, HueManager* hue);
    void begin();
    void update(); // Controlla e applica i riavvii
    
    bool isAPMode() const;
    String getSSID() const;
    String getIP() const;

private:
    SensorsManager* _sensors;
    HueManager* _hue; // NUOVO PUNTATORE
    AsyncWebServer* _server;
    Preferences _preferences;
    
    bool _shouldReboot = false;
    
    // Hue state
    String _hueBridgeIp = "";
    String _hueToken = "";
    String _hueSelectedLights = "";
    bool _isHueConfigured = false;

    void setupWiFi();
    void setupRoutes();
};