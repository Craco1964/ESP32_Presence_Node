#ifndef SENSORSMANAGER_H
#define SENSORSMANAGER_H

#include <Arduino.h>
#include "Config.h"
#include <LD2420GeoGab.h>

class SensorsManager {
public:
    SensorsManager();
    bool begin();
    
    // --- IL PEZZO MANCANTE ---
    void update(); 

    bool getRadarPresence() const { return currentRadarPresence; }
    int getRadarDistance() const { return currentRadarDistance; }
    
    // --- NUOVO: Restituisce 0 (Vuoto), 1 (Movimento), 2 (Presenza Statica) ---
    int getRadarStatus() const { return currentRadarStatus; } 

    unsigned long getLastRadarUpdateTime() const { return lastRadarUpdate; }
    bool radarUpdated();
    void clearRadarUpdated();

    void requestCalibration();
    void factoryReset();
    void applyCustomThresholds();

private:
   
    bool firstRun = true;

    LD2420GeoGab radar;
    bool currentRadarPresence = false;
    int currentRadarDistance = 0;
    int currentRadarStatus = 0; 

    bool lastRadarPresence = false;
    unsigned long lastRadarUpdate = 0;
    bool _radarUpdatedFlag = false;

    bool _calibrationRequested = false;
    
};

#endif