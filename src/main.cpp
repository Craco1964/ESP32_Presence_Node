#include <Arduino.h>
#include "Config.h"
#include "SensorsManager.h"
#include "WebManager.h"
#include "HueManager.h"

SensorsManager sensors;
HueManager hue(&sensors);
WebManager web(&sensors, &hue); 

void setup() {
    // 1. Diamo tempo all'alimentazione di stabilizzarsi
    delay(500); 

    Serial.begin(115200);
    Serial.println("\n--- BOOT ESP32 SATELLITE NODE ---");

    Serial.println("[1/3] Avvio Sensori...");
    
    // Proviamo ad avviare i sensori fino a 3 volte
    int sensorRetries = 0;
    bool sensorsReady = false;
    while (sensorRetries < 3) {
        if (sensors.begin()) {
            sensorsReady = true;
            break;
        }
        sensorRetries++;
        Serial.printf("⚠️ Tentativo avvio sensori %d fallito, ci riprovo...\n", sensorRetries);
        delay(500);
    }

    if (!sensorsReady) {
        Serial.println("❌ Errore critico: i sensori non rispondono! Continuo il boot per il WebServer...");
    }

    Serial.println("[2/3] Avvio WebManager...");
    web.begin(); 

    Serial.println("[3/3] Avvio HueManager...");
    hue.begin(); 

    Serial.println("--- SETUP COMPLETATO ---");
    Serial.print("🌐 Indirizzo IP: ");
    Serial.println(web.getIP());
}

void loop() {
    // Il cuore del sistema: WebServer, Sensori, Hue. Stop.
    web.update();
    sensors.update(); // <- Rimosso il 'true' dell'IMU come fatto nel SensorsManager
    hue.update(); 

    // Manteniamo pulito il flag del radar per il WebServer
    if (sensors.radarUpdated()) {
        sensors.clearRadarUpdated();
    }

    // Un piccolo delay per far respirare il Wi-Fi
    delay(60); 
}