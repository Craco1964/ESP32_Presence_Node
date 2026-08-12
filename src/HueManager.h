#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <vector>
#include <ArduinoJson.h>
#include "SensorsManager.h"

// Struttura che mappa la regola che ci arriva da React
struct TimeRule {
    int startMins;     // Orario di inizio (es. "23:00" -> 1380 minuti)
    int endMins;       // Orario di fine (es. "07:00" -> 420 minuti)
    String action;     // "on_off", "on_only", "off_only"
    int brightness;    // 1-100
    String color;
};

class HueManager {
public:
    HueManager(SensorsManager* sensors);
    void begin();
    void update();
    void reloadConfig();
    
    String getActiveColor() const { return _currentColor; }
    void forceLightsState(bool state, int brightness = -1, String hexColor = "");

    bool isLightOn() const { return _lightIsOn; }
    String getActiveRuleName() const { return _currentRuleName; }
    void adjustBrightness(int deltaPercent);

    bool isAutomationEnabled() const { return _automationEnabled; }
    void setAutomationEnabled(bool state);

private:
    SensorsManager* _sensors;
    Preferences _preferences;

    // Configurazione Hue
    String _bridgeIp;
    String _token;
    String _lights; // Formato CSV es. "1,2,3"
    bool _isHueConfigured;
    String _currentRuleName = "Inizializzazione...";
    String _currentColor = "#ffffff";

    // Configurazione Radar e Regole
    int _sensitivity; 
    int _timeoutSec;
    bool _automationEnabled;
    std::vector<TimeRule> _rules; // Contenitore delle fasce orarie

    // Stato logico
    bool _lightIsOn;
    unsigned long _lastValidPresenceTime;
    bool _timeoutExpiredCommandSent;
    
    // Funzione helper per le regole
    bool isRuleActive(const TimeRule& rule, int currentMins);
    // Funzione helper matematica
    String hexToXY(String hexColor);
};