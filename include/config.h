#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include "time.h"
#include <PZEM004Tv30.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// ==========================================
// HARDWARE PIN DEFINITIONS
// ==========================================
#define PIN_RELAY_GRID_L  14 
#define PIN_RELAY_GRID_N  32 

// Relay Solar (PV)
#define PIN_RELAY_SOLAR_L 19 
#define PIN_RELAY_SOLAR_N 33

// Triac Solid State
#define PIN_TRIAC_GRID    12
#define PIN_TRIAC_SOLAR   18

#define PZEM_RX_PIN     17 
#define PZEM_TX_PIN     16
// Button Pin
#define PIN_BTN_MODE    26  // Auto/manual Mode
#define PIN_BTN_PAGE    27  // Change screen pages
#define PIN_BTN_TOGGLE  25  // Execution button (Enter)

// ==========================================
// OLED DISPLAY & NTP SETTINGS
// ==========================================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

#define NTP_SERVER "pool.ntp.org"
#define GMT_OFFSET_SEC (7 * 3600) 
#define DAYLIGHT_OFFSET_SEC 0

// ==========================================
// ENUMS & DATA STRUCTURES
// ==========================================
enum AtsState { STATE_GRID_ACTIVE, STATE_TRANSITIONING, STATE_SOLAR_ACTIVE, STATE_ERROR_NO_POWER };
enum TimePeriod { PROD_TIME, TRANS_TIME, NON_PROD_TIME };

struct SensorData {
    float voltage;
    float frequency;
    float current;
    float power;
    float anomalyDurationSec;
};

// ==========================================
// GLOBAL EXTERN VARIABLES
// ==========================================
extern SemaphoreHandle_t sensorMutex;
extern SensorData currentSensorData;
extern AtsState currentState;
extern TimePeriod activePeriod;
extern PZEM004Tv30 pzem;
extern Adafruit_SSD1306 display;

// Additional variable for WifiManager, UI, and manual mode
extern WiFiManager wm;
extern String connectedWiFi;
extern String lastSyncTime; 
extern bool isManualMode;
extern bool manualForceGrid; 
extern bool emergencyLock;
extern bool isStreaming;