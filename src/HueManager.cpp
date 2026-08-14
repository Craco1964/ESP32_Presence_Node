#include "HueManager.h"
#include <WiFiClientSecure.h> 
#include <time.h>

HueManager::HueManager(SensorsManager* sensors) : _sensors(sensors) {
    _lightIsOn = false;
    _lastValidPresenceTime = 0;
    _timeoutExpiredCommandSent = false;
}

void HueManager::begin() {
    reloadConfig();
}

void HueManager::reloadConfig() {
    _preferences.begin("hue_cfg", true);
    _bridgeIp = _preferences.getString("ip", "");
    _token = _preferences.getString("token", "");
    _lights = _preferences.getString("lights", "");
    _preferences.end();

    _isHueConfigured = (_bridgeIp != "" && _token != "");

    _preferences.begin("radar_cfg", true);
    _sensitivity = _preferences.getInt("sensitivity", 250);
    _timeoutSec = _preferences.getInt("timeout", 60);
    _automationEnabled = _preferences.getBool("automation", true);
    _ignoreIfOn = _preferences.getBool("ignoreIfOn", false); // Carica il nuovo flag
    
    String rulesStr = _preferences.getString("rules", "[]");
    _preferences.end();

    _rules.clear();
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, rulesStr);
    
    if (!error && doc.is<JsonArray>()) {
        JsonArray arr = doc.as<JsonArray>();
        for (JsonObject r : arr) {
            TimeRule rule;
            String startStr = r["start"].as<String>(); 
            String endStr = r["end"].as<String>();     
            
            rule.startMins = startStr.substring(0, 2).toInt() * 60 + startStr.substring(3, 5).toInt();
            rule.endMins = endStr.substring(0, 2).toInt() * 60 + endStr.substring(3, 5).toInt();
            rule.action = r["action"].as<String>();
            rule.brightness = r["brightness"].as<int>();
            
            rule.color = r["color"].as<String>();
            if (rule.color == "") rule.color = "#ffffff"; 

            _rules.push_back(rule);
        }
        Serial.printf("💾 [HUE-CONFIG] Caricate %d regole orarie dalla memoria flash.\n", _rules.size());
    } else {
        Serial.println("⚠️ [HUE-CONFIG] Errore nel parsing delle regole orarie o memoria vuota.");
    }
}

bool HueManager::isRuleActive(const TimeRule& rule, int currentMins) {
    // 👇 CORREZIONE BUG SINGOLA REGOLA 👇
    if (rule.startMins == rule.endMins) {
        return true; // Se inizio e fine coincidono (singola regola), è attiva 24h su 24
    } else if (rule.startMins < rule.endMins) {
        return (currentMins >= rule.startMins && currentMins < rule.endMins);
    } else {
        // Passaggio oltre la mezzanotte
        return (currentMins >= rule.startMins || currentMins < rule.endMins);
    }
}

void HueManager::update() {
    if (!_isHueConfigured) return; // Se non c'è IP, esci

    // SE L'AUTOMAZIONE È DISABILITATA MANUALMENTE
    if (!_automationEnabled) {
        _currentRuleName = "MANUALE (Tocca qui)";
        return; // Esce senza far intervenire il radar
    }
    
    struct tm timeinfo;
    int currentMins = -1; 
    if (getLocalTime(&timeinfo, 10)) { 
        currentMins = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    }

    if (currentMins < 0 && !_rules.empty()) {
        return; 
    }

    // --- 2. Troviamo se c'è una regola attiva in questo momento ---
    String activeAction = "on_off"; 
    int activeBrightness = -1;      
    String activeColor = "#ffffff"; // Default di sicurezza
    _currentRuleName = "Auto: ON & OFF (24/7)"; 
    
    if (currentMins >= 0) {
        for (const auto& rule : _rules) {
            if (isRuleActive(rule, currentMins)) {
                activeAction = rule.action;
                activeBrightness = rule.brightness;
                activeColor = rule.color; 
                
                if (activeAction == "on_off") _currentRuleName = "Auto: ON & OFF";
                else if (activeAction == "on_only") _currentRuleName = "Auto: Solo Accensione";
                else if (activeAction == "off_only") _currentRuleName = "Auto: Solo Spegnimento";
                else if (activeAction == "do_nothing") _currentRuleName = "Auto: Nessuna Azione"; // <-- NUOVO TESTO
                
                break; 
            }
        }
    }

    _currentColor = activeColor;

    // --- LOG DIAGNOSTICO DI CONFIGURAZIONE (Ogni 5 secondi) ---
    static unsigned long lastDebugTime = 0;
    if (millis() - lastDebugTime > 5000) {
        Serial.println("----------------------------------------");
        if (currentMins >= 0) {
            Serial.printf("⏰ [HUE-ORARIO] Ora ESP32: %02d:%02d | Regole totali: %d\n", timeinfo.tm_hour, timeinfo.tm_min, _rules.size());
            Serial.printf("🎯 [HUE-REGOLA] Azione: %s | Lum: %d%% | Colore della fascia: %s\n", activeAction.c_str(), activeBrightness, activeColor.c_str());
        }
        lastDebugTime = millis();
    }

    int radarStatus = _sensors->getRadarStatus(); 
    int distance = _sensors->getRadarDistance();
    if (radarStatus == 0) distance = 0; 

    bool currentValidPresence = (radarStatus > 0 && distance <= _sensitivity);

    // 👇 --- NOVITÀ: REGOLA "NESSUNA AZIONE" --- 👇
    if (activeAction == "do_nothing") {
        if (currentValidPresence) _lastValidPresenceTime = millis(); // Traccia comunque la presenza per sicurezza
        return; // Blocca l'esecuzione: ignora il radar per questa fascia oraria!
    }

    // --- 4. MOTORE DELLE AZIONI ---
    if (currentValidPresence) {
        _lastValidPresenceTime = millis(); 
        _timeoutExpiredCommandSent = false; 
        
        if (!_lightIsOn) {
            if (activeAction == "on_off" || activeAction == "on_only") {
                
                // 👇 --- NOVITÀ: RISPETTO PER IL TELECOMANDO --- 👇
                bool skipOnCommand = false;
                if (_ignoreIfOn) {
                    if (isPhysicallyOn()) { // Interroga il bridge
                        Serial.println("💡 [AUTO-HUE] Luce già accesa fisicamente! Ignoro per non sovrascrivere il telecomando.");
                        skipOnCommand = true;
                        _lightIsOn = true; // Sincronizza lo stato interno dell'ESP32
                    }
                }

                if (!skipOnCommand) {
                    Serial.printf("\n💡 [AUTO-HUE] Presenza rilevata! Accendo col colore della fascia: %s\n", activeColor.c_str());
                    forceLightsState(true, activeBrightness, activeColor);
                }
            }
        }
    } else {
        unsigned long elapsedMs = millis() - _lastValidPresenceTime;
        unsigned long timeoutLimitMs = _timeoutSec * 1000UL;

        if (elapsedMs > timeoutLimitMs) {
            if (!_timeoutExpiredCommandSent) { 
                if (activeAction == "on_off" || activeAction == "off_only") {
                    Serial.println("\n🌙 [AUTO-HUE] Nessuna attività. SPENGO DI SICUREZZA!");
                    forceLightsState(false);
                }
                _timeoutExpiredCommandSent = true; 
            }
        }
    }
}

void HueManager::forceLightsState(bool state, int brightness, String hexColor) {
    _lightIsOn = state; 
    if (state) _lastValidPresenceTime = millis(); 
    if (_lights.length() == 0) return;

    WiFiClientSecure client; client.setInsecure(); HTTPClient http; http.setReuse(true); 

    String payload = "";
    if (state) {
        payload = "{\"on\":true";
        if (brightness >= 0) {
            int hueBri = (brightness * 254) / 100;
            payload += ", \"bri\":" + String(hueBri);
        }
        
        // 👇 --- GRAFICA E LOG COSTRUZIONE PAYLOAD COLORE --- 👇
        if (hexColor.length() == 7 && hexColor.startsWith("#")) {
            String xyString = hexToXY(hexColor);
            payload += ", " + xyString;
            Serial.printf("🎨 [HUE-MATH] Colore HEX %s convertito con successo in %s\n", hexColor.c_str(), xyString.c_str());
        } else {
            Serial.printf("⚠️ [HUE-MATH] Attenzione: Stringa colore non valida o vuota: '%s'\n", hexColor.c_str());
        }
        // ☝️ -------------------------------------------------- ☝️
        
        payload += "}";
    } else {
        payload = "{\"on\":false}";
    }

    // 🚀 STAMPA DEL STRINGONE INVIATO A PHILIPS HUE
    Serial.printf("🚀 [HUE-HTTP-PUT] Invio questa stringa JSON al Bridge: %s\n", payload.c_str());

    int start = 0; int end = _lights.indexOf(',');
    while (true) {
        String id = (end != -1) ? _lights.substring(start, end) : _lights.substring(start);
        if (id.length() > 0) {
            String url = "https://" + _bridgeIp + "/api/" + _token + "/lights/" + id + "/state";
            http.begin(client, url);
            int httpCode = http.PUT(payload);
            if (httpCode != HTTP_CODE_OK && httpCode > 0) {
                Serial.printf("❌ [HUE-HTTP-ERRORE] Risposta del Bridge: %d per la luce %s\n", httpCode, id.c_str());
            }
            http.end();
        }
        if (end == -1) break; 
        start = end + 1; end = _lights.indexOf(',', start);
    }
}

// Convertitore Magico: Da HEX di React a CIE XY di Philips Hue
String HueManager::hexToXY(String hexColor) {
    long number = strtol(&hexColor[1], NULL, 16);
    float r = (number >> 16) / 255.0f;
    float g = (number >> 8 & 0xFF) / 255.0f;
    float b = (number & 0xFF) / 255.0f;

    // Gamma correction nativa Philips Hue
    r = (r > 0.04045f) ? pow((r + 0.055f) / (1.0f + 0.055f), 2.4f) : (r / 12.92f);
    g = (g > 0.04045f) ? pow((g + 0.055f) / (1.0f + 0.055f), 2.4f) : (g / 12.92f);
    b = (b > 0.04045f) ? pow((b + 0.055f) / (1.0f + 0.055f), 2.4f) : (b / 12.92f);

    float X = r * 0.664511f + g * 0.154324f + b * 0.162028f;
    float Y = r * 0.283881f + g * 0.668433f + b * 0.047685f;
    float Z = r * 0.000088f + g * 0.072310f + b * 0.986039f;

    float x = 0.3227f; 
    float y = 0.3290f;
    if ((X + Y + Z) > 0) {
        x = X / (X + Y + Z);
        y = Y / (X + Y + Z);
    }
    
    return "\"xy\":[" + String(x, 4) + "," + String(y, 4) + "]";
}

void HueManager::adjustBrightness(int deltaPercent) {
    if (_lights.length() == 0 || !_lightIsOn) return;
    WiFiClientSecure client; client.setInsecure(); HTTPClient http; http.setReuse(true); 
    int hueDelta = (deltaPercent * 254) / 100;
    String payload = "{\"bri_inc\":" + String(hueDelta) + "}";
    int start = 0; int end = _lights.indexOf(',');
    while (true) {
        String id = (end != -1) ? _lights.substring(start, end) : _lights.substring(start);
        if (id.length() > 0) {
            String url = "https://" + _bridgeIp + "/api/" + _token + "/lights/" + id + "/state";
            http.begin(client, url); http.PUT(payload); http.end();
        }
        if (end == -1) break; 
        start = end + 1; end = _lights.indexOf(',', start);
    }
}

void HueManager::setAutomationEnabled(bool state) {
    _automationEnabled = state;
    _preferences.begin("radar_cfg", false);
    _preferences.putBool("automation", state);
    _preferences.end();
}

// 👇 --- NUOVA FUNZIONE PER INTERROGARE IL BRIDGE --- 👇
// Interroga la primissima luce associata per scoprire se è stata accesa a mano
bool HueManager::isPhysicallyOn() {
    if (_lights.length() == 0 || _bridgeIp == "") return false;
    
    // Estraiamo il primo ID lampadina dalla stringa (es. "4,7,9" -> "4")
    String firstId = _lights.substring(0, _lights.indexOf(',') == -1 ? _lights.length() : _lights.indexOf(','));
    
    WiFiClientSecure client; client.setInsecure(); HTTPClient http;
    String url = "https://" + _bridgeIp + "/api/" + _token + "/lights/" + firstId;
    
    http.begin(client, url);
    int httpCode = http.GET();
    bool isOn = false;
    
    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        // Lettura veloce tramite ricerca stringa, per non appesantire l'ESP32 col parsing JSON!
        if (payload.indexOf("\"on\":true") > 0 || payload.indexOf("\"on\": true") > 0) {
            isOn = true;
        }
    }
    http.end();
    return isOn;
}