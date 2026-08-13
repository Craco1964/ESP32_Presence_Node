#include "SensorsManager.h"

volatile bool g_isPresent = false;
volatile uint16_t g_distance = 0;
volatile uint8_t g_status = 0; 
volatile bool g_hasNewData = false;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static const char *statusLabel(LD2420DetectionStatus s) {
    switch (s) {
        case LD2420DetectionStatus::Motion:   return "MOTION";
        case LD2420DetectionStatus::Presence: return "PRESENCE";
        default:                              return "NONE";
    }
}

/* 
Ciascuno dei 16 gate copre 70 cm . Il gate 0 (0–70 cm) in genere produce falsi trigger dal PCB del sensore stesso: escluderlo con minGate = 1.

Gate  0 =    0 –   70 cm  ← near-field, exclude with minGate=1
Gate  1 =   70 –  140 cm
Gate  5 =  350 –  420 cm
Gate  8 =  560 –  630 cm
Gate 11 =  770 –  840 cm  ← practical wall-mount motion limit (~8 m)
Gate 15 = 1050 – 1120 cm  ← theoretical max
*/

/* Wiring:
 *   ESP32 / ESP32-S3    LD2420
 *   GG_TXPIN        ──  RX
 *   GG_RXPIN        ──  TX
 *   3.3 V           ──  VCC
 *   GND             ──  GND
 */

/**
 * @brief Print a single-line ASCII bar for one gate energy value.
 * @details Scale: each '█' represents 500 energy units, max 20 bars.
 */
static void printEnergyBar(uint8_t gate, uint16_t energy) {
    uint8_t bars = (uint8_t)min((uint32_t)(energy / 500), (uint32_t)20);
    Serial.printf("  G%02u [%5u] |", gate, energy);
    for (uint8_t b = 0; b < bars;  b++) Serial.print("█");
    for (uint8_t b = bars; b < 20; b++) Serial.print("░");
    Serial.println("|");
}


SensorsManager::SensorsManager() {}

bool SensorsManager::begin() {
    bool success = true;

    if (!radar.begin()) {
        Serial.println("❌ Sensore Radar non trovato (Controlla i cavi TX/RX)!");
        success = false;
    } else {
        radar.activateConfigMode();
        radar.setSystemMode(LD2420SystemMode::Energy);
        radar.setGateRange(0, 8, 5);   // ogni gate è circa 75 cm, 5s hold-off
        radar.deactivateConfigMode();
        //applyCustomThresholds();
        
        // Elabora la seriale ogni 50ms per evitare l'overflow dei buffer
        radar.setUpdateInterval(50);

        radar.setPresenceCallback([](bool present) {
            g_isPresent = present;
            g_hasNewData = true;
            //Serial.printf("\n[PRESENCE] %s\n\n", present ? "DETECTED" : "CLEAR");
        });

        radar.setDistanceCallback([](uint16_t dist) {
            g_distance = dist;
            g_hasNewData = true;
        });

        radar.setStatusCallback([](LD2420DetectionStatus status) {
            if (status == LD2420DetectionStatus::Motion) g_status = 1;
            else if (status == LD2420DetectionStatus::Presence) g_status = 2;
            else g_status = 0;
            g_hasNewData = true;
            //Serial.printf("[STATUS]   %s\n", statusLabel(status));
        });

        radar.setCalibrationCompleteCallback([](bool success, const LD2420ABDConfig &cfg) {
            if (success) {
                Serial.println("✅ [CALIBRAZIONE] Nuove soglie acquisite con successo!");
            } else {
                Serial.println("❌ [CALIBRAZIONE] Fallita o annullata.");
            }
        });

    /*
    // Fires every frame with the full gate energy array.
    // Prints a bar graph — reduce GG_DEBUG or comment this out in production.
    radar.setEnergyCallback([](const LD2420EnergyFrame &frame) {
        Serial.printf("[ENERGY]   status=%-8s  dist=%u cm\n",
                      statusLabel(frame.status), frame.distance);
        for (uint8_t g = 0; g < LD2420_TOTAL_GATES; g++)
            printEnergyBar(g, frame.gateEnergy[g]);
        Serial.println();
    });
    */
    
        Serial.println("📡 Radar LD2420 Inizializzato (Modalità ENERGY via UART)");
    }

    delay(500);

    
    return success;
}

void SensorsManager::update() {

    // --- LOGICA AUTOCALIBRAZIONE (BLOCCANTE) ---
    if (_calibrationRequested) {
        _calibrationRequested = false;
        Serial.println("\n⚠️ [CALIBRAZIONE] Inizio tra 5 secondi... USCIRE DALLA STANZA!");
        
        for (int i = 5; i > 0; i--) {
            Serial.printf("⏳ -%d...\n", i);
            delay(1000); // L'ESP32 non va in crash grazie al FreeRTOS sotto il cofano
        }
        
        Serial.println("⚙️ [CALIBRAZIONE] Acquisizione in corso (circa 10 sec)...");
        
        // Collezione 100 frames, no delay extra, modalità bloccante, ignora Gate 0 (troppo sensibile)
        radar.startAutoCalibration(100, 0, true, true); 
        
        Serial.println("✅ [CALIBRAZIONE] Processo terminato. Riprendo la normale lettura.\n");
    }

    // --- AGGIORNAMENTO RADAR ---
    radar.update(); 

    if (g_hasNewData) {
        currentRadarPresence = g_isPresent;
        currentRadarDistance = g_distance;
        
        // Se il sensore fisico non rileva nulla (0), azzeriamo senza obiezioni
        if (g_status == 0) {
            currentRadarStatus = 0;
        } 
        // Se il sensore rileva MOTION (1) ma noi lo avevamo bloccato a PRESENCE (2),
        // NON lo sovrascriviamo! Lasciamo che sia il nostro Heartbeat a decidere se sbloccarlo.
        else if (g_status == 1 && currentRadarStatus == 2) {
             // currentRadarStatus rimane 2 (Presenza Software)
        } 
        // In tutti gli altri casi, copiamo il dato grezzo
        else {
            currentRadarStatus = g_status;
        }
        
        g_hasNewData = false;

        // Segnaliamo all'ESP32 che deve ridisegnare il display
        if (currentRadarPresence != lastRadarPresence || (currentRadarPresence && millis() - lastRadarUpdate > 250)) {
            lastRadarUpdate = millis();
            _radarUpdatedFlag = true;
            lastRadarPresence = currentRadarPresence;
        }
    }

    
    // --- LOGICA SOFTWARE "PRESENZA STATICA" E HEARTBEAT ---
    const int DISTANCE_TOLERANCE = 15;     
    const int REQUIRED_STABLE_SECONDS = 5; 

    static int savedDistance = 0;
    static int stableSeconds = 0;
    static unsigned long lastHeartbeat = 0;

    if (millis() - lastHeartbeat > 1000) {
        
        // 1. Valutiamo SEMPRE se c'è un target valido (sia in Motion che in Presence)
        if (currentRadarStatus == 1 || currentRadarStatus == 2) { 
            
            // Se eravamo a 0, inizializziamo la distanza di partenza
            if (savedDistance == 0) savedDistance = currentRadarDistance;

            int delta = abs(currentRadarDistance - savedDistance);
            
            if (delta <= DISTANCE_TOLERANCE) {
                // Sei rimasto fermo entro la tolleranza
                if (stableSeconds < REQUIRED_STABLE_SECONDS) {
                    stableSeconds++; 
                }
                
                if (stableSeconds >= REQUIRED_STABLE_SECONDS) {
                    currentRadarStatus = 2; // Passa a (o mantieni) PRESENCE
                    // Aggiorniamo gradualmente l'ancora per assecondare la respirazione
                    savedDistance = currentRadarDistance; 
                }
            } else {
                // TI SEI MOSSO OLTRE LA TOLLERANZA! 
                // Rompiamo il blocco e torniamo a MOTION
                currentRadarStatus = 1;
                savedDistance = currentRadarDistance; // Nuovo punto di ancoraggio
                stableSeconds = 0; // Azzera il contatore
            }
        } 
        else if (currentRadarStatus == 0) { 
            // Stanza vuota, resetta tutto
            stableSeconds = 0;
            savedDistance = 0;
        }

        // 2. Prepariamo la stringa per il monitor seriale
        String stateStr = (currentRadarStatus == 1) ? "🏃 MOTION  " : 
                          (currentRadarStatus == 2) ? "🧘 PRESENCE" : 
                                                      "👻 NONE    ";
        
        int logDistance = (currentRadarStatus == 0) ? 0 : currentRadarDistance;
        
        Serial.printf("💓 [LIVE] Stato: %s | Distanza: %4d cm | Stabilità: %ds | Loop: OK\n", 
                      stateStr.c_str(), logDistance, stableSeconds);
                      
        lastHeartbeat = millis();
    }
}

bool SensorsManager::radarUpdated() {
    return _radarUpdatedFlag;
}

void SensorsManager::clearRadarUpdated() {
    _radarUpdatedFlag = false;
}

void SensorsManager::requestCalibration() {
    _calibrationRequested = true;
}

void SensorsManager::factoryReset() {
    Serial.println("🔄 [FACTORY RESET] Ripristino valori di fabbrica del radar...");
    
    radar.activateConfigMode();
    radar.factoryReset(); // Invia il comando hardware di reset
    radar.restart();      // Riavvia il chip del radar
    
    // Il radar si è appena spento e riacceso. Dobbiamo dargli un paio di secondi per riprendersi!
    Serial.println("⏳ Attendo il riavvio del sensore...");
    delay(2000);
    
    // Ora che è tornato di fabbrica (in Modalità Semplice), forziamolo di nuovo in Modalità ENERGY!
    radar.activateConfigMode();
    radar.setSystemMode(LD2420SystemMode::Energy);
    radar.deactivateConfigMode();
    
    Serial.println("✅ [FACTORY RESET] Sensore ripristinato e riconfigurato in ENERGY mode!");
}

void SensorsManager::applyCustomThresholds() {
    Serial.println("⚙️ [RADAR] Applicazione soglie manuali chirurgiche (Gate 2-8)...");

    radar.activateConfigMode();

    // Impostiamo il range visivo (ignora Gate 0 e 1, guarda fino al Gate 8, hold-off 5s)
    radar.setGateRange(2, 8, 5); 

    // Ciclo per sovrascrivere le soglie dei Gate da 2 a 8
    for (uint8_t gate = 2; gate <= 8; gate++) {
        // Imposta la soglia di movimento a 150
        radar.setGateABDHighThreshold(gate, 150);
        
        // Imposta la soglia di presenza (respiro) a 25
        radar.setGateABDLowThreshold(gate, 25);
    }

    radar.deactivateConfigMode();
    
    Serial.println("✅ [RADAR] Soglie personalizzate applicate con successo! Sensore ottimizzato.");
}