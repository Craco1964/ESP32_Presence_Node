#include "WebManager.h"
#include "HueManager.h"
#include <time.h> // <-- NUOVO: Aggiunta per la gestione del tempo

WebManager::WebManager(SensorsManager* sensors, HueManager* hue) : _sensors(sensors), _hue(hue) {
    _server = new AsyncWebServer(80);
}

void WebManager::begin() {
    if(!LittleFS.begin(true)){ Serial.println("Errore LittleFS"); }

    // Hue Config Lettura
    _preferences.begin("hue_cfg", true);
    _hueBridgeIp = _preferences.getString("ip", "");
    _hueToken = _preferences.getString("token", "");
    _hueSelectedLights = _preferences.getString("lights", "");
    _preferences.end();

    if (_hueBridgeIp != "" && _hueToken != "") {
        _isHueConfigured = true;
        Serial.printf("🏠 Trovata configurazione Hue: %s\n", _hueBridgeIp.c_str());
    } else {
        _isHueConfigured = false;
        Serial.println("ℹ️ Nessuna configurazione Hue. Modalità Setup.");
    }

    setupWiFi();
    
    if (MDNS.begin("smartcontroller")) { 
        Serial.println("mDNS: http://smartcontroller.local"); 
    }

    setupRoutes();
    _server->begin();
    Serial.println("Server Web avviato!");
}

void WebManager::update() {
    if (_shouldReboot) {
        Serial.println("Riavvio in corso per applicare credenziali...");
        delay(1000);
        ESP.restart();
    }
}

bool WebManager::isAPMode() const {
    return WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA;
}

String WebManager::getSSID() const {
    return isAPMode() ? WiFi.softAPSSID() : WiFi.SSID();
}

String WebManager::getIP() const {
    return isAPMode() ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
}

void WebManager::setupWiFi() {
    _preferences.begin("wifi_cfg", true); 
    String saved_ssid = _preferences.getString("ssid", "");
    String saved_pass = _preferences.getString("password", "");
    _preferences.end();

    if (saved_ssid == "") {
        Serial.println("Nessuna rete salvata. Avvio Access Point...");
        WiFi.mode(WIFI_AP);
        WiFi.softAP("Setup_Sensore", "12345678");
    } else {
        WiFi.mode(WIFI_STA);
        WiFi.begin(saved_ssid.c_str(), saved_pass.c_str());
        
        int retries = 0;
        while (WiFi.status() != WL_CONNECTED && retries < 20) {
            delay(500); Serial.print("."); retries++;
        }

        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("\nWi-Fi non trovato. Fallback AP...");
            WiFi.mode(WIFI_AP);
            WiFi.softAP("Setup_Sensore", "12345678"); 
        } else {
            // --- NUOVO: SINCRONIZZAZIONE SERVER NTP DOPO LA CONNESSIONE WIFI ---
            Serial.println("\nWi-Fi Connesso! Configuro l'orologio NTP...");
            // Evita che il wi-fi vada in sleep mode per non perdere la sincronizzazione del tempo  
            WiFi.setSleep(false); // <-- TODO V2.0: Toggle da App React
            configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.nist.gov");
        }
    }
}

void WebManager::setupRoutes() {
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Origin, X-Requested-With, Content-Type, Accept");

    _server->onNotFound([](AsyncWebServerRequest *request) {
        if (request->method() == HTTP_OPTIONS) request->send(200);
        else request->send(404, "text/plain", "Not found");
    });

    _server->serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    // 1. GET Parametri Radar e Automazione (AGGIORNATO PER LE REGOLE)
    _server->on("/api/settings/radar", HTTP_GET, [this](AsyncWebServerRequest *request){
        _preferences.begin("radar_cfg", true);
        int sens = _preferences.getInt("sensitivity", 250);
        int timeo = _preferences.getInt("timeout", 60);
        bool autoEn = _preferences.getBool("automation", true);
        String rulesStr = _preferences.getString("rules", "[]"); // Legge l'array JSON come stringa
        _preferences.end();
        
        String response = "{\"sensitivity\":" + String(sens) + ",\"timeout\":" + String(timeo) + ",\"automationEnabled\":" + (autoEn?"true":"false") + ",\"rules\":" + rulesStr + "}";
        request->send(200, "application/json", response);
    });

    // 2. POST Parametri (AGGIORNATO PER LE REGOLE)
    AsyncCallbackJsonWebHandler* radarSettingsHandler = new AsyncCallbackJsonWebHandler("/api/settings/radar", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        JsonObject jsonObj = json.as<JsonObject>();
        
        _preferences.begin("radar_cfg", false);
        _preferences.putInt("sensitivity", jsonObj["sensitivity"] | 250);
        _preferences.putInt("timeout", jsonObj["timeout"] | 60);
        _preferences.putBool("automation", jsonObj["automationEnabled"] | true);
        
        if (!jsonObj["rules"].isNull()) {
            String rulesStr;
            serializeJson(jsonObj["rules"], rulesStr); // Converte l'array JSON in stringa testuale
            _preferences.putString("rules", rulesStr); // Lo salva in NVRAM in modo sicuro
        }
        _preferences.end();

        if (_hue) _hue->reloadConfig(); 
        
        request->send(200, "application/json", "{\"status\":\"saved\"}");
    });
    _server->addHandler(radarSettingsHandler);

    // 3. API MANUAL
    AsyncCallbackJsonWebHandler* manualHueHandler = new AsyncCallbackJsonWebHandler("/api/hue/manual", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        JsonObject jsonObj = json.as<JsonObject>();
        bool state = jsonObj["state"] | false;
        
        if (_hue) {
            _hue->forceLightsState(state); // Questa chiamata ora ignora la luminosità (accende all'ultima nota)
            
            _preferences.begin("radar_cfg", false);
            _preferences.putBool("automation", state); 
            _preferences.end();
            
            _hue->reloadConfig(); 
            Serial.printf("🔒 [MANUAL] Forzato %s: Automazione Radar riscritta a %s\n", state ? "ON" : "OFF", state ? "ATTIVA" : "DISATTIVATA");
        }
        
        request->send(200, "application/json", "{\"status\":\"ok\"}");
    });
    _server->addHandler(manualHueHandler);

    // 4. Endpoint Calibrazione
    _server->on("/api/sensors/calibrate", HTTP_POST, [this](AsyncWebServerRequest *request){
        _sensors->requestCalibration();
        request->send(200, "application/json", "{\"status\":\"started\"}");
    });

    // 5. Endpoint Reset
    _server->on("/api/sensors/reset", HTTP_POST, [this](AsyncWebServerRequest *request){
        _sensors->factoryReset();
        request->send(200, "application/json", "{\"status\":\"reset\"}");
    });

    _server->on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest *request){
        int n = WiFi.scanNetworks();
        JsonDocument doc;
        JsonArray networks = doc["networks"].to<JsonArray>();
        for (int i = 0; i < n; ++i) { networks.add(WiFi.SSID(i)); }
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });

    AsyncCallbackJsonWebHandler* connectHandler = new AsyncCallbackJsonWebHandler("/api/wifi/connect", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        String new_ssid = json["ssid"].as<String>();
        String new_pass = json["password"].as<String>();
        
        _preferences.begin("wifi_cfg", false); 
        _preferences.putString("ssid", new_ssid);
        _preferences.putString("password", new_pass);
        _preferences.end();
        
        request->send(200, "application/json", "{\"status\":\"ok\"}");
        _shouldReboot = true; 
    });
    _server->addHandler(connectHandler);

    _server->on("/api/wifi/status", HTTP_GET, [this](AsyncWebServerRequest *request){
        JsonDocument doc;
        if (WiFi.status() == WL_CONNECTED) {
            doc["status"] = "connected"; doc["ip"] = getIP(); doc["ssid"] = getSSID();
        } else {
            doc["status"] = "ap"; doc["ip"] = getIP(); doc["ssid"] = getSSID();
        }
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });

    _server->on("/api/sensors", HTTP_GET, [this](AsyncWebServerRequest *request){
        JsonDocument doc;
        
        doc["lastRadarUpdate"]   = _sensors->getLastRadarUpdateTime();
        doc["lastRadarPresence"] = _sensors->getRadarPresence();
        doc["lastRadarDistance"] = _sensors->getRadarDistance();
        
        int status = _sensors->getRadarStatus();
        doc["radarStatus"] = status; 
        doc["lastRadarDistance"] = (status == 0) ? 0 : _sensors->getRadarDistance();

        if (_hue) {
            doc["hueLightOn"] = _hue->isLightOn();
        } else {
            doc["hueLightOn"] = false;
        }

        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });

    _server->on("/api/hue/proxy", HTTP_OPTIONS, [](AsyncWebServerRequest *request){ request->send(200); });
    
    _server->on("/api/hue-discovery", HTTP_GET, [](AsyncWebServerRequest *request) {
        WiFiClientSecure client; client.setInsecure();
        HTTPClient http;
        http.begin(client, "https://discovery.meethue.com/");
        int httpCode = http.GET();
        if (httpCode > 0) request->send(200, "application/json", http.getString());
        else request->send(500, "application/json", "[]"); 
        http.end();
    });

    _server->on("/api/hue/status", HTTP_GET, [this](AsyncWebServerRequest *request){
        JsonDocument doc;
        doc["configured"] = _isHueConfigured; doc["ip"] = _hueBridgeIp;
        doc["token"] = _hueToken; doc["lights"] = _hueSelectedLights;
        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });

    AsyncCallbackJsonWebHandler* saveHueHandler = new AsyncCallbackJsonWebHandler("/api/hue/save", [this](AsyncWebServerRequest *request, JsonVariant &json) {
        JsonObject jsonObj = json.as<JsonObject>();
        
        _hueBridgeIp = jsonObj["bridgeIp"].as<String>();
        _hueToken = jsonObj["hueUsername"].as<String>();
        
        JsonArray lightsArray = jsonObj["selectedLights"].as<JsonArray>();
        _hueSelectedLights = "";
        for(size_t i = 0; i < lightsArray.size(); i++) {
            if(i > 0) _hueSelectedLights += ",";
            _hueSelectedLights += lightsArray[i].as<String>();
        }

        _preferences.begin("hue_cfg", false);
        _preferences.putString("ip", _hueBridgeIp);
        _preferences.putString("token", _hueToken);
        _preferences.putString("lights", _hueSelectedLights);
        _preferences.end();

        _isHueConfigured = true;

        if (_hue) _hue->reloadConfig(); 

        request->send(200, "application/json", "{\"status\":\"saved\"}");
    });
    _server->addHandler(saveHueHandler);

    AsyncCallbackJsonWebHandler* writeCacheHandler = new AsyncCallbackJsonWebHandler("/api/hue/write-cache", [](AsyncWebServerRequest *request, JsonVariant &json) {
        File cacheFile = LittleFS.open("/hue_cache.json", "w");
        if (cacheFile) {
            serializeJson(json, cacheFile);
            cacheFile.close();
            request->send(200, "application/json", "{\"status\":\"ok\"}");
        } else {
            request->send(500, "application/json", "{\"error\":\"fs_error\"}");
        }
    });
    writeCacheHandler->setMaxContentLength(30000); 
    _server->addHandler(writeCacheHandler);

    AsyncCallbackJsonWebHandler* hueProxy = new AsyncCallbackJsonWebHandler("/api/hue/proxy", [](AsyncWebServerRequest *request, JsonVariant &json) {
        JsonObject jsonObj = json.as<JsonObject>();
        String ip = jsonObj["ip"].as<String>();
        String endpoint = jsonObj["endpoint"].as<String>();
        String method = jsonObj["method"].as<String>();
        String payload = jsonObj["payload"].as<String>();

        WiFiClientSecure client; client.setInsecure(); 
        HTTPClient http;
        http.begin(client, "https://" + ip + endpoint);
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        http.addHeader("Content-Type", "application/json");
        http.addHeader("Accept", "application/json");

        int httpCode = (method == "POST") ? http.POST(payload) : http.GET();
        if (httpCode > 0) request->send(200, "application/json", http.getString());
        else request->send(500, "application/json", "{\"error\":\"Connessione al Bridge fallita\"}");
        http.end();
    });
    _server->addHandler(hueProxy);
}