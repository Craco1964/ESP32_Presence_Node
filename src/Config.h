#pragma once
#include <Arduino.h>

// --- Pinout Display ---
#define PIN_LCD_SCK  1
#define PIN_LCD_MOSI 2
#define PIN_LCD_CS   14
#define PIN_LCD_DC   15
#define PIN_LCD_RST  22
#define PIN_LCD_BL   23
#define PIN_TP_RST   20
#define PIN_TP_INT   21

// --- Pinout I2C (IMU) ---
#define PIN_I2C_SDA  18
#define PIN_I2C_SCL  19
#define IMU_ADDRESS 0x6B

// --- Pinout Radar LD2420 (I PIN CORRETTI!) ---
#define PIN_RADAR_RX  5  // Collegato al  TX del Radar (OT2) - filo giallo radar
#define PIN_RADAR_TX  4  // Collegato all'RX del Radar (OT1) - filo nero radar
#define PIN_RADAR_OUT 6  // Pin di presenza  del Radar (RX)  - filo nero

