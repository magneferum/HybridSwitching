#include <Arduino.h>
#include "config.h"
#include "fuzzy_logic.h"

// ==========================================
// PHYSICAL MEMORY ALLOCATION
// ==========================================
SemaphoreHandle_t sensorMutex;
SensorData currentSensorData = {0.0, 0.0, 0.0, 0.0, 0.0};
AtsState currentState = STATE_GRID_ACTIVE;
TimePeriod activePeriod = PROD_TIME;

// Global Object
PZEM004Tv30 pzem(Serial2, PZEM_RX_PIN, PZEM_TX_PIN); 

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
WiFiManager wm; 

String connectedWiFi = "Disconnected";
String lastSyncTime = " Not Synced "; 
bool isManualMode = false;
bool manualForceGrid = true; 
bool emergencyLock = false; 
bool isStreaming = false; 

// Google Apps Script URL
String googleScriptURL = "https://script.google.com/macros/s/AKfycbwzDVv_IC8FuaDIn_l7hm2o14spNaH9AqbqyZQSZNVjHLfW4nff_vNVWWLH1McN-uJMEg/exec";

void taskSwitchingControl(void *pvParameters);
void taskReadTelemetry(void *pvParameters);
void taskUpdateUI(void *pvParameters);
void taskDataStream(void *pvParameters); 

void setupTime() {
    vTaskDelay(pdMS_TO_TICKS(2000)); 
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    struct tm timeinfo;
    int retry = 0;
    while (!getLocalTime(&timeinfo, 1000) && retry < 10) retry++;

    if (retry < 10) {
        char timeStringBuff[50];
        strftime(timeStringBuff, sizeof(timeStringBuff), "%y/%m/%d %H:%M", &timeinfo);
        lastSyncTime = String(timeStringBuff);
    } else {
        lastSyncTime = "Sync Failed";
    }
}

void setup() {
    Serial.begin(115200);

    pinMode(PIN_RELAY_GRID_L, OUTPUT); pinMode(PIN_RELAY_GRID_N, OUTPUT);
    pinMode(PIN_RELAY_SOLAR_L, OUTPUT); pinMode(PIN_RELAY_SOLAR_N, OUTPUT);
    pinMode(PIN_TRIAC_GRID, OUTPUT); pinMode(PIN_TRIAC_SOLAR, OUTPUT);

    digitalWrite(PIN_RELAY_GRID_L, HIGH); digitalWrite(PIN_RELAY_GRID_N, HIGH);
    digitalWrite(PIN_RELAY_SOLAR_L, LOW); digitalWrite(PIN_RELAY_SOLAR_N, LOW);
    digitalWrite(PIN_TRIAC_GRID, LOW); digitalWrite(PIN_TRIAC_SOLAR, LOW);

    pinMode(PIN_BTN_MODE, INPUT_PULLUP);
    pinMode(PIN_BTN_PAGE, INPUT_PULLUP);
    pinMode(PIN_BTN_TOGGLE, INPUT_PULLUP);

    // --- Oled Clock ---
    Wire.begin();
    Wire.setClock(400000); 
    // ---- Startup Interface -----
    display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    display.clearDisplay();
    display.setTextColor(WHITE);
    display.setTextSize(1);
    display.setCursor(10, 25); display.println("ATS MICROGRID");
    display.setCursor(10, 40); display.println("System Booting...");
    display.display();
    
    delay(3000); 

    wm.setConfigPortalTimeout(30); 
    if (wm.autoConnect("ATS_Microgrid_AP")) {
        connectedWiFi = WiFi.SSID();
        setupTime(); 
    } else {
        connectedWiFi = "Offline";
    }

    sensorMutex = xSemaphoreCreateMutex();
    if (sensorMutex != NULL) {
        // FreeRTOS Parameter
        xTaskCreatePinnedToCore(taskReadTelemetry, "Telemetry", 4096, NULL, 4, NULL, 1); 
        xTaskCreatePinnedToCore(taskSwitchingControl, "ATS_Control", 4096, NULL, 3, NULL, 1);
        xTaskCreatePinnedToCore(taskDataStream, "DataStream", 8192, NULL, 2, NULL, 0); 
        xTaskCreatePinnedToCore(taskUpdateUI, "UI", 6144, NULL, 1, NULL, 1);     
    }
}

void loop() { vTaskDelete(NULL); }

// ==========================================
// TASK 1: HYBRID ATS & FUZZY LOGIC CONTROL
// ==========================================
void taskSwitchingControl(void *pvParameters) {

    vTaskDelay(pdMS_TO_TICKS(6000)); 

    SensorData localData;
    struct tm timeinfo;
    int currentHour = 12; 
    static bool checkingSecondSource = false;

    for(;;) {
        if (xSemaphoreTake(sensorMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            localData = currentSensorData;
            xSemaphoreGive(sensorMutex);
        }

        if (getLocalTime(&timeinfo, 0)) currentHour = timeinfo.tm_hour;
        // =======================================================
        // Emergency Condition (Voltage < 5V )
        // =======================================================
        if (localData.voltage < 5.0) {
            if (!checkingSecondSource) {
                
                // 1. Safe Current Active Source
                bool wasGridActive = (currentState == STATE_GRID_ACTIVE) || manualForceGrid;
                
                // 2. Changing system status
                checkingSecondSource = true;
                emergencyLock = true; 
                currentState = STATE_TRANSITIONING;

                // 3. Emergency mode execution
                if (wasGridActive) {
                    digitalWrite(PIN_RELAY_GRID_L, LOW); digitalWrite(PIN_RELAY_GRID_N, LOW);
                    vTaskDelay(pdMS_TO_TICKS(50)); 
                    digitalWrite(PIN_RELAY_SOLAR_L, HIGH); digitalWrite(PIN_RELAY_SOLAR_N, HIGH);
                    
                    currentState = STATE_SOLAR_ACTIVE;
                    manualForceGrid = false; 
                } else {
                    digitalWrite(PIN_RELAY_SOLAR_L, LOW); digitalWrite(PIN_RELAY_SOLAR_N, LOW);
                    vTaskDelay(pdMS_TO_TICKS(50)); 
                    digitalWrite(PIN_RELAY_GRID_L, HIGH); digitalWrite(PIN_RELAY_GRID_N, HIGH);
                    
                    currentState = STATE_GRID_ACTIVE;
                    manualForceGrid = true; 
                }
            }
        } 
        // =======================================================
        // Normal Condition (Voltage > 5V )
        // =======================================================
        else {
            checkingSecondSource = false; 
            
            if (currentHour >= 7 && currentHour < 16) activePeriod = PROD_TIME;
            else if ((currentHour >= 16 && currentHour < 18) || (currentHour >= 5 && currentHour < 7)) activePeriod = TRANS_TIME;
            else activePeriod = NON_PROD_TIME;

            g_fisInput[0] = constrain(localData.voltage, 100.0, 250.0);
            g_fisInput[1] = constrain(localData.frequency, 45.0, 55.0);
            g_fisInput[2] = constrain(localData.anomalyDurationSec, 0.0, 120.0);

            if (activePeriod == PROD_TIME) fis_evaluate_Production();
            else if (activePeriod == TRANS_TIME) fis_evaluate_Transition();
            else fis_evaluate_NonProduction();

            bool targetIsGrid = true;
            if (emergencyLock) targetIsGrid = (currentState == STATE_GRID_ACTIVE);
            else if (isManualMode) targetIsGrid = manualForceGrid; 
            else targetIsGrid = (g_fisOutput[0] >= 0.5); 

            if (!targetIsGrid && currentState != STATE_SOLAR_ACTIVE) {
                currentState = STATE_TRANSITIONING;
                digitalWrite(PIN_RELAY_GRID_L, LOW); digitalWrite(PIN_RELAY_GRID_N, LOW);
                vTaskDelay(pdMS_TO_TICKS(50)); 
                digitalWrite(PIN_RELAY_SOLAR_L, HIGH); digitalWrite(PIN_RELAY_SOLAR_N, HIGH);
                currentState = STATE_SOLAR_ACTIVE;
            } else if (targetIsGrid && currentState != STATE_GRID_ACTIVE) {
                currentState = STATE_TRANSITIONING;
                digitalWrite(PIN_RELAY_SOLAR_L, LOW); digitalWrite(PIN_RELAY_SOLAR_N, LOW);
                vTaskDelay(pdMS_TO_TICKS(50)); 
                digitalWrite(PIN_RELAY_GRID_L, HIGH); digitalWrite(PIN_RELAY_GRID_N, HIGH);
                currentState = STATE_GRID_ACTIVE;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50)); 
    }
}

// ==========================================
// TASK 2: SENSOR TELEMETRY (PZEM-004T)
// ==========================================
void taskReadTelemetry(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(2000)); 
    

    // Force Serial2 for using CPU 0
    Serial2.end();
    Serial2.begin(9600, SERIAL_8N1, PZEM_RX_PIN, PZEM_TX_PIN);
    // -------------------------------------------------
    unsigned long startAnomaly = 0;
    
    for(;;) {
        // PZEM Library for read paremters
        float t_vol = pzem.voltage();
        float t_fre = pzem.frequency();
        float t_cur = pzem.current();
        float t_pow = pzem.power();
        float t_dur = 0.0;

        // --- PZEM debugging ---
        /*
        Serial.print("[PZEM TRACKER] ");
        if (isnan(t_vol)) {
            Serial.println("STATUS: GAGAL (NaN) -> Data terputus di jalan / AC 220V Mati!");
        } else {
            Serial.printf("SUKSES! V: %.1f | Hz: %.2f | A: %.2f | W: %.0f\n", t_vol, t_fre, t_cur, t_pow);
        }
        */
        // -------------------------------------------------------------------------

        // Change NaN to 0
        if (isnan(t_vol)) t_vol = 0.0; 
        if (isnan(t_fre)) t_fre = 0.0;
        if (isnan(t_cur)) t_cur = 0.0;
        if (isnan(t_pow)) t_pow = 0.0;

        // Start timer for anomaly condition
        if (t_vol < 200.0 || t_vol > 240.0 || t_fre < 49.58 || t_fre > 50.42) {
            if (startAnomaly == 0) startAnomaly = millis();
            t_dur = (millis() - startAnomaly) / 1000.0; 
        } else {
            startAnomaly = 0; 
            t_dur = 0.0;
        }

        // Insert to global memory for Fuzzy and display
        if (xSemaphoreTake(sensorMutex, portMAX_DELAY) == pdTRUE) {
            currentSensorData.voltage = t_vol;
            currentSensorData.frequency = t_fre;
            currentSensorData.current = t_cur;
            currentSensorData.power = t_pow;
            currentSensorData.anomalyDurationSec = t_dur;
            xSemaphoreGive(sensorMutex);
        }
        // off task after 1 second
        vTaskDelay(pdMS_TO_TICKS(1000)); 
    }
}
// ==========================================
// TASK 3: USER INTERFACE & MANUAL BUTTONS
// ==========================================
void taskUpdateUI(void *pvParameters) {
    int currentPage = 0; 
    int menuCursor = 0; 
    bool showConfirm = false;
    bool confirmYes = false;
    bool lastPage = HIGH, lastMode = HIGH, lastEnter = HIGH;

    for(;;) {
        bool btnPage = digitalRead(PIN_BTN_PAGE);
        bool btnMode = digitalRead(PIN_BTN_MODE);
        bool btnEnter = digitalRead(PIN_BTN_TOGGLE);

        if (btnPage == LOW && lastPage == HIGH) {
            if (!showConfirm) {
                currentPage++;
                if (currentPage > 2) currentPage = 0;
            }
        }
        lastPage = btnPage;

        if (btnMode == LOW && lastMode == HIGH) {
            if (currentPage == 2) {
                if (showConfirm) confirmYes = !confirmYes;
                else {
                    menuCursor++;
                    if (menuCursor > 3) menuCursor = 0; 
                }
            }
        }
        lastMode = btnMode;

        if (btnEnter == LOW && lastEnter == HIGH) {
            if (currentPage == 2) {
                if (showConfirm) {
                    if (confirmYes) {
                        if (menuCursor == 0 && emergencyLock) {
                            emergencyLock = false;
                            isManualMode = false; 
                        } 
                        else if (menuCursor == 1) {
                            manualForceGrid = !manualForceGrid; 
                        } 
                        else if (menuCursor == 3) { 
                            display.clearDisplay();
                            display.setCursor(0, 10); display.println("[ WiFi Portal ]");
                            display.setCursor(0, 25); display.println("Connect to AP:");
                            display.setCursor(0, 35); display.println("ATS_Microgrid_AP");
                            display.display();
                            
                            wm.startConfigPortal("ATS_Microgrid_AP");
                            
                            if (WiFi.status() == WL_CONNECTED) {
                                connectedWiFi = WiFi.SSID();
                                setupTime(); 
                            } else {
                                connectedWiFi = "Offline";
                            }
                            currentPage = 0; 
                        }
                    }
                    showConfirm = false; 
                } else {
                    if (menuCursor == 0) {
                        if (emergencyLock) {
                            showConfirm = true; confirmYes = false; 
                        } else {
                            isManualMode = !isManualMode;
                            if (isManualMode) manualForceGrid = (currentState == STATE_GRID_ACTIVE);
                        }
                    } 
                    else if (menuCursor == 1) {
                        if (isManualMode && !emergencyLock) {
                            showConfirm = true; confirmYes = false; 
                        }
                    } 
                    else if (menuCursor == 2) {
                        display.clearDisplay();
                        display.setCursor(0, 10); display.println("[ WiFi Portal ]");
                        display.setCursor(0, 25); display.println("Connect to AP:");
                        display.setCursor(0, 35); display.println("ATS_Microgrid_AP");
                        display.display();
                        
                        wm.startConfigPortal("ATS_Microgrid_AP");
                        
                        if (WiFi.status() == WL_CONNECTED) {
                            connectedWiFi = WiFi.SSID();
                            setupTime(); 
                        } else {
                            connectedWiFi = "Offline";
                        }
                        currentPage = 0; 
                    }
                    else if (menuCursor == 3) {
                        // =========================================================
                        // Stream just for Auto mode
                        // =========================================================
                        if (!emergencyLock && !isManualMode) {
                            if (WiFi.status() == WL_CONNECTED) {
                                isStreaming = !isStreaming; 
                            } else {
                                if (isStreaming) {
                                    isStreaming = false; 
                                } else {
                                    showConfirm = true;  
                                    confirmYes = false;
                                }
                            }
                        }
                    }
                }
            }
        }
        lastEnter = btnEnter;

        SensorData uiData;
        if (xSemaphoreTake(sensorMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            uiData = currentSensorData;
            xSemaphoreGive(sensorMutex);
        }
        AtsState uiState = currentState;
        float uiFuzzyScore = g_fisOutput[0]; 

        static unsigned long lastOledUpdate = 0;
        if (millis() - lastOledUpdate > 250) {
            lastOledUpdate = millis();
            
            if (WiFi.status() != WL_CONNECTED) {
                connectedWiFi = "Offline";
                isStreaming = false; 
            }
            
            // =========================================================
            // Force end stream if emergency mode active
            // =========================================================
            if (emergencyLock || isManualMode) {
                isStreaming = false; 
            }

            display.clearDisplay();
            display.invertDisplay(false);
            display.setTextSize(1);

            if (currentPage == 0) {
                display.setCursor(0, 0);
                if (emergencyLock) display.print("[EMERGENCY]");
                else if (isManualMode) display.print("[MANUAL]");
                else display.print("[AUTO]");

                display.setCursor(85, 0);
                if (uiState == STATE_GRID_ACTIVE) display.print("PLN");
                else if (uiState == STATE_SOLAR_ACTIVE) display.print("PV");
                else display.print("TRANSIT.");
                display.drawLine(0, 10, 128, 10, WHITE);

                display.setCursor(0, 14); display.printf("V : %.1f V", uiData.voltage);
                display.setCursor(70, 14); display.printf("Hz: %.2f", uiData.frequency);

                display.setCursor(0, 25); display.printf("P : %.0f W", uiData.power);
                display.setCursor(70, 25); display.printf("A : %.2f", uiData.current);
                display.drawLine(0, 35, 128, 35, WHITE);
                
                display.setCursor(0, 38); display.print("WiFi : "); display.println(connectedWiFi);
                display.setCursor(0, 47); display.print("Timer: "); display.print(uiData.anomalyDurationSec, 1); display.print(" s");

                display.setCursor(0, 56); display.print("Z: ");
                if (activePeriod == PROD_TIME) display.print("Prod"); else if (activePeriod == TRANS_TIME) display.print("Trans"); else display.print("N-Prod");

                display.setCursor(65, 56); display.print("Fz: "); display.print(uiFuzzyScore, 2);
            } 
            else if (currentPage == 1) {
                display.setCursor(0, 0); display.print("--- TIME TRACKER ---"); display.drawLine(0, 10, 128, 10, WHITE);
                struct tm timeinfo;
                if (getLocalTime(&timeinfo, 0)) {
                    display.setCursor(0, 18); display.print("Live : "); display.printf("%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
                    display.setCursor(85, 18); display.print("(WIB)"); 
                } else {
                    display.setCursor(0, 18); display.print("Live : --:--   (WIB)");
                }
                display.setCursor(0, 35); display.print("Last Synced:");
                display.setCursor(0, 48); display.print("> "); display.print(lastSyncTime);
            }
            else if (currentPage == 2) {
                display.setCursor(0, 0); display.print("--- SETTINGS MENU ---"); display.drawLine(0, 10, 128, 10, WHITE);

                if (!showConfirm) {
                    display.setCursor(0, 13);
                    display.print(menuCursor == 0 ? "> Mode: " : "  Mode: ");
                    if (emergencyLock) display.println("EMERGENCY"); else display.println(isManualMode ? "MANUAL" : "AUTO");

                    display.setCursor(0, 25);
                    display.print(menuCursor == 1 ? "> Swtch:" : "  Swtch:");
                    if (emergencyLock || !isManualMode) display.print(" (LOCKED)"); else display.print(manualForceGrid ? " To PV" : " To GRID");

                    display.setCursor(0, 37);
                    display.print(menuCursor == 2 ? "> Reset WiFi" : "  Reset WiFi");

                    display.setCursor(0, 49);
                    display.print(menuCursor == 3 ? "> Stream: " : "  Stream: ");
                    if (emergencyLock || isManualMode) {
                        display.print("(LOCKED)");
                    } else {
                        display.print(isStreaming ? "ON" : "OFF");
                    }
                } 
                else {
                    if (menuCursor == 3) {
                        display.setCursor(8, 15); display.println("WiFi Not Connected!");
                        display.setCursor(30, 27); display.println("Reset WiFi?");
                    } else {
                        display.setCursor(0, 20);
                        if (menuCursor == 0 && emergencyLock) display.println("Unlock Emergency?");
                        else if (menuCursor == 1) {
                            if (manualForceGrid) display.println("Switch to PV Source?"); else display.println("Switch to GRID Source?");
                        }
                    }
                    
                    display.setCursor(20, 45); display.print(confirmYes ? "[X] YES" : "[ ] YES");
                    display.setCursor(75, 45); display.print(!confirmYes ? "[X] NO" : "[ ] NO");
                }
            }
            display.display();
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
// ==========================================
// TASK 4: GOOGLE SHEETS DATA STREAMING
// ==========================================
void taskDataStream(void *pvParameters) {
    // Temporary memory for save confirmation data
    static bool emergencyLogSent = false;

    for(;;) {
        if (WiFi.status() == WL_CONNECTED) {
            
            // Read emergency data from taskSwitchingControl
            bool currentEmergency = emergencyLock; 

            // Clean temporary memory if Auto mode active
            if (!currentEmergency) {
                emergencyLogSent = false;
            }

            // Decide massage for data stream
            bool triggerEmergencyStream = (currentEmergency && !emergencyLogSent);
            bool triggerNormalStream = (isStreaming && !currentEmergency);

            // HTTP execution if any data for stream
            if (triggerEmergencyStream || triggerNormalStream) {
                SensorData streamData;
                AtsState streamState;
                TimePeriod streamPeriod;
                float streamFuzzy;
                
                if (xSemaphoreTake(sensorMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                    streamData = currentSensorData;
                    streamState = currentState;
                    streamPeriod = activePeriod;
                    streamFuzzy = g_fisOutput[0];
                    xSemaphoreGive(sensorMutex);
                }

                String baseUrl = googleScriptURL;
                baseUrl.trim(); 
                String url = baseUrl + "?";
                
                if (triggerEmergencyStream) {
                    // Lock Data and send emergency massage (one time)
                    url += "v=0.0";
                    url += "&hz=0.0";
                    url += "&w=0";
                    url += "&timer=0.0";
                    url += "&fuzzy=0.0";
                    url += "&state=EMERGENCY";
                    url += "&zone=EMERGENCY";
                    
                    emergencyLogSent = true; 
                    Serial.println("\n[DEBUG STREAM] ! MENGIRIM PERINGATAN EMERGENCY !");
                    } else {
                    // --- Spreedsheat stream data sending ---
                    
                    // 1. STATE
                    String stateText = "";
                    if (streamState == STATE_SOLAR_ACTIVE) {
                        stateText = "PV";
                    } else {
                        stateText = "Grid"; 
                    }

                    // 2. ZONE
                    String zoneText = "";
                    if (streamPeriod == PROD_TIME) zoneText = "Prod";
                    else if (streamPeriod == TRANS_TIME) zoneText = "Trans";
                    else zoneText = "N-Prod";
                    // -----------------------------------------

                    // 3. Other parameters
                    url += "v=" + String(streamData.voltage, 1);
                    url += "&hz=" + String(streamData.frequency, 2);
                    url += "&w=" + String(streamData.power, 0);
                    url += "&timer=" + String(streamData.anomalyDurationSec, 1);
                    url += "&fuzzy=" + String(streamFuzzy, 2);
                    
                    url += "&state=" + stateText;
                    url += "&zone=" + zoneText;
                }
                
                url.replace(" ", ""); 
                Serial.println(url); // print URL for debugging

                WiFiClientSecure client;
                client.setInsecure(); 
                
                HTTPClient http;
                http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); 
                
                http.begin(client, url);
                int httpCode = http.GET();
                
                if (httpCode > 0) {
                    Serial.printf("[STREAM] Sukses terkirim! HTTP Code: %d\n", httpCode);
                } else {
                    Serial.printf("[STREAM] Gagal! Error: %s\n", http.errorToString(httpCode).c_str());
                }
                http.end();
            }
        }
        
        // Off task after 10 second
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}